#pragma once

#include "hkFreeListAllocatorServer.hpp"

class hkLargeBlockLimitedMemoryListener;
class hkMemTreeChunk;
typedef UInt32 hkBinIndex;
typedef UInt32 hkBinMap;

struct hkMemChunk {
	size_t prevFoot;
	size_t head;
};

struct hkMemPage {
	hkMemPage*	m_prev;
	hkMemPage*	m_next;
	int			m_numAllocs;
	size_t		m_size;
	char*		m_start;
	char*		m_end;
};

class hkLargeBlockAllocator : public hkFreeListAllocatorServer {
public:
	hkLargeBlockLimitedMemoryListener*	m_limitedListener;
	hkBinMap							m_treemap;
	char*								m_leastAddr;
	hkMemChunk*							m_top;
	size_t								m_topsize;
	hkMemTreeChunk*						m_treebins[32];
	hkMemoryBlockServer*				m_server;
	hkMemChunk							m_zeroChunk;
	hkMemPage							m_pages;
	size_t								m_sumAllocatedWithMgrOverhead;
	size_t								m_sumAllocatedSize;
};

ASSERT_SIZE(hkLargeBlockAllocator, 0xC4);