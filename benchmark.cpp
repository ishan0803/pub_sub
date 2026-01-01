/*
    benchmark.cpp - High-Performance C++ Load Tester
    - Correctly handles EAGAIN via Output Buffering
    - Uses std::deque for pointer stability
    - Optimized String Parsing
    
    Compile: g++ benchmark.cpp -o benchmark -O3
*/

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <deque> // Safer than vector for pointer stability
#include <chrono>
#include <string>

#define SERVER_PORT 5555
#define SERVER_IP "127.0.0.1"
#define MAX_EVENTS 10000

enum State { DISCONNECTED, CONNECTING, SUBSCRIBING, PUBLISHING };

struct Client {
    int fd = -1;
    State state = DISCONNECTED;
    std::string input_buffer; 
    std::string output_buffer; // Write Buffer
    bool epoll_out_active = false;
};

long long total_msgs = 0;
sockaddr_in server_addr;

void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// Update Epoll flags based on buffer state
void update_epoll(Client* c, int epfd) {
    uint32_t events = EPOLLIN | EPOLLET;
    bool needs_out = !c->output_buffer.empty() || c->state == CONNECTING;
    
    if (needs_out) events |= EPOLLOUT;

    // Only make syscall if state changed (Optimization)
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
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Fatal error, let main loop handle via HUP/ERR or next read
        return;
    }

    update_epoll(c, epfd);
}

void queue_message(Client* c, const char* data, size_t len, int epfd) {
    c->output_buffer.append(data, len);
    flush_output(c, epfd);
}

void try_connect(Client* c, int epfd) {
    if (c->fd != -1) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, nullptr);
        close(c->fd);
    }
    
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(c->fd);
    
    connect(c->fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    c->state = CONNECTING;
    c->input_buffer.clear();
    c->output_buffer.clear();
    c->epoll_out_active = true; // We need EPOLLOUT to detect connection completion
    
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
}

void process_line(Client* c, const std::string& line, int epfd) {
    // Optimized State Machine
    if (c->state == CONNECTING) {
        // Fast check for "OK"
        if (line.size() >= 2 && line[0] == 'O' && line[1] == 'K') {
            c->state = SUBSCRIBING;
            char cmd[64]; int len = sprintf(cmd, "SUB u%d t1\n", c->fd);
            queue_message(c, cmd, len, epfd);
        }
    }
    else if (c->state == SUBSCRIBING) {
        // Fast check for "Subscribed"
        if (strncmp(line.c_str(), "Subscribed", 10) == 0) {
            c->state = PUBLISHING;
            queue_message(c, "PUB t1 D\n", 9, epfd);
        }
    }
    else if (c->state == PUBLISHING) {
        // We assume any message here is the echo
        total_msgs++;
        queue_message(c, "PUB t1 D\n", 9, epfd);
    }
}

int main(int argc, char* argv[]) {
    int num_clients = (argc > 1) ? atoi(argv[1]) : 1000;
    std::cout << ">> Benchmark: " << num_clients << " Clients\n";

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    int epfd = epoll_create1(0);
    
    // Use Deque for Pointer Stability
    std::deque<Client> clients(num_clients);
    
    for (int i = 0; i < num_clients; ++i) try_connect(&clients[i], epfd);

    struct epoll_event events[MAX_EVENTS];
    char buf[4096];
    auto last_log = std::chrono::steady_clock::now();

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 50);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >= 1000) {
            static long long last_total = 0;
            std::cout << "\rRate: " << (total_msgs - last_total) << " msg/s | Total: " << total_msgs << "     " << std::flush;
            last_total = total_msgs;
            last_log = now;
        }

        for (int i = 0; i < nfds; ++i) {
            Client* c = (Client*)events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (evs & (EPOLLERR | EPOLLHUP)) { try_connect(c, epfd); continue; }

            // Write Ready (Flush Buffer or Connect)
            if (evs & EPOLLOUT) {
                if (c->state == CONNECTING) {
                    // Check connection
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) try_connect(c, epfd);
                    else {
                        // Connected, waiting for OK. 
                        // IMPORTANT: Disable EPOLLOUT until we have data to write
                        // But wait, our update_epoll logic handles this automatically
                        // based on output_buffer being empty.
                        update_epoll(c, epfd);
                    }
                } else {
                    flush_output(c, epfd);
                }
            }

            // Read Ready
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
}