/*
    server_sharded.cpp - "Share Nothing" HFT Architecture
    - Sharded Workers based on Topic Hash
    - ZERO Locks on the Hot Path (No Mutexes)
    - True Parallelism: N Workers = N Independent Engines
    
    Compile: g++ server_sharded.cpp -o server_sharded -O3 -lpthread
*/

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

#include "concurrentqueue.h"
#include "memory_pool.h"

#define PORT 5555
#define MAX_EVENTS 10000
#define NUM_IO_THREADS 4      // Adjust to available cores (e.g., 2)
#define NUM_WORKER_THREADS 10  // Adjust to available cores (e.g., 4)

// --- Hashing for Sharding ---
// FNV-1a Hash (Fast and good distribution for strings)
inline uint32_t hash_topic(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 16777619u;
    }
    return hash;
}

// --- Task Definition ---
enum TaskType { CMD_SUB, CMD_PUB, CMD_DISCONNECT };

struct Task {
    TaskType type;
    int client_fd;
    char topic[64];
    char payload[1024];
};

MemoryPool taskPool(500000); 
std::atomic<bool> running{true};

// --- Sharded Queues ---
// Each worker has its OWN queue. IO threads push to specific queues.
std::vector<moodycamel::ConcurrentQueue<Task*>> workerQueues(NUM_WORKER_THREADS);

// --- Network Utils ---
void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void set_tcp_nodelay(int sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

// --- WORKER THREAD (Consumer) ---
void worker_loop(int id) {
    printf(">> Worker %d: Started (Owns Shard %d)\n", id, id);
    
    // THREAD LOCAL STATE (No Locks Needed!)
    std::unordered_map<std::string, std::unordered_set<int>> localTopics;
    
    Task* task;
    char broadcast_buf[2048]; // Stack buffer

    while (running) {
        // Only consume from MY queue
        if (workerQueues[id].try_dequeue(task)) {
            
            if (task->type == CMD_SUB) {
                // No Mutex! Only I touch this map.
                localTopics[task->topic].insert(task->client_fd);
                
                int len = snprintf(broadcast_buf, sizeof(broadcast_buf), 
                                   "Subscribed to %s\n", task->topic);
                send(task->client_fd, broadcast_buf, len, MSG_NOSIGNAL);
            }
            else if (task->type == CMD_PUB) {
                if (localTopics.count(task->topic)) {
                    int len = snprintf(broadcast_buf, sizeof(broadcast_buf), 
                                       "Message on %s: %s\n", task->topic, task->payload);
                    
                    auto& subs = localTopics[task->topic];
                    
                    for (auto it = subs.begin(); it != subs.end(); ) {
                         int sub_fd = *it;
                         ssize_t sent = send(sub_fd, broadcast_buf, len, MSG_NOSIGNAL);
                         
                         // Slow Consumer Disconnect
                         if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                             close(sub_fd);
                             it = subs.erase(it);
                         } else {
                             ++it;
                         }
                    }
                }
            }
            else if (task->type == CMD_DISCONNECT) {
                // We don't know which topics the client was on, so we scan.
                // Since this is thread-local, it's fast (cache hot).
                for (auto& pair : localTopics) {
                    pair.second.erase(task->client_fd);
                }
                // Note: We do NOT close(fd) here, IO thread or first worker handles it. 
                // In this architecture, it's safer if IO thread closes FD after broadcasting disconnect.
            }

            taskPool.deallocate(task);
        } else {
            // Spin/Yield hybrid for low latency
            std::this_thread::yield();
        }
    }
}

// --- IO THREAD (Producer) ---
struct ClientBuffer {
    int fd;
    char buffer[4096];
    size_t len = 0;
};

