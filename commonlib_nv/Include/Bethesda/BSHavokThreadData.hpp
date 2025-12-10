#pragma once

#include "hkMemoryRouter.hpp"

class BSHavokThreadData {
public:
	hkMemoryRouter* pHavokMemoryRouter	= nullptr;
	void*			pHavokAlloc			= nullptr;
	DWORD			uiOwnerID			= 0;

	static void Initialize(BSHavokThreadData& arData);
	static void Shutdown(BSHavokThreadData& arData);
};

ASSERT_SIZE(BSHavokThreadData, 0xC);