#include "BSMemory.hpp"
#include "MemoryManager.hpp"
#include "SafeWrite.h"

namespace
{
	using CreateHeapFn = int(__cdecl*)(UInt32);

	int __cdecl CreateHeapStub(UInt32) { return 1; }

	struct HeapBootstrapCallSite
	{
		const char* name;
		UInt32 heapAddress;
		UInt32 callAddress;
		CreateHeapFn createHeap;
		UInt32 predecessorTarget = 0;

		bool ReadTarget(UInt32& target) const
		{
			target = 0;
			if (!callAddress
				|| *reinterpret_cast<const UInt8*>(callAddress) != 0xE8)
			{
				return false;
			}
			const SInt32 displacement =
				*reinterpret_cast<const SInt32*>(callAddress + 1);
			target = static_cast<UInt32>(callAddress + 5 + displacement);
			return true;
		}

		bool RollbackOwned()
		{
			UInt32 currentTarget = 0;
			if (!predecessorTarget || !ReadTarget(currentTarget))
				return false;
			if (currentTarget == reinterpret_cast<UInt32>(&CreateHeapStub))
			{
				if (!ReplaceCall(callAddress, predecessorTarget)
					|| !ReadTarget(currentTarget))
				{
					return false;
				}
			}
			return currentTarget == predecessorTarget;
		}
	};

	HeapBootstrapCallSite s_gameHeapBootstrapSite{
		"FalloutNV heap bootstrap CALL (__cdecl)",
		0xF9907C,
		0xC62B21,
		reinterpret_cast<CreateHeapFn>(0xC770C3),
	};
	HeapBootstrapCallSite s_geckHeapBootstrapSite{
		"GECK heap bootstrap CALL (__cdecl)",
		0x12705BC,
		0xECC3CB,
		reinterpret_cast<CreateHeapFn>(0xEDDB6A),
	};
}

bool	bInitialized = false;

_declspec(noinline) void InitializeHeap() {
	HeapBootstrapCallSite& bootstrapSite = *(UInt8*)0x401190 != 0x55
		? s_gameHeapBootstrapSite : s_geckHeapBootstrapSite;
	if (!*reinterpret_cast<HANDLE*>(bootstrapSite.heapAddress)
		&& bootstrapSite.createHeap)
	{
		bootstrapSite.createHeap(true);
		UInt32 predecessorTarget = 0;
		if (bootstrapSite.ReadTarget(predecessorTarget))
		{
			bootstrapSite.predecessorTarget = predecessorTarget;
			// FalloutNV/GECK heap bootstrap CALL (__cdecl).
			const bool published =
				WriteRelCall(bootstrapSite.callAddress, &CreateHeapStub);
			UInt32 publishedTarget = 0;
			if (!published || !bootstrapSite.ReadTarget(publishedTarget)
				|| publishedTarget != reinterpret_cast<UInt32>(&CreateHeapStub))
			{
				bootstrapSite.RollbackOwned();
			}
		}
	}

	bInitialized = true;
}

// 0x401000
void* BSNew(size_t stSize) {
	if (!bInitialized)
		InitializeHeap();
    return MemoryManager::GetSingleton()->Allocate(stSize);
}

// 0x553320
void* BSNewAligned(size_t stAlign, size_t stSize) {
    char* pMemory = static_cast<char*>(MemoryManager::GetSingleton()->Allocate(stSize + stAlign));
    UInt32 uiAlignment = stAlign - (reinterpret_cast<UInt8>(pMemory) & (stAlign - 1));
    pMemory[UInt8(uiAlignment) - 1] = uiAlignment;
    return &pMemory[UInt8(uiAlignment)];
}

// 0x42F5D0
void* BSReallocate(void* pvMem, size_t stSize) {
    return MemoryManager::GetSingleton()->Reallocate(pvMem, stSize);
}

// 0x401030
void BSFree(void* pvMem) {
    MemoryManager::GetSingleton()->Deallocate(pvMem);
}

SIZE_T BSSize(void* pvMem) {
	return MemoryManager::GetSingleton()->Size(pvMem);
}
