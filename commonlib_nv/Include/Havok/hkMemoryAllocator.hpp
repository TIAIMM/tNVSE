#pragma once

class hkMemoryAllocator {
public:
	virtual			~hkMemoryAllocator();
	virtual void*	blockAlloc(int);
	virtual void	blockFree(void*, int);
	virtual void*	bufAlloc(int*);
	virtual void	bufFree(void*, int);
	virtual void*	bufRealloc(void*, int, int*);
	virtual void	blockAllocBatch(void**, int, int);
	virtual void	blockFreeBatch(void**, int, int);
	virtual bool	canAllocSingleBlock(int);
	virtual bool	canAllocTotal(int);
	virtual void	getMemoryStatistics(void*);
	virtual int		getAllocatedSize(const void*, int);
	virtual void	resetPeakMemoryStatistics();
};

ASSERT_SIZE(hkMemoryAllocator, 0x4);