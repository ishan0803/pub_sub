/*
    benchmark_telemetry.cpp - High-Performance Latency & Throughput Analyzer
    - Measures RTT (Round Trip Time) using payload embedding
    - Calculates p50, p95, p99, Max Latency
    - Robust handling of Non-blocking I/O
    
    Compile: g++ benchmark_telemetry.cpp -o bench_telemetry -O3
*/

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <deque>
#include <chrono>
#include <string>
#include <algorithm>
#include <iomanip>
#include <numeric>

#include <netinet/tcp.h> // <--- ADD THIS HEADER

void set_tcp_nodelay(int sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

#define SERVER_PORT 5555
#define SERVER_IP "192.168.1.9"
#define MAX_EVENTS 10000

// --- Statistics Engine ---
struct Stats {
    std::vector<double> latencies; // Microseconds
    long long total_msgs = 0;
    
    void record(long long start_ns, long long end_ns) {
        double us = (end_ns - start_ns) / 1000.0;
        latencies.push_back(us);
        total_msgs++;
    }

    void print_and_reset(double elapsed_sec, int active_clients) {
        if (latencies.empty()) {
            std::cout << "\r[Waiting for data...] Active: " << active_clients << std::flush;
            return;
        }

        // Sort for percentiles
        std::sort(latencies.begin(), latencies.end());
        
        double min_lat = latencies.front();
        double max_lat = latencies.back();
        double avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
        double p50 = latencies[latencies.size() * 0.50];
        double p95 = latencies[latencies.size() * 0.95];
        double p99 = latencies[latencies.size() * 0.99];
        
        double throughput = latencies.size() / elapsed_sec;

        // Print Table Row
        printf("\rRate: %7.0f/s | Latency (us): Avg %5.1f | p50 %5.1f | p95 %5.1f | p99 %6.1f | Max %6.1f | Clients: %d   ",
               throughput, avg_lat, p50, p95, p99, max_lat, active_clients);
        fflush(stdout);

        // Reset
        latencies.clear();
    }
};

Stats global_stats;

// --- Networking ---
enum State { DISCONNECTED, CONNECTING, SUBSCRIBING, PUBLISHING };

struct Client {
    int fd = -1;
    State state = DISCONNECTED;
    std::string input_buffer;
    std::string output_buffer;
    bool epoll_out_active = false;
    std::string my_topic;
};

sockaddr_in server_addr;

long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void update_epoll(Client* c, int epfd) {
    uint32_t events = EPOLLIN | EPOLLET;
    bool needs_out = !c->output_buffer.empty() || c->state == CONNECTING;
    if (needs_out) events |= EPOLLOUT;

    if (needs_out != c->epoll_out_active) {
        struct epoll_event ev;
        ev.events = events;
        ev.data.ptr = c;
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->epoll_out_active = needs_out;
    }
}

void flush_output(Client* c, int epfd) {
    if (c->output_buffer.empty()) return;

    ssize_t n = send(c->fd, c->output_buffer.data(), c->output_buffer.size(), MSG_NOSIGNAL);
    if (n > 0) {
        c->output_buffer.erase(0, n);
    } 
    update_epoll(c, epfd);
}

void queue_data(Client* c, const std::string& data, int epfd) {
    c->output_buffer.append(data);
    flush_output(c, epfd);
}

void send_pub(Client* c, int epfd) {
    // Protocol: PUB <topic> <timestamp_ns>
    long long ts = now_ns();
    std::string msg = "PUB " + c->my_topic + " " + std::to_string(ts) + "\n";
    queue_data(c, msg, epfd);
}

void try_connect(Client* c, int epfd) {
    if (c->fd != -1) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, nullptr);
        close(c->fd);
    }
    
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(c->fd);
    set_tcp_nodelay(c->fd);

    connect(c->fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    c->state = CONNECTING;
    c->input_buffer.clear();
    c->output_buffer.clear();
    c->epoll_out_active = true;
    
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
}

void process_line(Client* c, const std::string& line, int epfd) {
    if (c->state == CONNECTING) {
        // Wait for "OK"
        if (line.size() >= 2 && line[0] == 'O') {
            c->state = SUBSCRIBING;
            // Topic is unique to FD to avoid broadcast storms during latency testing
            c->my_topic = "t" + std::to_string(c->fd);
            std::string cmd = "SUB u" + std::to_string(c->fd) + " " + c->my_topic + "\n";
            queue_data(c, cmd, epfd);
        }
    }
    else if (c->state == SUBSCRIBING) {
        if (line.find("Subscribed") != std::string::npos) {
            c->state = PUBLISHING;
            send_pub(c, epfd); // Send first message
        }
    }
    else if (c->state == PUBLISHING) {
        // Format: "Message on <topic>: <timestamp>"
        // Find the colon and parse the timestamp
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos && line.size() > colon_pos + 2) {
            try {
                long long sent_time = std::stoll(line.substr(colon_pos + 2));
                global_stats.record(sent_time, now_ns());
            } catch (...) {}
        }
        
        // Pipelining: Send next message immediately
        send_pub(c, epfd);
    }
}

int main(int argc, char* argv[]) {
    int num_clients = (argc > 1) ? atoi(argv[1]) : 1000;
    
    std::cout << "========================================================================\n";
    std::cout << "   Latency & Throughput Telemetry\n";
    std::cout << "   Clients: " << num_clients << " (Unique Topics)\n";
    std::cout << "========================================================================\n";

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    int epfd = epoll_create1(0);
    std::deque<Client> clients(num_clients); // Deque for pointer stability

    for (int i = 0; i < num_clients; ++i) try_connect(&clients[i], epfd);

    struct epoll_event events[MAX_EVENTS];
    char buf[4096];
    auto last_log = std::chrono::steady_clock::now();

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 50);

        // --- Reporting ---
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_log;
        if (elapsed.count() >= 1.0) {
            int active = 0;
            for(const auto& c : clients) if (c.state == PUBLISHING) active++;
            
            global_stats.print_and_reset(elapsed.count(), active);
            last_log = now;
        }

        // --- Event Loop ---
        for (int i = 0; i < nfds; ++i) {
            Client* c = (Client*)events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (evs & (EPOLLERR | EPOLLHUP)) {
                try_connect(c, epfd);
                continue;
            }

            if (evs & EPOLLOUT) {
                if (c->state == CONNECTING) {
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) try_connect(c, epfd);
                    else update_epoll(c, epfd); // Connected, disable OUT, wait for IN (OK)
                } else {
                    flush_output(c, epfd);
                }
            }

            if (evs & EPOLLIN) {
                while(true) {
                    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        c->input_buffer.append(buf, n);
                        size_t pos;
                        while ((pos = c->input_buffer.find('\n')) != std::string::npos) {
                            std::string line = c->input_buffer.substr(0, pos);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            process_line(c, line, epfd);
                            c->input_buffer.erase(0, pos + 1);
                        }
                    } else {
                        if (n == 0 || errno != EAGAIN) try_connect(c, epfd);
                        break; 
                    }
                }
            }
        }
    }
    return 0;
}