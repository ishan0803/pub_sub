/*
    benchmark.cpp - Load Testing Tool for Pub-Sub Server
    Compile with: g++ benchmark.cpp -o benchmark -lws2_32
    (On Windows using MinGW or MSVC)
*/

#define FD_SETSIZE 1024 // Increase select() limit for the benchmark tool itself
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <chrono>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

// Configuration
const std::string SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 5555;
const int TOTAL_CLIENTS = 1000;      // Defaulting to 1k for safety. 
const int CLIENTS_PER_THREAD = 50;   // How many sockets one thread manages
const int NUM_MESSAGES = 10;         // Messages per client to send

// Statistics
std::atomic<int> connected_clients(0);
std::atomic<int> failed_connections(0);
std::atomic<long long> total_latency_ms(0);
std::atomic<int> messages_sent(0);
std::atomic<int> messages_received(0);

std::mutex log_mutex;

void log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << msg << std::endl;
}

// Simple helper to receive exactly N bytes or until delimiter (not fully robust for high perf, but sufficient for bench)
bool receive_response(SOCKET sock, std::string& buffer) {
    char buf[1024];
    int bytes = recv(sock, buf, sizeof(buf) - 1, 0);
    if (bytes > 0) {
        buf[bytes] = '\0';
        buffer = std::string(buf);
        return true;
    }
    return false;
}

void client_worker(int client_count, int start_id) {
    std::vector<SOCKET> sockets;
    sockets.reserve(client_count);

    // 1. Connection Phase
    for (int i = 0; i < client_count; ++i) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            failed_connections++;
            continue;
        }

        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr);

        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            closesocket(sock);
            failed_connections++;
            continue;
        }

        // Consume "Welcome" message
        std::string response;
        receive_response(sock, response);

        sockets.push_back(sock);
        connected_clients++;
    }

    // 2. Subscription Phase
    for (size_t i = 0; i < sockets.size(); ++i) {
        std::string name = "User" + std::to_string(start_id + i);
        std::string topic = "general"; 
        std::string cmd = "SUB " + name + " " + topic + "\n";
        
        send(sockets[i], cmd.c_str(), (int)cmd.length(), 0);
        
        std::string response;
        receive_response(sockets[i], response); // Consume subscription confirmation
    }

    // 3. Publish/Benchmark Phase
    // To simulate 100k users without 100k threads, we iterate through our managed sockets
    for (int m = 0; m < NUM_MESSAGES; ++m) {
        for (size_t i = 0; i < sockets.size(); ++i) {
            auto start = std::chrono::high_resolution_clock::now();

            std::string msg = "TestMsg_" + std::to_string(m);
            std::string cmd = "PUB general " + msg + "\n";

            int sent = send(sockets[i], cmd.c_str(), (int)cmd.length(), 0);
            if (sent > 0) {
                messages_sent++;
                
                // Wait for the echo back (since we are subscribed to the topic we published to)
                std::string response;
                if (receive_response(sockets[i], response)) {
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                    total_latency_ms += duration;
                    messages_received++;
                }
            }
        }
    }

    // Cleanup
    for (SOCKET s : sockets) {
        closesocket(s);
    }
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "   Pub-Sub Server Benchmark Tool" << std::endl;
    std::cout << "   Target: " << SERVER_IP << ":" << SERVER_PORT << std::endl;
    std::cout << "   Target Clients: " << TOTAL_CLIENTS << std::endl;
    std::cout << "========================================" << std::endl;

    // Calculate threads needed
    int num_threads = (TOTAL_CLIENTS + CLIENTS_PER_THREAD - 1) / CLIENTS_PER_THREAD;
    std::vector<std::thread> threads;

    auto bench_start = std::chrono::high_resolution_clock::now();

    // Launch Workers
    for (int i = 0; i < num_threads; ++i) {
        int count = std::min(CLIENTS_PER_THREAD, TOTAL_CLIENTS - (i * CLIENTS_PER_THREAD));
        threads.emplace_back(client_worker, count, i * CLIENTS_PER_THREAD);
        // Small delay to prevent SYN flood triggering DDoS protection on localhost
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
    }

    std::cout << "All threads launched. Waiting for completion..." << std::endl;

    // Wait for completion
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = bench_end - bench_start;

    // Results
    std::cout << "\n========================================" << std::endl;
    std::cout << "   Benchmark Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Time Elapsed:       " << elapsed.count() << " seconds" << std::endl;
    std::cout << "Connected Clients:  " << connected_clients << " / " << TOTAL_CLIENTS << std::endl;
    std::cout << "Failed Connections: " << failed_connections << std::endl;
    std::cout << "Messages Sent:      " << messages_sent << std::endl;
    std::cout << "Messages Received:  " << messages_received << std::endl;
    
    if (messages_received > 0) {
        double avg_latency = (double)total_latency_ms / messages_received;
        double throughput = messages_received / elapsed.count();
        std::cout << "Avg Latency:        " << avg_latency << " ms" << std::endl;
        std::cout << "Throughput:         " << throughput << " msgs/sec" << std::endl;
    } else {
        std::cout << "Avg Latency:        N/A" << std::endl;
    }

    WSACleanup();
    return 0;
}