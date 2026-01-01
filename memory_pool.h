#pragma once
#include <vector>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <limits>

// Optimization: 4KB Block Size matches OS Page Size
constexpr size_t BLOCK_SIZE = 4096; 

class MemoryPool {
public:
    struct Block {
        alignas(64) char data[BLOCK_SIZE];
    };

private:
    struct TaggedIndex {
        uint32_t index;
        uint32_t tag;
    };

    static constexpr uint32_t NULL_IDX = std::numeric_limits<uint32_t>::max();

    std::vector<Block> blocks;
    std::vector<uint32_t> next_indices;
    std::atomic<uint64_t> head;

    void* pool_start;
    void* pool_end;

    static uint64_t pack(TaggedIndex ti) {
        return (static_cast<uint64_t>(ti.index) << 32) | ti.tag;
    }

    static TaggedIndex unpack(uint64_t val) {
        return { static_cast<uint32_t>(val >> 32), static_cast<uint32_t>(val) };
    }

public:
    explicit MemoryPool(size_t poolSize = 50000) { // ~200MB Default
        blocks.resize(poolSize);
        next_indices.resize(poolSize);
        pool_start = blocks.data();
        pool_end = blocks.data() + blocks.size();

        for (uint32_t i = 0; i < poolSize - 1; ++i) next_indices[i] = i + 1;
        next_indices[poolSize - 1] = NULL_IDX;

        head.store(pack({0, 0}));
    }

    void* allocate() {
        uint64_t old_val = head.load(std::memory_order_acquire);
        while (true) {
            TaggedIndex curr = unpack(old_val);
            if (curr.index == NULL_IDX) return ::operator new(sizeof(Block)); 
            
            uint32_t next = next_indices[curr.index];
            uint64_t new_val = pack({next, curr.tag + 1});

            if (head.compare_exchange_weak(old_val, new_val, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return &blocks[curr.index];
            }
        }
    }

    void deallocate(void* ptr) {
        if (ptr >= pool_start && ptr < pool_end) {
            uint32_t idx = static_cast<uint32_t>(static_cast<Block*>(ptr) - static_cast<Block*>(pool_start));
            uint64_t old_val = head.load(std::memory_order_acquire);
            while (true) {
                TaggedIndex curr = unpack(old_val);
                next_indices[idx] = curr.index;
                uint64_t new_val = pack({idx, curr.tag + 1});
                if (head.compare_exchange_weak(old_val, new_val, std::memory_order_acq_rel, std::memory_order_acquire)) return;
            }
        } else {
            ::operator delete(ptr);
        }
    }
};