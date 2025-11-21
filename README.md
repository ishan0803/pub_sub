# High-Performance Lock-Free Pub-Sub System: Architecture and Performance Analysis

## Abstract
This project implements a low-latency Publish-Subscribe (Pub-Sub) messaging system designed to minimize thread contention and memory allocation overhead. The core architecture utilizes a hybrid model: a `select()`-based network layer for handling concurrent TCP connections, integrated with a lock-free internal message queue (`moodycamel::ConcurrentQueue`) and a custom slab-based memory pool. This document details the system architecture, experimental methodology, and a quantitative analysis of performance benchmarks conducted with 1,000 concurrent clients.

## 1. System Architecture

The server is built on C++ (Windows/Winsock) and prioritizes low-latency message dispatching through three key architectural components:

### 1.1 Network I/O Layer
The network layer utilizes the `select()` system call to multiplex I/O operations across multiple socket descriptors. While `select()` is traditionally O(N), it serves as a baseline concurrency model for managing connection states (Connect, Subscribe, Publish).
* **Connection Handling:** A single thread manages the `select` loop, accepting incoming connections and routing protocol commands (`SUB`, `PUB`).
* **Protocol:** A lightweight text-based protocol is used, delimited by newlines, ensuring minimal parsing overhead.

### 1.2 Lock-Free Internal Messaging
To decouple network I/O from message broadcasting, the system employs the `moodycamel::ConcurrentQueue`, a multi-producer, multi-consumer lock-free queue.
* **Topic Queues:** Each topic is assigned a dedicated `ConcurrentQueue<std::string>`, allowing publishers to enqueue messages without acquiring mutex locks.
* **Worker Threads:** Dedicated worker threads consume from these queues and broadcast messages to subscribers, ensuring that heavy broadcasting logic does not block the main network loop.

### 1.3 Zero-Overhead Memory Management
To prevent heap fragmentation and the latency spikes associated with `new`/`delete` during high-throughput bursts, the system utilizes a custom `MemoryPool`.
* **Mechanism:** The pool pre-allocates fixed-size blocks (1KB) and manages them using a lock-free queue for rapid allocation and deallocation.
* **Impact:** This ensures O(1) memory retrieval time for incoming messages.

## 2. Experimental Methodology

### 2.1 Benchmark Configuration
The system was stress-tested using a custom C++ benchmarking tool (`benchmark.cpp`) designed to simulate concurrent client behavior.
* **Environment:** Localhost (Loopback interface).
* **Concurrency:** 1,000 concurrent persistent TCP connections.
* **Workload:**
    * **Phase 1:** Connection Establishment (Mass connect).
    * **Phase 2:** Subscription (All clients subscribe to a single topic "general").
    * **Phase 3:** Publish/Ack Cycle (Clients publish messages and wait for echo confirmation).

### 2.2 Metrics
* **Throughput:** Defined as the number of messages successfully published and acknowledged per second.
* **Latency:** The round-trip time (RTT) from publishing a message to receiving the broadcast confirmation.

## 3. Results and Analysis

Three independent trials were conducted with varying loads. The results demonstrate sub-millisecond latency characteristics typical of lock-free architectures.

### 3.1 Performance Data

| Trial | Clients | Messages Sent | Duration (s) | Throughput (msg/sec) | Avg Latency (ms) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | 1,000 | 100,000 | 4.93 | **20,276** | **0.78 ms** |
| **2** | 500 | 5,000 | 0.36 | **14,082** | **0.35 ms** |
| **3** | 1,000 | 10,000 | 0.41 | **24,441** | **0.09 ms** |

### 3.2 Latency Analysis
The system achieved an ultra-low latency floor of **0.089 ms** (Trial 3). This performance is attributed to the `ConcurrentQueue` integration; by removing lock contention on the critical path of message enqueueing, the server processes requests immediately upon receipt. Even under the sustained load of 100,000 messages (Trial 1), latency remained stable at **0.78 ms**, indicating minimal queuing delay.

### 3.3 Throughput Analysis
The observed throughput peaked at **~24,441 messages/second**. It is important to note that this figure reflects the limits of the *synchronous benchmarking client* rather than the server's saturation point.
* The benchmark operates on a "Send -> Wait for Reply" cycle.
* With a latency of ~0.09ms, a single thread is theoretically capped at ~11,000 requests/second.
* The server successfully handled the aggregate throughput of all client threads without saturating the CPU, as evidenced by the 0.0 failure rate across all trials.

## 4. Conclusion

The initial implementation of the Pub-Sub server demonstrates that integrating lock-free data structures with standard `select()` I/O yields high-performance results for moderate concurrency levels (up to 1,000 clients). The system successfully maintains sub-millisecond latency (<1ms) under load.

For scaling beyond 10,000+ concurrent users, the `select()` O(N) polling mechanism will eventually become a bottleneck. Future iterations will transition the network layer to **I/O Completion Ports (IOCP)** to maintain these latency characteristics at a massive scale.

## 5. References
1.  **Server Implementation**: `server.cpp` - Core networking logic using `select` and worker threads.
2.  **Lock-Free Queue**: `concurrentqueue.h` - `moodycamel::ConcurrentQueue` implementation.
3.  **Memory Management**: `memory_pool.h` - Custom slab allocator.