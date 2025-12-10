#pragma once

#include <BSSpinLock.hpp>

class ScrapHeapManager {
public:
	struct Buffer {
		LPVOID	pData;
		UInt32	uiSize;
	};

	Buffer				kBuffers[64];
	UInt32				uiBufferCount;
	UInt32				uiTotalSize;
	DWORD				dword208[6];
	BSSpinLock			kLock;

	static ScrapHeapManager* GetSingleton();
	void	FreeAllBuffers();
	void	FreeBuffers(UInt32 auiBuffersToFree);
	void	ReleaseBuffer(void* apAddress, SIZE_T aSize);
	void*	RequestBuffer(SIZE_T& arSize);

private:
	friend class ScrapHeap;

	void*	AllocBuffer(SIZE_T aSize);
	BOOL	Release(void* apAddress, SIZE_T aSize);
	BOOL	Decommit(SIZE_T aStartSize, SIZE_T aEndSize, SIZE_T aSize);

	void	RemoveBuffer(UInt32 auiIndex);

	void	ExtendBufferMemory(void* apAddress, SIZE_T aLargestAvailableSize, SIZE_T aSize);
};

ASSERT_SIZE(ScrapHeapManager, 0x240);

class ScrapHeap : public BSMemObject {
public:
	ScrapHeap();
	ScrapHeap(SIZE_T aReserveSize);
	~ScrapHeap();

	struct Block {
		UInt32	uiSize;
		Block*	pPrevious;
	};

	struct FreeBlock : Block {
		FreeBlock* pLeft;
		FreeBlock* pRight;
	};


	char*	pBaseAddress;
	char*	pCommitStart;
	char*	pCommitEnd;
	Block*	pLastBlock;

	template <typename T>
	T* Allocate(UInt32 aCount) {
		return (T*)Allocate(aCount * sizeof(T), alignof(T));
	}
	template <typename T>
	T* Allocate(UInt32 aCount, UInt32 aAlignment) {
		return (T*)Allocate(aCount * sizeof(T), aAlignment);
	}
	void*	Allocate(UInt32 aSize, UInt32 aAlignment = 4);
	void	Deallocate(void* apMem);

	UInt32	GetSize() const;

private:
	void	Initialize(SIZE_T aReserveSize);
};

ASSERT_SIZE(ScrapHeap, 0x10);
ASSERT_SIZE(ScrapHeap::Block, 0x8);