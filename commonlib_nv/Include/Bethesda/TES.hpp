#pragma once

#include "GridCellArray.hpp"
#include "IOTask.hpp"
#include "Sky.hpp"
#include "TESWaterSystem.hpp"

class TESCreature;
class NavMeshInfoMap;
class LoadedAreaBound;
class NiFogProperty;
class TESWorldSpace;
class ImageSpaceModifierInstance;
class TESObjectREFR;
class QueuedFile;
class GridDistantArray;
class BSTempNodeManager;
class bhkPickData;

class TES {
public:
	virtual bool GetMapNameForLocation(BSString& arName, NiPoint3 akLocation, TESWorldSpace* apWorldSpace) const;

	struct DeathCount {
		TESCreature*	creatureOrNPC;
		UInt16			count;
	};

	GridDistantArray*									pGridDistantArray;
	GridCellArray*										pGridCellArray;
	NiNode*												pObjRoot;
	NiNode*												pLODRoot;
	NiNode*												pObjLODWaterRoot;
	BSTempNodeManager*									pTempNodeManager;
	NiDirectionalLight*									pObjLight;
	NiFogProperty*										pObjFog;
	SInt32												iCurrentGridX;
	SInt32												iCurrentGridY;
	SInt32												iCurrentQueuedX;
	SInt32												iCurrentQueuedY;
	TESObjectCELL*										pInteriorCell;
	TESObjectCELL**										pInteriorBuffer;
	TESObjectCELL**										pExteriorBuffer;
	UInt32												uiTempInteriorBufferSize;
	UInt32												uiTempExteriorBufferSize;
	SInt32												iSaveGridX;
	SInt32												iSaveGridY;
	UInt8												byte50;
	bool												bRunningCellTests;
	UInt8												byte52;
	UInt8												gap53;
	UInt32												renderTestCellsCallback;
	UInt32												unk58;
	UInt32												unk5C;
	bool												bShowLandBorders;
	TESWaterSystem*											pWaterManager;
	Sky*												pSky;
	BSSimpleList<NiPointer<ImageSpaceModifierInstance>> kActiveIMODs;
	UInt32												unk74;
	UInt32												unk78;
	UInt8												byte7C;
	UInt8												byte7D;
	UInt8												gap7E;
	UInt8												gap7F;
	float												fCell_delta_x;
	float												fCell_delta_y;
	TESWorldSpace*										pWorldSpace;
	UInt32*												list8C[2];
	BSSimpleList<TESObjectREFR*>						cellFurnitureList;
	BSSimpleList<DeathCount*>							deathCounts;
	NiPointer<QueuedFile>								spQueuedFileA4;
	NiSourceTexturePtr									spBloodDecalPreload1;
	NiPointer<QueuedFile>								spQueuedFileAC;
	void*												pParticleCacheHead;
	bool												bFadeWhenLoading;
	bool												bAllowUnusedPurge;
	UInt8												byteB6;
	UInt8												byteB7;
	UInt32												iPlaceableWaterCount;
	NavMeshInfoMap*										pNavMeshInfoMap;
	NiPointer<LoadedAreaBound>							spLoadedAreaBound;

	static TES* GetSingleton();

	static Sky* GetSky();

	static TESWaterSystem* GetWaterManger();

	static TESWorldSpace* GetWorldSpace();

	static TESObjectCELL* GetInterior();
	TESObjectCELL* GetCurrentCell();
	TESObjectCELL* GetGridCellCell(SInt32 aiX, SInt32 aiY);

	NavMeshInfoMap* GetNavMeshInfoMap();

	void ClearAllCells(bool abBufferedonly, bool abCurrentCell);

	void InitVars();

	void AddTempDebugObject(NiAVObject* apObject, float afTime);

	bool IsCellLoaded(TESObjectCELL* apCell, bool abIgnoreBuffered);
	IO_TASK_PRIORITY GetCellPriority(TESObjectCELL* apCell, NiPoint3* apPos);

	bool CanAttach3D(TESObjectREFR* apRef);

	GridCell* GetGridCell(SInt32 aX, SInt32 aY);

	bool GetLandHeight(const NiPoint3& akPosition, float* afHeight) const;
	NiGeometry* GetLandGeometry(const NiPoint2& arPosition) const;

	void CreateTextureImage(const char* apPath, NiSourceTexturePtr& aspTexture, bool abNoFileOK, bool abArchiveOnly);
	void SaveTextureImage(NiTexture* apTexture, const char* apPath, D3DXIMAGE_FILEFORMAT aeFormat = D3DXIFF_PNG);

	bool FreeTexture(const char* apPath, UInt32 auiMinRefCount = 0);

	void UpdateMultiBoundVisibility(NiCamera* apCamera);
	void ResetAllMultiBoundNodes(bool abLandscape, bool abRooms);

	NiAVObject* Pick(bhkPickData* apPickData, bool abHavok = true) const;

	void HandleCellLoopingSounds();

	NiObject* CloneNiObject(NiObject* apObject, NiCloningProcess& arCloneProc);
	static void CloneModelData(NiObject* apObject);

	void UpdateRefAnimations();
};

ASSERT_SIZE(TES, 0xC4);

extern TES* g_TES;