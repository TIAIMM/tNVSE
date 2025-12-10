#pragma once

class hkMemoryRouter;
class hkOstream;
class hkDebugMemorySnapshot;
class hkMemoryPointerInfo;

class hkMemorySystem {
public:
	struct FrameInfo {
		SInt32	m_stackSize;
		SInt32	m_solverBufferSize;
	};

	enum FlagBits {
		FLAG_ROUTER = 1,
		FLAG_FRAME = 2,
		FLAG_ALL = FLAG_ROUTER | FLAG_FRAME
	};

	typedef void (__cdecl* MemoryWalkCallback)(void* start, size_t size, bool allocated, int pool, void* param);

	virtual					~hkMemorySystem();
	virtual hkMemoryRouter* mainInit(const FrameInfo& info, UInt32 flags = FLAG_ALL);
	virtual UInt32			mainQuit(UInt32 flags);
	virtual void			threadInit(hkMemoryRouter&, const char* name, UInt32 flags = FLAG_ALL);
	virtual void			threadQuit(hkMemoryRouter&, UInt32 flags = FLAG_ALL);
	virtual void			printStatistics(hkOstream& ostr);
	virtual UInt32			walkMemory(MemoryWalkCallback callback, void* param);
	virtual void			garbageCollect();
	virtual void			advanceFrame();
	virtual const void*		debugFindBaseAddress(const void* p, int nbytes);
	virtual UInt32			getMemorySnapshot(hkDebugMemorySnapshot& snapshot);
	virtual void			debugLockBaseAddress(const void* baseAddress);
	virtual void			debugUnlockBaseAddress(const void* baseAddress);
	virtual void			debugTagAddress(const void* baseAddress, const void* tag);
	virtual UInt32			getAllocationCallStack(void* ptr, hkMemoryPointerInfo& info);

	static hkMemorySystem* getInstance();
};

ASSERT_SIZE(hkMemorySystem, 0x4);