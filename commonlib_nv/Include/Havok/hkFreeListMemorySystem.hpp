#pragma once

#include "hkMemorySystem.hpp"
#include "hkFreeListAllocator.hpp"

class hkMemoryAllocator;
class hkSystemMemoryBlockServer;

class hkFreeListMemorySystem : public hkMemorySystem {
public:
	hkMemoryAllocator* m_systemAllocator;
	hkSystemMemoryBlockServer m_freeListMemoryServer;
	hkFreeListAllocator m_freeListAllocator;
	DWORD m_leakDetectAllocator;
	_BYTE gapABC[4];
	DWORD dwordAC0;
	DWORD dwordAC4;
	DWORD dwordAC8;
	DWORD dwordACC;
	_RTL_CRITICAL_SECTION rtl_critical_sectionAD0;
	_BYTE gapAE8[24];
	FrameInfo m_frameInfo;
	hkMemoryAllocator* m_heapAllocator;
	hkMemoryRouter m_mainRouter;
	hkSolverAllocator m_solverAllocator;
	hkFreeListMemorySystem::ThreadData m_threadData[32];
	hkCriticalSection m_threadDataLock;
};

ASSERT_SIZE(hkFreeListMemorySystem, 0x3C04);
