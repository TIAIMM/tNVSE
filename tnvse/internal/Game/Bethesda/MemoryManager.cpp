#include "MemoryManager.hpp"

MemoryManager* MemoryManager::GetSingleton() {
	return &*(MemoryManager*)0x11F6238;
}


MemoryPool* MemoryManager::GetPool(UInt32 auiIndex) {
	return ((MemoryPool**)0x11F63B8)[auiIndex];
}

// 0xAA42E0
ScrapHeap* MemoryManager::GetThreadScrapHeap() {
	return ThisStdCall<ScrapHeap*>(0xAA42E0, this);
}

// 0xAA3E40
void* MemoryManager::Allocate(size_t aSize) {
	return ThisStdCall<void*>(0xAA3E40, this, aSize);
}

// 0xAA4150
void* MemoryManager::Reallocate(void* apOldMem, size_t auiNewSize) {
	return ThisStdCall<void*>(0xAA4150, this, apOldMem, auiNewSize);
}

// 0xAA4060
void MemoryManager::Deallocate(void* apMemory) {
	ThisStdCall(0xAA4060, this, apMemory);
}

// 0xAA44C0
size_t MemoryManager::Size(void* apMemory) const {
	return ThisStdCall<size_t>(0xAA44C0, this, apMemory);
}
