#pragma once
#include <vector>
#include <atomic>
#include <cstddef>
#include "concurrentqueue.h"

class MemoryPool {
    struct Block {
        alignas(64) char data[1024]; // fixed-size block (1 KB per message)
    };

    std::vector<Block> blocks;
    moodycamel::ConcurrentQueue<Block*> freeBlocks;

public:
    explicit MemoryPool(size_t poolSize = 100000) {
        blocks.resize(poolSize);
        for (auto &block : blocks) {
            freeBlocks.enqueue(&block);
        }
    }

    void* allocate() {
        Block* ptr = nullptr;
        if (freeBlocks.try_dequeue(ptr)) {
            return ptr->data;
        }
        // fallback if pool exhausted
        return ::operator new(sizeof(Block));
    }

    void deallocate(void* p) {
        Block* blockPtr = reinterpret_cast<Block*>(
            reinterpret_cast<char*>(p) - offsetof(Block, data));
        freeBlocks.enqueue(blockPtr);
    }
};
