#pragma once

#include "ThreadSpecificInterfaceManager.hpp"
#include "InterfacedClass.hpp"
#include "BSSpinLock.hpp"

template<class T_DATA>
class LockFreeQueue : public InterfacedClass {
public:
	LockFreeQueue(UInt32 auiMaxThreads, UInt32 auiUnk);
	virtual UInt32 IncItemCount();
	virtual UInt32 DecItemCount();
	virtual UInt32 GetItemCount() const;

	struct Node {
		Node*	pNext = nullptr;
		T_DATA	Data;
	};

	class Interface : public BaseInterface {
	public:
		Interface(LockFreeQueue* apOwner, Node* apNode, T_DATA* apData) {
			pOwner		= apOwner;
			pNode		= apNode;
			pData		= apData;
			uiCounter0C = 0;
			dword10		= 0;
		}

		Node*					pNode;
		T_DATA*					pData;
		DWORD					uiCounter0C;
		DWORD					dword10;

		LockFreeQueue* GetOwner() const {
			return pOwner;
		}

		void Insert(T_DATA& Data) {
			AutoMemContext c(MC_THREAD_SAFE_STRUCT);

			Node* pNewNode = new Node;
			pNewNode->Data = Data;

			LockFreeQueue* pOwner = GetOwner();

			Node* pIter = nullptr;
			do {
				while (true) {
					do {
						pIter = pOwner->pObjectListHead_8;
						pNode->pNext = pIter;
					} while (pIter != pOwner->pObjectListHead_8);

					if (!pIter->pNext)
						break;

					ThreadSafeStructures::Compare(&pOwner->pObjectListHead_8, pIter->pNext, pIter);
				} 
			} while (!ThreadSafeStructures::Compare(pIter, pNewNode, nullptr));

			pOwner->IncItemCount();

			ThreadSafeStructures::Compare(&pOwner->pObjectListHead_8, pNewNode, pIter);

			pNode->pNext = nullptr;
		}
	};

	typedef ThreadSpecificInterfaceManager<Interface> Manager;

	Node*		pObjectList;
	Node*		pObjectListHead_8;
	DWORD		uiUnk0C;
	void*		pUnk10;
	Manager*	pManager;
	UInt32		uiItemCount;
	DWORD		dword1C;
	BSSpinLock	kLock;

	Interface* GetInterface() {
		return pManager->GetInterface(this);
	}

	void Insert(T_DATA& Data) {
		kLock.Lock();
		GetInterface()->Insert(Data);
		kLock.Unlock();
	};
};

ASSERT_SIZE(LockFreeQueue<char const*>::Interface, 0x14);
ASSERT_SIZE(LockFreeQueue<char const*>, 0x40);