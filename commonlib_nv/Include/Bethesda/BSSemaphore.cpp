#include "BSSemaphore.hpp"

BSSemaphore::BSSemaphore() {
	uiCount = 0;
	uiMaxCount = 1;
	hSemaphore = CreateSemaphore(0, 0, 1, 0);
}

BSSemaphore::~BSSemaphore() {
	CloseHandle(hSemaphore);
}

void BSSemaphore::Lock() {
	ReleaseSemaphore(hSemaphore, 1, 0);
	InterlockedIncrement(&uiCount);
}

void BSSemaphore::Wait() {
	WaitForSingleObject(hSemaphore, 0xFFFFFFFF);
	InterlockedDecrement(&uiCount);
}
