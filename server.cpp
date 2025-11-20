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
#include "concurrentqueue.h"
#include "memory_pool.h"

#pragma comment(lib, "ws2_32.lib")

#define PORT 5555
#define BUFFER_SIZE 1024

using moodycamel::ConcurrentQueue;

class PubSub {
    std::unordered_map<std::string, std::vector<SOCKET>> subscribers;
    std::unordered_map<std::string, std::shared_ptr<ConcurrentQueue<std::string>>> topicQueues;
    std::vector<std::thread> workerThreads;
    std::atomic<bool> running{true};
    MemoryPool pool;

public:
    PubSub() {}
    ~PubSub() {
        running = false;
        for (auto &t : workerThreads)
            if (t.joinable()) t.join();
    }

    // ✅ Subscribe a client to a topic
    void subscribe(SOCKET client_fd, const std::string &topic, const std::string &clientName) {
        if (!subscribers.count(topic)) {
            subscribers[topic] = {};
            topicQueues[topic] = std::make_shared<ConcurrentQueue<std::string>>();
            // Start a worker thread for this topic
            workerThreads.emplace_back(&PubSub::broadcastLoop, this, topic, topicQueues[topic]);
        }

        subscribers[topic].push_back(client_fd);
        std::string msg = clientName + " subscribed to " + topic + "\n";
        send(client_fd, msg.c_str(), (int)msg.size(), 0);
        std::cout << clientName << " subscribed to " << topic << std::endl;
    }

    // ✅ Publish a message to all subscribers
    void publish(const std::string &topic, const std::string &message) {
        if (!topicQueues.count(topic)) {
            std::cout << "No subscribers for topic: " << topic << std::endl;
            return;
        }

        // Allocate memory for message using pool
        void *mem = pool.allocate();
        std::string *msgPtr = new (mem) std::string(message);

        topicQueues[topic]->enqueue(*msgPtr);

        // Clean up and recycle block
        msgPtr->~basic_string();
        pool.deallocate(mem);
    }

    // ✅ Helper to check topic existence
    bool topicExists(const std::string &topic) {
        return topicQueues.count(topic) > 0;
    }

    // ✅ Helper to access queue
    std::shared_ptr<ConcurrentQueue<std::string>> getQueue(const std::string &topic) {
        if (topicQueues.count(topic))
            return topicQueues[topic];
        return nullptr;
    }

private:
    // ✅ Worker thread: pushes messages to all subscribers in real-time
    void broadcastLoop(std::string topic, std::shared_ptr<ConcurrentQueue<std::string>> q) {
        while (running) {
            std::string msg;
            if (q->try_dequeue(msg)) {
                std::string fullMessage = "Message on " + topic + ": " + msg + "\n";
                for (auto fd : subscribers[topic]) {
                    send(fd, fullMessage.c_str(), (int)fullMessage.size(), 0);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed\n";
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    if (listen(server_fd, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed\n";
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    std::cout << "🚀 Lock-Free Pub-Sub Server listening on port " << PORT << "...\n";

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_fd, &master_set);
    SOCKET max_fd = server_fd;

    PubSub ps;

    while (true) {
        read_fds = master_set;
        if (select((int)(max_fd + 1), &read_fds, nullptr, nullptr, nullptr) < 0) {
            std::cerr << "select() failed\n";
            break;
        }

        for (SOCKET fd = 0; fd <= max_fd; ++fd) {
            if (FD_ISSET(fd, &read_fds)) {
                if (fd == server_fd) {
                    sockaddr_in client_addr{};
                    int addrlen = sizeof(client_addr);
                    SOCKET new_socket = accept(server_fd, (sockaddr *)&client_addr, &addrlen);
                    if (new_socket != INVALID_SOCKET) {
                        FD_SET(new_socket, &master_set);
                        if (new_socket > max_fd) max_fd = new_socket;
                        std::string ip = inet_ntoa(client_addr.sin_addr);
                        std::cout << "🟢 New connection from " << ip << "\n";
                        std::string welcome = "Welcome to the Lock-Free Pub-Sub Server!\n";
                        send(new_socket, welcome.c_str(), (int)welcome.size(), 0);
                    }
                } else {
                    char buffer[BUFFER_SIZE];
                    int valread = recv(fd, buffer, BUFFER_SIZE - 1, 0);
                    if (valread <= 0) {
                        std::cout << "🔴 Client disconnected (fd=" << fd << ")\n";
                        closesocket(fd);
                        FD_CLR(fd, &master_set);
                    } else {
                        buffer[valread] = '\0';
                        std::string line(buffer);
                        line.erase(remove(line.begin(), line.end(), '\r'), line.end());
                        line.erase(remove(line.begin(), line.end(), '\n'), line.end());

                        std::istringstream iss(line);
                        std::string cmd;
                        iss >> cmd;

                        if (cmd == "SUB") {
                            std::string client, topic;
                            iss >> client >> topic;
                            if (!topic.empty()) ps.subscribe(fd, topic, client);
                        } else if (cmd == "PUB") {
                            std::string topic;
                            iss >> topic;
                            std::string message;
                            std::getline(iss, message);
                            if (!message.empty() && message[0] == ' ')
                                message.erase(0, 1);
                            if (!topic.empty()) ps.publish(topic, message);
                        }
                        // ✅ FETCH command for polling clients
                        else if (cmd == "FETCH") {
                            std::string topic;
                            iss >> topic;

                            if (topic.empty()) {
                                std::string msg = "Topic not provided\n";
                                send(fd, msg.c_str(), (int)msg.size(), 0);
                                continue;
                            }

                            if (!ps.topicExists(topic)) {
                                std::string msg = "No messages for topic: " + topic + "\n";
                                send(fd, msg.c_str(), (int)msg.size(), 0);
                                continue;
                            }

                            std::string msg;
                            auto queue = ps.getQueue(topic);
                            if (queue && queue->try_dequeue(msg)) {
                                std::string full = "Topic " + topic + ": " + msg + "\n";
                                send(fd, full.c_str(), (int)full.size(), 0);
                            } else {
                                std::string none = "(No new messages)\n";
                                send(fd, none.c_str(), (int)none.size(), 0);
                            }
                        } else {
                            std::string msg = "Unknown command\n";
                            send(fd, msg.c_str(), (int)msg.size(), 0);
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
