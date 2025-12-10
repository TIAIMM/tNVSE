#pragma once

class hkFreeListAllocatorServer {
public:
    virtual         ~hkFreeListAllocatorServer();
    virtual void*   alloc(size_t size);
    virtual void    free(void* data);
    virtual bool    isValidAlloc(void* data);
    virtual size_t  getAllocatedSize(const void* p, size_t size);
    virtual size_t  getAllocatedSizeWithOverhead(void* p, size_t size);
    virtual void*   criticalAlloc(size_t size);
    virtual void    criticalFree(void* ptr, size_t size);
};

ASSERT_SIZE(hkFreeListAllocatorServer, 0x4);