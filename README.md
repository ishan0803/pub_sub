
# High-Frequency Publish–Subscribe Model

A high-performance publish–subscribe messaging system optimized for low-latency, high-throughput message delivery. The implementation prioritizes per-message latency and throughput over feature completeness.

## Overview

- Purpose: a minimal, efficient pub-sub server designed for high-frequency trading, telemetry, and real-time event distribution.
- Core implementation:
  - `server.cpp` – multi-threaded server with separate IO and worker threads, lock-free queues, and per-topic sharding for minimal lock contention.
  - `memory_pool.h` – fixed-size, lock-free memory pool that eliminates malloc/free overhead on the critical path.

Target environment: Intel i7-12650H (12th Gen), 10 cores / 16 threads, Ubuntu on WSL2, compiled with g++ `-O2 -march=native`.

## Server Architecture (server.cpp)

The server uses a share-nothing, sharded design to minimize contention:

- **IO threads** (`NUM_IO_THREADS = 4`): accept connections via `SO_REUSEPORT` and perform non-blocking reads with `epoll` edge-triggered mode.
- **Worker threads** (`NUM_WORKER_THREADS = 10`): handle topic operations and message delivery. IO threads parse commands and enqueue `Task` objects to lock-free queues via deterministic topic hashing (FNV-1a).
- **Per-topic sharding**: all operations for a given topic are routed to a single worker, eliminating locks on subscriber sets. Each worker maintains thread-local `localTopics` maps.
- **Aggressive pruning**: slow or unresponsive subscribers are closed immediately to prevent head-of-line blocking.

Design trade-offs:

- **Sharding improves throughput** but introduces risk of load imbalance if topic subscription is skewed.
- **Edge-triggered epoll** reduces syscalls but requires careful non-blocking logic and full drain on EPOLLIN events.
- **Kernel-level load balancing** via `SO_REUSEPORT` distributes accepts across IO threads without user-space logic.

## Memory Management (memory_pool.h)

A lock-free fixed-size memory pool eliminates malloc/free syscall overhead:

- **Fixed block size** (`BLOCK_SIZE = 4096`) pre-allocated on startup.
- **Treiber-style free-list** using a single atomic with index + tag (64-bit CAS) for ABA mitigation.
- **Allocation**: CAS loop pops from free-list; falls back to `operator new` if exhausted.
- **Deallocation**: CAS loop returns blocks to free-list; external pointers freed normally.
- **Cache alignment**: `Block` structure uses 64-byte alignment to prevent false sharing across cores.

Trade-offs:

- Global atomic reduces allocations but adds contention under very high rates (>1M alloc/sec).
- Fixed block size is simple and fast, but wastes memory for small objects.

## Benchmark & Performance

A synthetic benchmark client (`benchmark.cpp`) measures end-to-end latency:

- Each client establishes a TCP connection, subscribes to a unique topic, and publishes with a nanosecond timestamp embedded in the payload.
- On receipt, the client measures RTT as the difference between current time and the embedded timestamp.
- `TCP_NODELAY` is enabled to prevent Nagle buffering and ensure per-message measurement accuracy.
- Throughput and latency percentiles (p50, p99) are logged every second.

## Performance Results

| Clients | Throughput (msg/s) | Avg Latency (µs) | p50 (µs) | p99 (µs) |
|--------:|-------------------:|-----------------:|---------:|---------:|
| 10      | 69,422             | 143.9            | 124.8    | 464.9    |
| 500     | 154,825            | 3,229.4          | 2,972.5  | 6,619.8  |
| 1000    | 168,339            | 5,939.2          | 5,691.9  | 10,853.5 |
| 2000    | 124,450            | 16,141.6         | 15,968.5 | 24,403.6 |

**Observations:**

- Peak throughput (~168k msg/s) achieved at 1000 concurrent clients; performance degrades at 2000 clients due to kernel scheduling overhead and per-socket buffer limits.
- Long p99 tail at high concurrency indicates occasional scheduling delays or slow subscriber drains.

## Limitations

- **No delivery guarantees**: slow or unresponsive clients are pruned; no persistent queue.
- **No backpressure**: subscribers are dropped rather than applying fine-grained flow control.
- **Topic-based sharding** can cause load imbalance if subscription patterns are skewed.
- **Single-machine only**: no clustering, replication, or cross-datacenter support.
- **Fixed-size memory blocks**: may waste memory for small allocations.

## Future Work

**Data plane optimizations:**
- Multi-packet batching to reduce syscall overhead.
- Zero-copy message handling and payload sharing across subscribers.
- DPDK or kernel bypass for sub-microsecond latency at line rate.
- User-space packet scheduling and pacing.

**High-frequency enhancements:**
- Per-worker NUMA-aware memory pools to reduce cross-socket contention.
- Dynamic topic shard rebalancing for skewed workloads.
- Fine-grained backpressure with per-subscriber queues and flow control.
- Persistent log and replay for order guarantees.

## Build & Run Instructions

Prerequisites:

- Linux environment (Ubuntu on WSL2 used for these experiments).
- g++ with C++17 support.
- Increase file descriptor limit for large client counts: `ulimit -n 100000`.

Compile server and benchmark:

```bash
g++ -O2 -std=c++17 -pthread -march=native -o server server.cpp
g++ -O2 -std=c++17 -pthread -march=native -o bench benchmark.cpp
```

Run server and benchmark (example):

```bash
# in one terminal
./server

# in another terminal: run benchmark with N simulated clients
./bench 1000
```

Benchmark options:

- `./bench <num_clients>` — number of concurrent client connections to simulate (default in code is 1000 if omitted).

Notes on running large experiments:

- Ensure `ulimit -n` is large enough for the number of simultaneous sockets you plan to open.
- On WSL2, system scheduling and virtualization overhead can affect absolute numbers; use results for relative comparisons and system behavior analysis rather than for absolute performance claims.

## Dependencies

- **concurrentqueue.h** (external): Moodycamel's lock-free MPMC queue used for inter-thread task distribution.

---