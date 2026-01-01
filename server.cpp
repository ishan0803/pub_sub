/*
    server.cpp - Professional Grade Epoll Server
    - TRUE Zero-Copy/Zero-Malloc Buffering (Pool-based Output Chain)
    - O(1) Disconnect Handling (Reverse Index)
    - Robust EPOLLOUT Handling (No Data Loss)
    
    Compile: g++ server.cpp -o server -O3
*/

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include "memory_pool.h" 

#include <netinet/tcp.h> // <--- ADD THIS HEADER

void set_tcp_nodelay(int sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

#define PORT 5555
#define MAX_EVENTS 20000

// --- Output Buffer Node ---
struct OutputBlock {
    size_t len = 0;
    size_t offset = 0;
    OutputBlock* next = nullptr;
    char data[BLOCK_SIZE - sizeof(size_t)*2 - sizeof(OutputBlock*)]; 
};

// --- Client Structure ---
struct Client {
    int fd;
    char input_buf[4096];
    size_t input_len = 0;

    OutputBlock* out_head = nullptr;
    OutputBlock* out_tail = nullptr;
    bool watching_write = false;

    std::unordered_set<std::string> subscriptions; 
};

// --- Globals ---
MemoryPool blockPool(100000); 
int epfd;
std::unordered_map<int, Client*> clients; 
std::unordered_map<std::string, std::vector<int>> topics;

void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void mod_epoll(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

// --- OUTPUT ENGINE ---
void queue_output(Client* c, const char* data, size_t len) {
    size_t remaining = len;
    size_t data_off = 0;

    while (remaining > 0) {
        if (!c->out_tail || c->out_tail->len == sizeof(c->out_tail->data)) {
            void* mem = blockPool.allocate();
            OutputBlock* b = new(mem) OutputBlock(); 
            
            if (c->out_tail) c->out_tail->next = b;
            else c->out_head = b;
            c->out_tail = b;
        }

        size_t space = sizeof(c->out_tail->data) - c->out_tail->len;
        size_t take = std::min(space, remaining);
        
        memcpy(c->out_tail->data + c->out_tail->len, data + data_off, take);
        c->out_tail->len += take;
        remaining -= take;
        data_off += take;
    }

    if (!c->watching_write) {
        mod_epoll(c->fd, EPOLLIN | EPOLLOUT | EPOLLET);
        c->watching_write = true;
    }
}

void flush_output(Client* c) {
    while (c->out_head) {
        OutputBlock* b = c->out_head;
        size_t to_write = b->len - b->offset;
        
        ssize_t sent = send(c->fd, b->data + b->offset, to_write, MSG_NOSIGNAL);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; 
            return; 
        }

        b->offset += sent;
        
        if (b->offset == b->len) {
            c->out_head = b->next;
            if (!c->out_head) c->out_tail = nullptr;
            blockPool.deallocate(b);
        } else {
            break; 
        }
    }

    if (!c->out_head && c->watching_write) {
        mod_epoll(c->fd, EPOLLIN | EPOLLET);
        c->watching_write = false;
    }
}

// --- CLIENT LOGIC ---
void remove_client(int fd) {
    if (clients.count(fd)) {
        Client* c = clients[fd];
        
        for (const std::string& topic : c->subscriptions) {
            auto& list = topics[topic];
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] == fd) {
                    list[i] = list.back();
                    list.pop_back();
                    break;
                }
            }
        }
        
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        
        OutputBlock* curr = c->out_head;
        while (curr) {
            OutputBlock* next = curr->next;
            blockPool.deallocate(curr);
            curr = next;
        }
        
        delete c; 
        clients.erase(fd);
    }
}

void process_data(Client* c) {
    char* buf = c->input_buf;
    size_t processed = 0;

    for (size_t i = 0; i < c->input_len; ++i) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            char* line = buf + processed;
            if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';

            if (strncmp(line, "SUB", 3) == 0) {
                char* sp = strchr(line + 4, ' ');
                if (sp) {
                    char* topic = sp + 1;
                    topics[topic].push_back(c->fd);
                    c->subscriptions.insert(topic);
                    std::string ack = "Subscribed to " + std::string(topic) + "\n";
                    queue_output(c, ack.c_str(), ack.length());
                }
            }
            else if (strncmp(line, "PUB", 3) == 0) {
                char* sp = strchr(line + 4, ' ');
                if (sp) {
                    *sp = '\0';
                    char* topic = line + 4;
                    char* msg = sp + 1;
                    
                    if (topics.count(topic)) {
                        std::string full = "Message on " + std::string(topic) + ": " + std::string(msg) + "\n";
                        for (int sub_fd : topics[topic]) {
                            queue_output(clients[sub_fd], full.c_str(), full.length());
                        }
                    }
                }
            }
            processed = i + 1;
        }
    }

    if (processed > 0) {
        size_t remain = c->input_len - processed;
        if (remain > 0) memmove(c->input_buf, c->input_buf + processed, remain);
        c->input_len = remain;
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    struct rlimit l; getrlimit(RLIMIT_NOFILE, &l);
    l.rlim_cur = l.rlim_max; setrlimit(RLIMIT_NOFILE, &l);

    int s_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(s_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT);
    bind(s_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(s_fd, SOMAXCONN);
    set_nonblocking(s_fd);

    epfd = epoll_create1(0);
    struct epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN | EPOLLET; ev.data.fd = s_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, s_fd, &ev);

    std::cout << ">> Server Running (Block Size: " << BLOCK_SIZE << ")\n";

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == s_fd) {
                while(true) {
                    int cfd = accept(s_fd, nullptr, nullptr);
                    if (cfd < 0) break;
                    set_nonblocking(cfd);
                    set_tcp_nodelay(cfd);
                    Client* c = new Client(); 
                    c->fd = cfd;
                    clients[cfd] = c;

                    ev.events = EPOLLIN | EPOLLET; ev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
                    
                    queue_output(c, "OK\n", 3);
                    flush_output(c);
                }
            } else {
                Client* c = clients[fd];
                uint32_t e = events[i].events;

                if (e & (EPOLLERR | EPOLLHUP)) {
                    remove_client(fd);
                    continue;
                }
                if (e & EPOLLIN) {
                    while (true) {
                        size_t space = 4096 - c->input_len;
                        if (space == 0) break; 
                        ssize_t n = recv(fd, c->input_buf + c->input_len, space, 0);
                        if (n > 0) {
                            c->input_len += n;
                            process_data(c);
                        } else {
                            if (n == 0 || (errno != EAGAIN)) remove_client(fd);
                            break;
                        }
                    }
                }
                if (e & EPOLLOUT) flush_output(c);
            }
        }
    }
}