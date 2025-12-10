#pragma once

#include "AttachDistant3DTask.hpp"
#include "BackgroundCloneThread.hpp"
#include "IOManager.hpp"
#include "QueuedAnimIdle.hpp"
#include "QueuedHelmet.hpp"
#include "QueuedReplacementKFList.hpp"
#include "QueuedTexture.hpp"
#include "LoadedFile.hpp"
#include "LockFreeMap.hpp"

class Animation;
class AnimIdle;
class BSFileEntry;
class KFModel;
class Model;
class TESObjectREFR;

class ModelLoader {
public:
	ModelLoader();
	~ModelLoader();

	LockFreeCaseInsensitiveStringMap<Model*>*						pModelMap;
	LockFreeCaseInsensitiveStringMap<KFModel*>*						pKFModelMap;
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedReference> >*		pQueuedReferenceMap1;
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedReference> >*		pQueuedReferenceMap2;
	LockFreeMap<AnimIdle*, NiPointer<QueuedAnimIdle>>*				pQueuedAnimIdleMap;
	LockFreeMap<Animation*, NiPointer<QueuedReplacementKFList>>*	pQueuedReplacementKFListMap;
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedHelmet>>*			pQueuedHelmetMap;
	LockFreeQueue<NiPointer<AttachDistant3DTask>>*					pAttachDistant3DTaskQueue;
	LockFreeMap<BSFileEntry*, NiPointer<QueuedTexture>>*			pQueuedTextureMap;
	LockFreeCaseInsensitiveStringMap<LoadedFile*>*					pLoadingFileMap;
	BackgroundCloneThread*											pBackgroundCloneThread;
	bool															bHasDelayedFree;

	static ModelLoader* GetSingleton();

	UInt32 GetReferenceCount();
	UInt32 GetTotalTaskCount();

	bool AddLoadedFile(const char* apPath, LoadedFile* apLoadedFile);
	void RemoveLoadedFile(const char* apPath);

	void QueueReference(TESObjectREFR* apRefr, IO_TASK_PRIORITY aePriority, bool abAllowQueueReferenceQueuing);

	NiNode* LoadFile(const char* apPath, UInt32 aeLODFadeMult, bool abAssignShaders, int unused, bool abKeepUV, bool abNoUseCountIncrease);

	bool LookupModel(const char* apPath, NiPointer<Model>& arOut);

	void Unload(const char* apPath);
	void ReleaseModel(const char* apPath, bool abRemove, int unk);

	bool QueueTexture(const char* apPath, IO_TASK_PRIORITY aePriority, QueuedFile* apFile);
	bool QueueTexture(BSFileEntry* apFileEntry, IO_TASK_PRIORITY aePriority, QueuedFile* apFile);

	void DisplayDebugLoadingText();
	bool UpdateAttachDistant3DQueue();
};

ASSERT_SIZE(ModelLoader, 0x30)