void process_buffer(ClientBuffer* c) {
    char* buf = c->buffer;
    size_t processed = 0;

    for (size_t i = 0; i < c->len; ++i) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            char* line = buf + processed;
            if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';

            bool valid = false;
            TaskType t_type;
            char t_topic[64] = {0};
            char t_payload[1024] = {0};

            if (strncmp(line, "SUB", 3) == 0) {
                char* sp = strchr(line + 4, ' ');
                if (sp) {
                    t_type = CMD_SUB;
                    strncpy(t_topic, sp + 1, 63);
                    valid = true;
                }
            }
            else if (strncmp(line, "PUB", 3) == 0) {
                char* sp = strchr(line + 4, ' ');
                if (sp) {
                    *sp = '\0';
                    t_type = CMD_PUB;
                    strncpy(t_topic, line + 4, 63);
                    strncpy(t_payload, sp + 1, 1023);
                    valid = true;
                }
            }

            if (valid) {
                Task* t = (Task*)taskPool.allocate();
                t->type = t_type;
                t->client_fd = c->fd;
                strcpy(t->topic, t_topic);
                if(t_type == CMD_PUB) strcpy(t->payload, t_payload);

                // KEY: Hash Topic to find Worker ID
                uint32_t worker_id = hash_topic(t->topic) % NUM_WORKER_THREADS;
                
                // Enqueue to SPECIFIC worker
                workerQueues[worker_id].enqueue(t);
            }

            processed = i + 1;
        }
    }

    if (processed > 0) {
        size_t rem = c->len - processed;
        if (rem > 0) memmove(c->buffer, c->buffer + processed, rem);
        c->len = rem;
    }
}

void io_loop(int id) {
    printf(">> IO Thread %d: Started\n", id);

    int s_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; 
    setsockopt(s_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(s_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)); 
    set_tcp_nodelay(s_fd);

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT);
    bind(s_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(s_fd, SOMAXCONN);
    set_nonblocking(s_fd);

    int epfd = epoll_create1(0);
    struct epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN | EPOLLET; ev.data.fd = s_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, s_fd, &ev);

    std::unordered_map<int, ClientBuffer*> local_clients;

    while (running) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == s_fd) {
                while (true) {
                    int cfd = accept(s_fd, nullptr, nullptr);
                    if (cfd < 0) break;
                    set_nonblocking(cfd);
                    set_tcp_nodelay(cfd);
                    
                    ClientBuffer* cb = new ClientBuffer();
                    cb->fd = cfd;
                    local_clients[cfd] = cb;

                    ev.events = EPOLLIN | EPOLLET; ev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
                    
                    send(cfd, "OK\n", 3, MSG_NOSIGNAL);
                }
            } else {
                if (events[i].events & EPOLLIN) {
                    if (local_clients.count(fd)) {
                         ClientBuffer* cb = local_clients[fd];
                         while (true) {
                            size_t space = 4096 - cb->len;
                            if (space == 0) {
                                // Buffer Full -> Disconnect
                                // Broadcast disconnect to ALL workers (since we don't know who owns this client)
                                for(int w=0; w<NUM_WORKER_THREADS; w++) {
                                    Task* t = (Task*)taskPool.allocate();
                                    t->type = CMD_DISCONNECT; t->client_fd = fd;
                                    workerQueues[w].enqueue(t);
                                }
                                close(fd);
                                delete local_clients[fd]; local_clients.erase(fd);
                                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                                break;
                            }

                            ssize_t n = recv(fd, cb->buffer + cb->len, space, 0);
                            if (n > 0) {
                                cb->len += n;
                                process_buffer(cb);
                            } else {
                                if (n == 0 || errno != EAGAIN) {
                                    // Disconnect broadcast
                                    for(int w=0; w<NUM_WORKER_THREADS; w++) {
                                        Task* t = (Task*)taskPool.allocate();
                                        t->type = CMD_DISCONNECT; t->client_fd = fd;
                                        workerQueues[w].enqueue(t);
                                    }
                                    close(fd); // IO Thread owns the socket close
                                    delete local_clients[fd]; local_clients.erase(fd);
                                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                                }
                                break;
                            }
                         }
                    }
                }
            }
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    struct rlimit l; getrlimit(RLIMIT_NOFILE, &l);
    l.rlim_cur = l.rlim_max; setrlimit(RLIMIT_NOFILE, &l);

    std::vector<std::thread> io_threads;
    std::vector<std::thread> worker_threads;

    // Launch Workers (Consumers)
    for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
        worker_threads.emplace_back(worker_loop, i);
    }

    // Launch IO (Producers)
    for (int i = 0; i < NUM_IO_THREADS; ++i) {
        io_threads.emplace_back(io_loop, i);
    }

    for (auto& t : io_threads) t.join();
    for (auto& t : worker_threads) t.join();

    return 0;
}