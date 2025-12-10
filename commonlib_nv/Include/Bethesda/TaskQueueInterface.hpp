#pragma once

#include "BSPackedTaskQueue.hpp"
#include "BSPackedTask.hpp"

class TESObjectREFR;
class NiAVObject;
class NiNode;

struct TaskQueueInterface {
public:
	BSPackedTaskQueue*	pQueue;
	BSPackedTaskQueue*	pSecondaryQueue;
	BSPackedTaskQueue*	pActiveQueue;
	UInt32				threadID0C;

	static bool* const bIsActive;

	static TaskQueueInterface* GetSingleton();

	static bool IsOnNonMainThread();

	void Wait();

	void QueueIfActive(BSPackedTask& akTask);

	void QueueAttachNode(NiAVObject* apObject, NiNode* apNewParent, bool abFirstAvail = true, bool abUpdateProperties = false);
	void QueueDetachNode(NiAVObject* apObject);
	void QueueUpdate3D(NiAVObject* apObject);

	void TaskUnpackFunc(BSPackedTask& akTask);


	void CellTransformsUpdateAddNode(NiAVObject* apObject);
	void QueueCellTransformsUpdate();
	void QueueUpdateGeomorphs(NiAVObject* apObject);
	void QueueSet3DNull(TESObjectREFR* apRef);
};

ASSERT_SIZE(TaskQueueInterface, 0x10);