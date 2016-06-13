#ifndef MEMMGR_H
#define MEMMGR_H

#include <memarena.h>
#include <mutex>
#include <aws/core/utils/memory/MemorySystemInterface.h>

struct freelist_entry_header {
    size_t mem_size;
};

struct freelist_entry {
    freelist_entry_header header;
    void *mem;
    freelist_entry *next;
};

class CustomMemoryManager : public Aws::Utils::Memory::MemorySystemInterface
{
  public:
    CustomMemoryManager(ma_arena *arena_aws, ma_arena *arena_curl);
    virtual void *AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag = nullptr) override;
    virtual void FreeMemory(void *memoryPtr) override;
    virtual void Begin() override;
    virtual void End() override;

    ma_arena *m_arena_aws;
    ma_arena *m_arena_curl;
    std::mutex m_mtx;

    freelist_entry *m_freelist;
};

#endif
