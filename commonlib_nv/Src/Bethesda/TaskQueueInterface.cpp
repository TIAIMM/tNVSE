#include "TaskQueueInterface.hpp"
#include "NiNode.hpp"

bool* const TaskQueueInterface::bIsActive = (bool*)0x11A31F4;

TaskQueueInterface* TaskQueueInterface::GetSingleton() {
	return *(TaskQueueInterface**)0x11DF1A8;
}

// GAME - 0x8C7AA0
bool TaskQueueInterface::IsOnNonMainThread() {
	return StdCall<bool>(0x8C7AA0);
}

// GAME - 0x87A6D0
void TaskQueueInterface::Wait() {
	pQueue->Wait();
}

// GAME - 0x87D160
void TaskQueueInterface::QueueIfActive(BSPackedTask& akTask) {
	if (*bIsActive) {
		if (!pActiveQueue->AddTask(akTask)) {
			_MESSAGE("[ TaskQueueInterface::QueueIfActive ] Failed to add task %s", akTask.GetTaskName());
			DebugBreak();
		}
	}
	else {
		_MESSAGE("[ TaskQueueInterface::QueueIfActive ] Task %s not added because queue is inactive", akTask.GetTaskName());
		TaskUnpackFunc(akTask);
	}
}

// GAME - 0x87AC00
void TaskQueueInterface::QueueAttachNode(NiAVObject* apObject, NiNode* apNewParent, bool abFirstAvail, bool abUpdateProperties) {
	if (!apObject || !apNewParent)
		return;

	BSPackedTask task;
	task.Data[0].iData = BSPackedTask::TASK_ATTACH_NODE;
	task.Data[1].pData = apObject;
	task.Data[2].pData = apNewParent;
	task.Data[3].bData = abFirstAvail;
	task.Data[4].bData = abUpdateProperties;

	apObject->IncRefCount();
	apNewParent->IncRefCount();
	QueueIfActive(task);
}

// GAME - 0x87AC60
void TaskQueueInterface::QueueDetachNode(NiAVObject* apObject) {
	if (!apObject)
		return;

	BSPackedTask task;
	task.Data[0].iData = BSPackedTask::TASK_DETACH_NODE;
	task.Data[1].pData = apObject;
	apObject->IncRefCount();
	QueueIfActive(task);
}

// GAME - 0x87AD50
void TaskQueueInterface::QueueUpdate3D(NiAVObject* apObject) {
	if (!apObject)
		return;

	BSPackedTask task;
	task.Data[0].iData = BSPackedTask::TASK_UPDATE_3D;
	task.Data[1].pData = apObject;
	apObject->IncRefCount();
	QueueIfActive(task);
}

void TaskQueueInterface::TaskUnpackFunc(BSPackedTask& akTask) {
	CdeclCall(0x87B990, &akTask);
}

// GAME - 0x87AAD0
void TaskQueueInterface::CellTransformsUpdateAddNode(NiAVObject* apObject) {
	ThisStdCall(0x87AAD0, this, apObject);
}

// GAME - 0x87AA90
void TaskQueueInterface::QueueCellTransformsUpdate() {
	BSPackedTask kTask;
	kTask.Data[0].iData = BSPackedTask::TASK_CELL_TRANSFORMS_UPDATE;
	QueueIfActive(kTask);
}

// GAME - 0x87ABB0
void TaskQueueInterface::QueueUpdateGeomorphs(NiAVObject* apObject) {
	BSPackedTask kTask;
	kTask.Data[0].iData = BSPackedTask::TASK_GEOMORPH_UPDATE;
	kTask.Data[1].pData = apObject;
	apObject->IncRefCount();
	QueueIfActive(kTask);
}

// GAME - 0x87B400
void TaskQueueInterface::QueueSet3DNull(TESObjectREFR* apRef) {
	BSPackedTask kTask;
	kTask.Data[0].iData = BSPackedTask::TASK_SET_3D_NULL;
	kTask.Data[1].pData = apRef;
	QueueIfActive(kTask);
}
