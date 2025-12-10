#pragma once

#include "BSMemObject.hpp"
#include "InterfacedClass.hpp"
#include "AutoMemContext.hpp"

template <typename T>
class ThreadSpecificInterfaceManager : public BSMemObject {
public:
	struct ThreadEntry : public BSMemObject {
		UInt32	uiThreadID;
		T*		pData;
	};

	// 0xC42180
	ThreadSpecificInterfaceManager(UInt32 auiMaxThreads) {
		AutoMemContext c(MC_THREAD_SAFE_STRUCT);

		uiThreadCount = 0;
		uiMaxThreads = auiMaxThreads;

		pThreadEntries = new ThreadEntry[auiMaxThreads];

		uiTLSIndex = TlsAlloc();
	}

	// 0x6665C0
	~ThreadSpecificInterfaceManager() {
		for (UInt32 i = 0; i < uiMaxThreads; i++) {
			if (pThreadEntries[i].pData)
				delete pThreadEntries[i].pData;
		}

		delete[] pThreadEntries;

		TlsFree(uiTLSIndex);
	}

	UInt32			uiMaxThreads;
	UInt32			uiTLSIndex;
	ThreadEntry*	pThreadEntries;
	UInt32			uiThreadCount;

	// 0xC41610
	T* AddInterface(InterfacedClass* apClass) {
		T* pInterface = nullptr;
		UInt32 uiThread = InterlockedIncrement(&uiThreadCount) - 1;
		if (uiThread >= uiMaxThreads) {
#ifdef _DEBUG
			_MESSAGE("Could not add new interface for thread %08X in ThreadSpecificInterfaceManager::AddInterface.  Max threads is: %i\n", GetCurrentThreadId(), uiMaxThreads);
#endif
			return pInterface;
		}

		pInterface = static_cast<T*>(apClass->AllocateInterface(uiThread));
		pThreadEntries[uiThread].pData = pInterface;
		pThreadEntries[uiThread].uiThreadID = GetCurrentThreadId();
		TlsSetValue(uiTLSIndex, pInterface);
		return pInterface;
	}

	// 0x44D5C0
	T* GetInterface(InterfacedClass* apClass) {
		T* pInterface = static_cast<T*>(TlsGetValue(uiTLSIndex));
		if (pInterface)
			return pInterface;

		return AddInterface(apClass);
	};
};

ASSERT_SIZE(ThreadSpecificInterfaceManager<void>, 0x10)
ASSERT_SIZE(ThreadSpecificInterfaceManager<void>::ThreadEntry, 0x8)