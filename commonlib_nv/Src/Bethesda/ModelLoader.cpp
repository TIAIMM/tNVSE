#include "ModelLoader.hpp"

class TESObjectREFR;

ModelLoader** g_modelLoader = (ModelLoader**)0x011C3B3C;

ModelLoader* ModelLoader::GetSingleton() {
	return *g_modelLoader;
}

// 0x446C90
UInt32 ModelLoader::GetReferenceCount() {
	return pQueuedReferenceMap1->GetQueuedCount();
}

// 0x446DC0
UInt32 ModelLoader::GetTotalTaskCount() {
	return ThisStdCall<UInt32>(0x446DE0, pBackgroundCloneThread);
}

// 0x448330
bool ModelLoader::AddLoadedFile(const char* apPath, LoadedFile* apLoadedFile) {
	return pLoadingFileMap->Insert(apPath, apLoadedFile, false);
}

// 0x448370
void ModelLoader::RemoveLoadedFile(const char* apPath) {
	pLoadingFileMap->EraseKey(apPath);
}

void ModelLoader::QueueReference(TESObjectREFR* apRefr, IO_TASK_PRIORITY aePriority, bool abAllowQueueReferenceQueuing) {
	ThisStdCall(0x444850, this, apRefr, aePriority, (UInt32)abAllowQueueReferenceQueuing);
}

// 0x447080
NiNode* ModelLoader::LoadFile(const char* apPath, UInt32 aeLODFadeMult, bool abAssignShaders, int unused, bool abKeepUV, bool abNoUseCountIncrease) {
	return ThisStdCall<NiNode*>(0x447080, this, apPath, aeLODFadeMult, abAssignShaders, unused, abKeepUV, abNoUseCountIncrease);
}

// 0x4472A0
bool ModelLoader::LookupModel(const char* apPath, NiPointer<Model>& arOut) {
	arOut = nullptr;
	Model* pModel = nullptr;
	if (pModelMap->Lookup(apPath, pModel)) {
		arOut = pModel;
		return true;
	}
	return false;
}

// 0x45A5E0
void ModelLoader::Unload(const char* apPath) {
	ReleaseModel(apPath, true, 1);
}

// 0x445300
void ModelLoader::ReleaseModel(const char* apPath, bool abRemove, int unk) {
	ThisStdCall(0x445300, this, apPath, abRemove, unk);
}

// 0x4436C0
bool ModelLoader::QueueTexture(const char* apPath, IO_TASK_PRIORITY aePriority, QueuedFile* apFile) {
	return ThisStdCall<bool>(0x4436C0, this, apPath, aePriority, apFile);
}

// 0x443AF0
bool ModelLoader::QueueTexture(BSFileEntry* apFileEntry, IO_TASK_PRIORITY aePriority, QueuedFile* apFile) {
	return ThisStdCall<bool>(0x443AF0, this, apFileEntry, aePriority, apFile);
}

// 0x446CB0
void ModelLoader::DisplayDebugLoadingText() {
#if 1
	return;
#else
	IOManager* pIOManager = IOManager::GetSingleton();

	UInt32 uiRefCount = GetReferenceCount();
	UInt32 uiTaskCount = GetTotalTaskCount();
	UInt32 uiQueuedCount = pIOManager->GetQueuedCount();
	UInt32 uiPostProcessCount = pIOManager->GetPostProcessQueueCount();
    if (uiQueuedCount || uiTaskCount || uiRefCount || uiPostProcessCount) {
		_MESSAGE("[ ModelLoader::DisplayDebugLoadingText ] Loading %i references (%i background cloning) and %i tasks (%i post processing)",
			uiRefCount,
			uiTaskCount,
			uiQueuedCount,
			uiPostProcessCount);
    }
#endif
}

// 0x448C10
bool ModelLoader::UpdateAttachDistant3DQueue() {
#if 0
	return ThisStdCall<bool>(0x448C10, this);
#else
	NiPointer<IOTask> spTask;
	ThisStdCall(0x449280, pAttachDistant3DTaskQueue, &spTask); // IOManager::ProcessQueue::Pop
	if (spTask) {
		_MESSAGE("\n[ ModelLoader::UpdateAttachDistant3DQueue ] Processing tasks...");
		while (spTask) {
			char v7[13000];
			memset(v7, 0, 13000);
			if (spTask->GetDescription(v7, 13000)) {
				_MESSAGE("[ ModelLoader::UpdateAttachDistant3DQueue ] %s", v7);
			}
			spTask->PostProcess();
			ThisStdCall(0x449280, pAttachDistant3DTaskQueue, &spTask);
		}
	}
	return true;
#endif
}
