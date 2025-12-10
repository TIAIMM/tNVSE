#pragma once

#include <cstring>
#include "BSMemObject.hpp"

class BSSpinLock : public BSMemObject {
public:
	BSSpinLock() {
		memset(this, 0, sizeof(BSSpinLock));
	}

	UInt32 uiOwningThread;
	UInt32 uiLockCount;
	UInt32 unk08[6];

	void Lock(const char* apName = nullptr);
	void Unlock();
};

ASSERT_SIZE(BSSpinLock, 0x20);