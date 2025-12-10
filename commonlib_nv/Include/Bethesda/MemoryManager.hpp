#pragma once

#include "BSMap.hpp"
#include "MemoryContexts.hpp"

class BSExtraData;
class ExtraDataList;
class NiNode;
class ScriptLocals;
class TESObjectREFR;
class MemoryPool;
class IMemoryHeap;
struct HeapStats;
class ScrapHeap;
class IMemoryManagerFile;

struct MemoryManager {
	static const char* ContextNames[66];
	static UInt32 ContextCounters[66];
	static UInt32 ContextAllocs[66];
	static UInt32 PoolAllocs[600];

    bool						bInitialized;
    UInt16						usNumHeaps;
    UInt16						usNumPhysicalHeaps;
    IMemoryHeap**				ppHeaps;
    IMemoryHeap*				pHeapsByContext[66];
	BSMap<UInt32, ScrapHeap*>	kThreadHeapMap;
	IMemoryHeap**				ppPhysicalHeaps;
    bool						bSpecialHeaps;
    bool						bAllowPoolUse;
    UInt32						uiSysAllocBytes;
	UInt32						uiMallocBytes;
	UInt32						uiEmergencyCleanupPassCount;

	static MemoryManager s_Instance;

	static MemoryManager* GetSingleton();

	void InitializePools();
	void Initialize();

	static MemoryPool* GetPool(UInt32 auiIndex);
    
	ScrapHeap* GetThreadScrapHeap();

	[[nodiscard]]
	__declspec(restrict) __declspec(allocator) void* Allocate(size_t aSize);
	[[nodiscard]]
	__declspec(restrict) __declspec(allocator) void* Reallocate(void* apOldMem, size_t auiNewSize);

    void Deallocate(void* apMemory);

	size_t Size(void* apMemory) const;
};