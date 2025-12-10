#pragma once

#include "BSLinearTaskThread.hpp"
#include "hkMemoryRouter.hpp"

class AILinearTaskThread : public BSLinearTaskThread {
public:
	void(__cdecl*	pTaskFunc)();
	hkMemoryRouter	kHavokMemoryRouter;
	void*			pHavokAlloc;
};

ASSERT_SIZE(AILinearTaskThread, 0x78);