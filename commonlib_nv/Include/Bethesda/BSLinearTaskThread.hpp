#pragma once

#include "BSThread.hpp"
#include "BSSemaphore.hpp"

class BSLinearTaskThread : public BSThread {
public:
	virtual void OnStartup() {};
	virtual void OnShutdown() {};
	virtual void RunLinearTasks() = 0;

	DWORD ThreadProc() override;

	BSLinearTaskThread();
	~BSLinearTaskThread();

	BSSemaphore StartProcessing;
	BSSemaphore FinishedProcessing;
	bool		bKillThread;

	DWORD ThreadProcEx();
};

ASSERT_SIZE(BSLinearTaskThread, 0x4C);