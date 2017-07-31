#include <assert.h>
#include <iostream>
#include <thread>
#include <memory>
#include <math.h>
#include <thread>
#include <stdint.h>
#include <string.h>
#include "memmgr.h"

CustomMemoryManager::CustomMemoryManager(ma_ctx *arena_aws, ma_ctx *arena_curl) :
    m_arena_aws(arena_aws),
    m_arena_curl(arena_curl),
    m_mtx()
{}

void *CustomMemoryManager::AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag) {
    void *result;
    std::lock_guard<std::mutex> guard(m_mtx);

    ma_ctx *arena_to_use;

    // we get seg faults if curl allocations go through the linear allocator
    // appears that some state persists between invocations, probably some initial setup
    if (strcmp(allocationTag, "libcurl") == 0) {
        arena_to_use = m_arena_curl;
    } else {
        arena_to_use = m_arena_aws;
    }

    result = ma_alloc(arena_to_use, blockSize);
    return result;
}

void CustomMemoryManager::FreeMemory(void *memoryPtr) {
    std::lock_guard<std::mutex> guard(m_mtx);

    // check if the memory exists within the curl arena range to
    // decide which arena to delegate to.
    uintptr_t free_addr = (uintptr_t)memoryPtr;
    uintptr_t curl_addr = (uintptr_t)m_arena_curl->memory;

    ma_ctx *arena_to_use;
    if (free_addr >= curl_addr && free_addr < (curl_addr + m_arena_curl->size)) {
        arena_to_use = m_arena_curl;
    } else {
        arena_to_use = m_arena_aws;
    }
    ma_free(arena_to_use, memoryPtr);
}

void CustomMemoryManager::Begin() {
}

void CustomMemoryManager::End() {
}
