#pragma once

static UInt32* GetCounterSingleton();

enum BS_TASK_STATE {
	BS_TASK_STATE_PENDING			= 0,
	BS_TASK_STATE_QUEUED			= 1,
	BS_TASK_STATE_MOVING			= 2,
	BS_TASK_STATE_RUNNING			= 3,
	BS_TASK_STATE_FINISHED			= 4,
	BS_TASK_STATE_COMPLETED			= 5,
	BS_TASK_STATE_CANCELED			= 6,
	BS_TASK_STATE_POST_PROCESSING	= 7,
};

template <typename T_Size>
class BSTask {
public:
	BSTask();
	virtual ~BSTask();
	virtual void Run();
	virtual void AddTask();
	virtual void Cancel(BS_TASK_STATE aeState, void* arg1);
	virtual bool GetDescription(const char* apDescription, size_t aiBufferSize);

	BSTask*			unk04;
	UInt32			uiRefCount;
	BS_TASK_STATE	eState;
#pragma pack(push, 4)
	mutable T_Size	iKey;
#pragma pack(pop)

	// 0x92C870
	void IncRefCount() {
		InterlockedIncrement(&uiRefCount);
	}

	// 0x44DD60
	void DecRefCount() {
		if (!InterlockedDecrement(&uiRefCount))
			delete this;
	}
};

ASSERT_SIZE(BSTask<SInt64>, 0x18);