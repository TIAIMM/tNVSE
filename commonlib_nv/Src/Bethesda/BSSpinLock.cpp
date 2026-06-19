#include "BSSpinLock.hpp"
#include "Windows.h"

//#define DEBUG_SPINLOCK

// 0x40FBF0
void BSSpinLock::Lock(const char* apName) {
#if 0
    ThisStdCall(0x40FBF0, this, 0);
#else
#ifdef DEBUG_SPINLOCK
    if (!apName)
		apName = "Unknown";
#endif

    DWORD CurrentThreadId = GetCurrentThreadId();
    if (uiOwningThread == CurrentThreadId) {
#ifdef DEBUG_SPINLOCK
        _MESSAGE("[ BSSpinLock::Lock ] Request from \"%s\" on owning thread | %i", apName, CurrentThreadId);
#endif
        ++uiLockCount;
    }
    else {
        UInt32 uiSpinCount = 0;
#ifdef DEBUG_SPINLOCK
        _MESSAGE("[ BSSpinLock::Lock ] Request from \"%s\" on non-owning thread | %i", apName, CurrentThreadId);
#endif
        while (InterlockedCompareExchange(&uiOwningThread, CurrentThreadId, 0)) {
            if (++uiSpinCount > 10000)
                Sleep(1);
            else {
                Sleep(0);
            }
        }
        uiLockCount = 1;
    }
#endif
}

// 0x40FBA0
void BSSpinLock::Unlock() {
#ifdef DEBUG_SPINLOCK
    DWORD CurrentThreadId = GetCurrentThreadId();
    bool bIsOwner = uiOwningThread == CurrentThreadId;
#endif

    if (uiLockCount-- == 1) {
#ifdef DEBUG_SPINLOCK
        if (bIsOwner) {
            _MESSAGE("[ BSSpinLock::Unlock ] Unlocked on owning thread | %i", CurrentThreadId);
        }
        else {
            _MESSAGE("[ BSSpinLock::Unlock ] Unlocked on non-owning thread | %i", CurrentThreadId);
		}
#endif
        uiOwningThread = 0;
    }
    else {
#ifdef DEBUG_SPINLOCK
        _MESSAGE("[ BSSpinLock::Unlock ] Unlocked on thread | %i | Lock count %i", CurrentThreadId, uiLockCount);
#endif
    }
}
