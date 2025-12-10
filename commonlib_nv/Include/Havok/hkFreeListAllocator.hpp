#pragma once

#include "hkMemoryAllocator.hpp"
#include "hkFreeList.hpp"
#include "hkLargeBlockAllocator.hpp"

class hkMemoryBlockServer;
class hkFreeList;

class hkFreeListAllocator : public hkMemoryAllocator {
public:
	CRITICAL_SECTION		m_criticalSection;
	size_t					m_totalBytesInFreeLists;
	size_t					m_peakInUse;
	hkMemoryBlockServer*	m_server;
	hkLargeBlockAllocator	m_largeAllocator;
	hkFreeList*				m_sizeToFreeList[41];
	hkFreeList*				m_freeLists[41];
	SInt32					m_numFreeLists;
	hkFreeList*				m_topFreeList;
	hkFreeList*				m_lastFreeList;
	hkFreeList				m_freeListMemory[41];
	size_t					m_softLimit;
	SInt32					m_incrementalFreeListIndex;
};

ASSERT_SIZE(hkFreeListAllocator, 0xA9C);