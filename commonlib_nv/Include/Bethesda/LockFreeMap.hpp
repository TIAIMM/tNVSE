#pragma once

#include "BSSpinLock.hpp"
#include "InterfacedClass.hpp"
#include "ThreadSpecificInterfaceManager.hpp"

template<typename T_Key, typename T_Data>
class LockFreeMap : public InterfacedClass {
public:
	struct Data004 {
		UInt32	unk000;
		UInt32	unk004;
		UInt32* unk008;
	};

	class Interface : BaseInterface {
	public:
		UInt32			mapData004Unk000;
		UInt32			mapData004Unk008;
		UInt32*			mapData004Unk00C;
		UInt32			unk010;
		UInt32*			unk014;
		UInt32*			unk018;
		UInt32			unk01C;
		UInt32			uiSomeCount20; // Compared with LockFreeMap::unk010

		struct Item {
			UInt32	uiKey;
			T_Data	Data;
			UInt32* pUnk;
		};
	};

	typedef ThreadSpecificInterfaceManager<Interface> Manager;

	LockFreeMap(UInt32 auiMaxThreads, UInt32 auiInitialSize, UInt32 auiUnk2);
	virtual			~LockFreeMap();
	virtual bool	Lookup(T_Key key, T_Data& val);
	virtual void	Unk_03(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt8 arg4);
	virtual bool	Insert(T_Key key, T_Data& val, UInt8 arg3);
	virtual void	EraseKey(T_Key key);
	virtual void	Unk_06(UInt32 arg1, UInt32 arg2);
	virtual void	Unk_07(UInt32 arg); // Calls 06 for every item
	virtual bool	Unk_08(T_Key key, T_Data& val); // Same as Lookup, but no lock
	virtual UInt32	CalcBucketIndex(T_Key key);
	virtual void	FreeKey(T_Key key);
	virtual T_Key	GenerateKey(T_Key src);
	virtual T_Key	CopyKeyTo(T_Key src, T_Key& val);
	virtual bool	LKeyGreaterOrEqual(T_Key lkey, T_Key rkey);
	virtual bool	KeysEqual(T_Key lkey, T_Key rkey);
	virtual UInt32	IncQueuedCount();
	virtual UInt32	DecQueuedCount();
	virtual UInt32	GetQueuedCount();

	Data004**	dat004;
	UInt32		uiSize;
	UInt32**	ppBuckets;
	UInt32		unk010;
	Manager*	pManager;
	UInt32		uiQueuedCount;
	UInt32		unk01C;
	BSSpinLock	kLock;

	void Reset(bool abNoNewManager);

	class LockFreeMapIterator : public BSMemObject {
	public:
		LockFreeMapIterator() : uiIndex(0) {};
		virtual ~LockFreeMapIterator() {};
		virtual void ClearKey() { Key = T_Key(0); };

		enum StateFlags {
			STATE_UNK_1		= 1,
			STATE_FINISHED	= 2,
		};

		UInt32		uiIndex;
		T_Key		Key;
		Bitfield8	ucFlags;

		void SetBit1(bool abSet) { ucFlags.SetBit(STATE_UNK_1, abSet); };
		bool GetBit1() const { return ucFlags.GetBit(STATE_UNK_1); };

		void SetFinished() { ucFlags.SetBit(STATE_FINISHED, true); };
		bool IsFinished() const { return ucFlags.IsSet(STATE_FINISHED); };
	};

#if 0
	bool GetNext(LockFreeMapIterator& arIter, T_Key& arKey, T_Data& arData, bool abUnk) {
		kLock.Lock();

		bool bResult = false;
		if (!arIter.IsFinished()) {
			while (true) {
				if (arIter.GetBit1()) {
					bResult =
				}
				else {
					bResult = 
					arIter.SetBit1(true);
				}

				if (bResult)
					break;

				++arIter.uiIndex;

				if (arIter.uiIndex >= uiSize) {
					arIter.SetFinished(true);
					break;
				}

				if (!abUnk)
					break;

				arIter.SetBit1(false);
			}
			arKey = arIter.Key;
		}

		kLock.Unlock();

		return bResult;
	}
#endif
};

template<class T_Data>
class LockFreeStringMap : public LockFreeMap<const char*, T_Data> {
public:
	class LockFreeStringMapIterator : public LockFreeMap<const char*, T_Data>::LockFreeMapIterator {
	public:
		LockFreeStringMapIterator() : LockFreeMap<const char*, T_Data>::LockFreeMapIterator() {
			//Key = &cBuffer;
			cBuffer[0] = 0;
		};
		virtual ~LockFreeStringMapIterator() {};
		virtual void ClearKey() override { cBuffer[0] = 0; };

		char cBuffer[256];
	};
};

template<class T_Data>
class LockFreeCaseInsensitiveStringMap : public LockFreeStringMap<T_Data> {};

ASSERT_SIZE(LockFreeCaseInsensitiveStringMap<char>, 0x40);
ASSERT_SIZE(LockFreeCaseInsensitiveStringMap<char>::Interface, 0x24);