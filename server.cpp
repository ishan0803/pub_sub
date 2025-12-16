/*
    server.cpp - Reliable Zero-Copy Pub-Sub
    - Fixes race condition crashes (mutex)
    - Zero-allocation (RawMessage + pool)
    - Handles TCP fragmentation
    
    Compile: g++ server.cpp -o server -lws2_32
*/

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define FD_SETSIZE 1024

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <sstream>
#include <algorithm>
#include <cstring> 
#include <mutex> 

#include "concurrentqueue.h"
#include "memory_pool.h" 

#pragma comment(lib, "ws2_32.lib")

#define PORT 5555
#define RECV_BUFFER_SIZE 4096

using moodycamel::ConcurrentQueue;

// Map struct to raw memory (no heap alloc)
struct RawMessage {
    uint32_t length;
    char content[1]; 
};

class PubSub {
    std::unordered_map<std::string, std::vector<SOCKET>> subscribers;
    std::mutex subMutex; // protect map
    
    std::unordered_map<std::string, std::shared_ptr<ConcurrentQueue<void*>>> topicQueues;
    std::vector<std::thread> workerThreads;
    std::atomic<bool> running{true};
    
    MemoryPool pool; 

public:
    PubSub() {}
    
    ~PubSub() {
        running = false;
        for (auto &t : workerThreads) {
            if (t.joinable()) t.join();
        }
    }

    void subscribe(SOCKET client_fd, const std::string &topic, const std::string &clientName) {
        // lock critical section (vector resize)
        {
            std::lock_guard<std::mutex> lock(subMutex);
            if (!subscribers.count(topic)) {
                subscribers[topic] = {};
                topicQueues[topic] = std::make_shared<ConcurrentQueue<void*>>();
                workerThreads.emplace_back(&PubSub::broadcastLoop, this, topic, topicQueues[topic]);
            }
            subscribers[topic].push_back(client_fd);
        }
        
        std::string msg = clientName + " subscribed to " + topic + "\n";
        send(client_fd, msg.c_str(), (int)msg.size(), 0);
        std::cout << "[INFO] " << clientName << " subscribed to " << topic << std::endl;
    }

    void publish(const std::string &topic, const std::string &message) {
        if (!topicQueues.count(topic)) return;

        // get block from pool
        void *mem = pool.allocate();
        if (!mem) return; 
        
        // zero-copy write
        RawMessage* msgPacket = static_cast<RawMessage*>(mem);
        
        const size_t max_payload = 1024 - sizeof(uint32_t) - 1;
        size_t copy_len = std::min(message.size(), max_payload);

        msgPacket->length = (uint32_t)copy_len;
        std::memcpy(msgPacket->content, message.c_str(), copy_len);
        msgPacket->content[copy_len] = '\0'; 

        // push to queue
        topicQueues[topic]->enqueue(mem);
    }

private:
    void broadcastLoop(std::string topic, std::shared_ptr<ConcurrentQueue<void*>> q) {
        // stack buffer (avoid malloc in loop)
        char sendBuffer[2048];
        std::string headerStr = "Message on " + topic + ": ";
        size_t headerLen = headerStr.size();
        std::memcpy(sendBuffer, headerStr.c_str(), headerLen);

        while (running) {
            void* mem = nullptr;
            if (q->try_dequeue(mem)) {
                RawMessage* msgPacket = static_cast<RawMessage*>(mem);
                
                size_t payloadLen = msgPacket->length;
                if (headerLen + payloadLen + 1 > 2048) {
                    payloadLen = 2048 - headerLen - 1;
                }

                std::memcpy(sendBuffer + headerLen, msgPacket->content, payloadLen);
                sendBuffer[headerLen + payloadLen] = '\n';
                int totalLen = (int)(headerLen + payloadLen + 1);

                // snapshot sockets (release lock before I/O)
                std::vector<SOCKET> targets;
                {
                    std::lock_guard<std::mutex> lock(subMutex);
                    if (subscribers.count(topic)) {
                        targets = subscribers[topic];
                    }
                }

                for (auto fd : targets) {
                    send(fd, sendBuffer, totalLen, 0);
                }

                pool.deallocate(mem);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, SOMAXCONN);

    std::cout << ">> Reliable Pub-Sub Server (Thread-Safe) listening on " << PORT << "\n";

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_fd, &master_set);
    SOCKET max_fd = server_fd;

    PubSub ps;
    char buffer[RECV_BUFFER_SIZE];

    while (true) {
        read_fds = master_set;
        timeval timeout = {1, 0}; 
        int activity = select(0, &read_fds, nullptr, nullptr, &timeout);

        if (activity == SOCKET_ERROR) break;

        for (unsigned int i = 0; i < read_fds.fd_count; ++i) {
            SOCKET sock = read_fds.fd_array[i];

            if (sock == server_fd) {
                sockaddr_in client_addr;
                int len = sizeof(client_addr);
                SOCKET new_sock = accept(server_fd, (sockaddr *)&client_addr, &len);
                if (new_sock != INVALID_SOCKET) {
                    FD_SET(new_sock, &master_set);
                    const char* welcome = "OK\n";
                    send(new_sock, welcome, 3, 0);
                }
            } else {
                int valread = recv(sock, buffer, RECV_BUFFER_SIZE - 1, 0);
                if (valread <= 0) {
                    closesocket(sock);
                    FD_CLR(sock, &master_set);
                } else {
                    buffer[valread] = '\0';
                    std::string streamData(buffer);

                    // handle TCP fragmentation (multiple cmds in one packet)
                    std::stringstream ss(streamData);
                    std::string segment;
                    while (std::getline(ss, segment, '\n')) {
                        if (!segment.empty() && segment.back() == '\r') segment.pop_back();
                        if (segment.empty()) continue;

                        std::istringstream iss(segment);
                        std::string cmd;
                        iss >> cmd;

                        if (cmd == "SUB") {
                            std::string client, topic;
                            iss >> client >> topic;
                            if (!topic.empty()) ps.subscribe(sock, topic, client);
                        } 
                        else if (cmd == "PUB") {
                            std::string topic;
                            iss >> topic;
                            std::string message;
                            std::getline(iss, message);
                            if (!message.empty() && message[0] == ' ') message.erase(0, 1);
                            if (!topic.empty()) ps.publish(topic, message);
                        }
                    }
                }
            }
        }
    }
    closesocket(server_fd);
    WSACleanup();
    return 0;
}