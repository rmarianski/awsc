#ifndef MEMMGR_H
#define MEMMGR_H

#include <memarena.h>
#include <mutex>
#include <aws/core/utils/memory/MemorySystemInterface.h>

class CustomMemoryManager : public Aws::Utils::Memory::MemorySystemInterface
{
  public:
    CustomMemoryManager(ma_ctx *arena_aws, ma_ctx *arena_curl);
    virtual void *AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag = nullptr) override;
    virtual void FreeMemory(void *memoryPtr) override;
    virtual void Begin() override;
    virtual void End() override;

    ma_ctx *m_arena_aws;
    ma_ctx *m_arena_curl;
    std::mutex m_mtx;
};

#endif
