#pragma once

class hkFreeListAllocatorServer;

class hkFreeList {
public:
    struct Element {
        Element* m_next;
    };

    __declspec(align(16)) struct Block {
        Block* m_next;
        size_t m_blockSize;
        UInt8* m_start;
        size_t m_numElements;
    };

    Element*                    m_free;
    size_t                      m_elementSize;
    Block*                      m_lastIncrementalBlock;
    Block*                      m_blocks;
    Block*                      m_freeBlocks;
    size_t                      m_blockSize;
    size_t                      m_align;
    size_t                      m_maxBlockSize;
    UInt8*                      m_top;
    UInt8*                      m_blockEnd;
    size_t                      m_numFreeElements;
    size_t                      m_totalNumElements;
    hkFreeListAllocatorServer*  m_memoryServer;
};

ASSERT_SIZE(hkFreeList, 0x34);