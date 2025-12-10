#pragma once

#include "NiTPointerMap.hpp"
#include "BGSSaveLoadHistory.hpp"
#include "BGSSaveLoadFormIDMap.hpp"

class BGSCellNumericIDArrayMap;
class BGSLoadGameSubBuffer;
class BGSReconstructFormsInFileMap;
class BGSReconstructFormsInAllFilesMap;
class BGSLoadFormBuffer;
class BGSSaveLoadChangesMap;
class BGSSaveLoadFile;
class BGSSaveLoadReferencesMap;
class Actor;
class TESObjectCELL;
class TESFile;
class BGSSaveLoadQueuedSubBufferMap;

class BGSSaveLoadGame {
public:
	BGSSaveLoadGame();
	~BGSSaveLoadGame();

	BGSSaveLoadChangesMap*						pChangesMap;
	BGSSaveLoadChangesMap*						pOldChangesMap;
	BGSSaveLoadFormIDMap*						pFormIDMap;
	BGSSaveLoadFormIDMap*						pWorldspaceFormIDMap;
	BGSSaveLoadReferencesMap*					pReferencesMap;
	BGSSaveLoadQueuedSubBufferMap*				pQueuedSubBuffersMap;
	NiTMap<UInt32, UInt32>*						pChangedFormIDMap;
	BGSSaveLoadHistory*							pHistory;
	BGSReconstructFormsInAllFilesMap*			pReconstructForms;
	BSSimpleArray<BGSLoadFormBuffer*>			kChangedForms;
	NiTMap<UInt32, Actor*>						kQueuedInitPackageLocationsActorMap;
	UInt8										ucSaveMods[255];
	UInt8										ucLoadedMods[255];
	UInt32										uiGlobalFlags;
	UInt8										ucCurrentMinorVersion;

	static BGSSaveLoadGame* GetSingleton();

	bool LoadPluginList(BGSSaveLoadFile* apFile);
	void SavePluginList(BGSSaveLoadFile* apFile);

	UInt32 GetConvertedFormID(UInt32 auiFormID);
	UInt8 GetSaveMod(UInt8 aucIndex) const;

	bool IsLoading();
	bool IsDeferInitForms();

	bool SetThreadAllowChanges(bool abEnable);
	void InitForms(bool abLowPriority);
	void LoadCell(TESObjectCELL* apCell);
};

ASSERT_SIZE(BGSSaveLoadGame, 0x24C);