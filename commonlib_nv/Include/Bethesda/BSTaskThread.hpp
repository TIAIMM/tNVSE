#pragma once

#include "BSSemaphore.hpp"
#include "BSHavokThreadData.hpp"

class BSTaskThread {
public:
	BSTaskThread(const char* apThreadName);
	virtual ~BSTaskThread();
	virtual void RunTasks() = 0;

	HANDLE				hThread;
	DWORD				uiThreadID;
	BSSemaphore			kSemaphore1;
	BSSemaphore			kSemaphore2;
	BSHavokThreadData	kHavokData;

	static DWORD WINAPI ThreadProc(LPVOID lpThreadParameter);

	void LockSema1();
	void WaitSema1();

	void LockSema2();
	void WaitSema2();
};

ASSERT_SIZE(BSTaskThread, 0x30);