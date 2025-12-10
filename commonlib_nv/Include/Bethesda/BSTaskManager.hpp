#pragma once

#include "LockFreeMap.hpp"
#include "NiSmartPointer.hpp"
#include "BSTaskManagerThread.hpp"
#include "BSTask.hpp"

class IOTask;

template <typename T>
class BSTaskManager : public LockFreeMap<T, NiPointer<BSTask<T>>> {
public:
	virtual bool	Unk18(void*);
	virtual void*	Unk19(void*);
	virtual bool	Unk20(IOTask* apTask);
	virtual void	CancelAllTasks();
	virtual void*	Unk22(void*);

	bool						bExit;
	DWORD						dword44;
	DWORD						dword48;
	UInt32						uiThreadCount;
	BSTaskManagerThread<T>**	ppThreads;
	void*						pBucketCounts;
	DWORD						uiBucketCountTotal;
};

ASSERT_SIZE(BSTaskManager<SInt64>, 0x5C);