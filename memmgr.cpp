#include <assert.h>
#include <iostream>
#include <thread>
#include <memory>
#include <math.h>
#include <thread>
#include <stdint.h>
#include <string.h>
#include "memmgr.h"

CustomMemoryManager::CustomMemoryManager(ma_arena *arena_aws, ma_arena *arena_curl) :
    m_arena_aws(arena_aws),
    m_arena_curl(arena_curl),
    m_mtx(),
    m_freelist(NULL)
{}

inline static unsigned int min(size_t a, size_t b) {
    return b > a ? a : b;
}

inline static unsigned int max(unsigned int a, unsigned int b) {
    return b < a ? a : b;
}

void *CustomMemoryManager::AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag) {
    void *result;
    std::lock_guard<std::mutex> guard(m_mtx);

    // we get seg faults if curl allocations go through the linear allocator

    if (strcmp(allocationTag, "libcurl") != 0) {
        result = ma_push(m_arena_aws, blockSize);
        return result;
    }

    // use a freelist strategy for curl

    freelist_entry *entry, *prev, *best_entry, *best_prev;
    prev = NULL;
    entry = m_freelist;
    best_entry = NULL;
    best_prev = NULL;
    // first look for an existing freelist entry that fits
    while (entry) {
        // NOTE: this scans through the whole free list to look for
        // the best fit
        if (blockSize <= entry->header.mem_size) {
            if (!best_entry ||
                best_entry->header.mem_size > entry->header.mem_size) {
                best_entry = entry;
                best_prev = prev;
            }
        }
        prev = entry;
        entry = entry->next;
    }
    if (best_entry) {
        if (best_prev) {
            best_prev->next = best_entry->next;
        } else {
            m_freelist = best_entry->next;
        }
        best_entry->next = NULL;
        return best_entry->mem;
    }
    // no entries found, create a new entry and return that
    // the freelist entry struct is stored before the memory
    // this makes it easy to restore to the freelist on deallocation
    void *entry_and_mem = ma_push(m_arena_curl, sizeof(freelist_entry) + blockSize);
    entry = (freelist_entry *)entry_and_mem;
    result = (unsigned char *)entry + sizeof(freelist_entry);
    entry->header.mem_size = blockSize;
    entry->mem = result;
    entry->next = NULL;
    return result;
}

void CustomMemoryManager::FreeMemory(void *memoryPtr) {
    std::lock_guard<std::mutex> guard(m_mtx);

    // check if the memory exists within the curl
    // arena range. if it does, it belonged to a curl
    // allocation. otherwise, it was created from a linear allocator
    // and deallocation can be ignored
    uintptr_t free_addr = (uintptr_t)memoryPtr;
    uintptr_t curl_addr = (uintptr_t)m_arena_curl->base;
    if (!(free_addr >= curl_addr && free_addr < (curl_addr + m_arena_curl->size))) return;

    // grab the entry before the memory and add it to the freelist
    freelist_entry *entry = (freelist_entry *)((unsigned char *)memoryPtr - sizeof(freelist_entry));
    entry->next = m_freelist;
    m_freelist = entry;
}

void CustomMemoryManager::Begin() {
}

void CustomMemoryManager::End() {
}
