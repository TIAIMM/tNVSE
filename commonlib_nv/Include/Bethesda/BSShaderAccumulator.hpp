#pragma once

#include "BSBatchRenderer.hpp"
#include "BSGraphics.hpp"
#include "NiAlphaAccumulator.hpp"
#include "ShadowSceneLight.hpp"

NiSmartPointer(BSShaderAccumulator)

class BSShaderProperty;

class BSShaderAccumulator : public NiAlphaAccumulator {
public:

	virtual void FinishAccumulatingPreResolveDepth();
	virtual void FinishAccumulatingPostResolveDepth();
	virtual void Func_32(UInt32, UInt32);

	enum BATCHRENDERER_CREATION_MODE {
		BRCM_NORMAL			= 99,
		BRCM_NO_GROUPS		= 100,
		BRCM_ALPHA_GROUP	= 101,
	};

	BSShaderAccumulator(BATCHRENDERER_CREATION_MODE aeBatchRendererCreationType, UInt32 batchRendererCount, BSShaderManager::RenderPassType maxPassCount);
	virtual ~BSShaderAccumulator();

	struct ActorOcclusionTest {
		NiBound							kBound;
		UInt32							uiRefID;
		BSGraphics::OcclusionQuery*		pOcclusionQuery;
		bool							bIsVisible;
		bool							bQuerySent;
		bool							bEnabled;
		DWORD							uiPixelCount;
	};

	struct SunOcclusionTest {
		BSGraphics::OcclusionQuery*		pOcclusionQuery;
		bool							bWaiting;
		float							fPercentOccluded;
		UInt32							uiPixelCount;
		UInt32							uiFrameCount;
	};

	UInt32									unk034;
	bool									bWireFrameRelated;
	bool									unk039;
	bool									bRenderDOF;
	UInt32									uiSunPixelCount;
	bool									bWaitingForSunQuery;
	float									fPercentSunOccludedStored;
	SunOcclusionTest						kSunTests[3];
	bool									bProcessLights;
	bool									bIs1stPerson;
	bool									unk086;
	UInt16									unk088;
	UInt16									usQueryCount;
	BSGraphics::OcclusionQuery**			ppOcclusionQueries;
	NiTPointerList<NiGeometry*>				kVolumetricFogGeoList;
	PersistentPassList						passList_09C[4];
	PersistentPassList						passList_0EC[4];
	NiGeometryPtr							spOcclusionTestShape;
	NiTPointerList<ActorOcclusionTest>		kActorOcclusionTestList;
	UInt32									uiLastRefID;
	bool									bLastCullResult;
	NiColorA								kAmbientColor;
	bool									bIgnoreWater;
	bool									bRenderWaterDepth;
	bool									bUseUnderwaterGroup; // Water height related
	bool									bIsUnderwater;
	SInt32									iWaterHeight;
	bool									bRenderDecals;
	UInt32									unk170;
	BSBatchRenderer*						pBatchRenderers[2];
	BSShaderManager::RenderPassType*		pPasses;
	BSBatchRenderer::TransparencyType*		pTransparencies;
	bool*									pPassStates;
	bool*									bPassFinishStates;
	UInt32									uiBatchRendererCount;
	UInt32									uiMaxPassCount;
	ShadowSceneNode*						pActiveShadowSceneNode;
	NiObjectPtr								spObject198;
	BSShaderManager::RenderMode				eRenderMode;
	ShadowSceneLightPtr						spDepthLght1A0;
	BSShaderProperty::RenderPass			kTileRenderPass;
	NiTPointerMap<NiAVObject*, bool>		kRoomSharedMap;
	UInt32									unk1C4;
	UInt32									unk1C8;
	UInt32									unk1CC;
	UInt32									unk1D0;
	UInt32									unk1D4;
	UInt32									unk1D8;
	UInt32									unk1DC;
	UInt32									unk1E0;
	UInt32									unk1E4;
	UInt32									unk1E8;
	UInt32									unk1EC;
	UInt32									unk1F0;
	UInt32									unk1F4;
	UInt32									unk1F8;
	UInt32									unk1FC;
	NiCriticalSection						kCriticalSection;
	UInt32									unk220;
	UInt32									unk224;
	UInt32									unk228;
	UInt32									unk22C;
	UInt32									unk230;
	UInt32									unk234;
	UInt32									unk238;
	UInt32									unk23C;
	UInt32									unk240;
	UInt32									unk244;
	UInt32									unk248;
	UInt32									unk24C;
	UInt32									unk250;
	UInt32									unk254;
	UInt32									unk258;
	UInt32									unk25C;
	UInt32									unk260;
	UInt32									unk264;
	UInt32									unk268;
	UInt32									unk26C;
	UInt32									unk270;
	UInt32									unk274;
	UInt32									unk278;
	UInt32									unk27C;

	NIRTTI_ADDRESS(0x11FA000)

	static BSShaderAccumulator* Create(BATCHRENDERER_CREATION_MODE aeBatchRendererCreationType = BRCM_NORMAL, UInt32 batchRendererCount = 1, BSShaderManager::RenderPassType maxPassCount = BSShaderManager::BSSM_FINAL_PASS) {
		return NiCreate<BSShaderAccumulator, 0xB660D0>(aeBatchRendererCreationType, batchRendererCount, maxPassCount);
	}

	bool RegisterObjectEx(NiGeometry* apGeo);
	bool AddSharedEx(NiAVObject* apObject);
	void ClearSharedMapEx();
	void FinishAccumulatingPreResolveDepthEx();
	void FinishAccumulatingPostResolveDepthEx();

	static void __cdecl FinishAccumulating_Standard(BSShaderAccumulator* apAccumulator);
	static void __cdecl FinishAccumulating_Standard_PreResolveDepth(BSShaderAccumulator* apAccumulator);
	static void __cdecl FinishAccumulating_Standard_PostResolveDepth(BSShaderAccumulator* apAccumulator);
	static void __cdecl FinishAccumulating_NoWater(BSShaderAccumulator* apAccumulator);
	static void __cdecl FinishAccumulating_WaterDepth(BSShaderAccumulator* apAccumulator);
	static void __cdecl FinishAccumulating_Tiles(BSShaderAccumulator* apAccumulator);
	static bool __cdecl RegisterObject_Standard(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_Tiles(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_LocalMap(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_WaterDepth(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_ConstAlpha(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_Unk13(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);
	static bool __cdecl RegisterObject_Depth(BSShaderAccumulator* apAccumulator, NiGeometry* apGeo, const NiPropertyState* apProperties, BSShaderProperty* apShaderProp, BSShader* apShader);	
	void RenderBatches(BSShaderManager::RenderPassType aeStart, BSShaderManager::RenderPassType aeEnd, bool abAlphaPass = false, BSBatchRenderer::GroupType aeGeometryGroup = BSBatchRenderer::GROUP_NONE);
	void RenderGeometryGroup(BSBatchRenderer::GroupType aeGeometryGroup, bool abAlphaPass);
	void RenderBlendedDecals();
	bool RenderVolumetricFog(NiCamera* apCamera, BSRenderedTexture* apTexture);
	void RenderTiles();
	void RenderLOD();
	void RenderAlphaGeometry(BSBatchRenderer::AlphaGroupType aeGeometryGroup);
	void SortAlpha();
	void RenderScreenSplatter();
	void RenderRefraction(NiCamera* apCamera, BSRenderedTexture* apTexture);
	void Unk_B65660();
	void ClearRenderPasses();
	void ClearBatchPasses();
	void ClearGroupPasses(UInt32 auiGroup);
	void RenderDepth(NiCamera* apCamera1, NiCamera* apCamera2);
	void StartGroupingAlphas(NiBound* apBound, bool abZPrePass);
	void StopGroupingAlphas();
	void FreeCurrentBatch();

	void DoSunOcclusionQuery(NiTriShape* apSunGeometry, UInt32 aiTestNumber, NiCamera* apCamera);
	void EvaluateSunOcclusionResults(UInt32 aiTestNumber);
	void ResetSunOcclusion();

	bool GetPassesWithinRange(BSShaderManager::RenderPassType aeStartRange, BSShaderManager::RenderPassType aeEndRange);
	bool GetWaterPassesWithinRange(BSShaderManager::RenderPassType aeStartRange, BSShaderManager::RenderPassType aeEndRange);

	__forceinline void SetActiveShadowSceneNode(ShadowSceneNode* apShadowSceneNode) { pActiveShadowSceneNode = apShadowSceneNode; };
	__forceinline ShadowSceneNode* GetActiveShadowSceneNode() const { return pActiveShadowSceneNode; };

	__forceinline void SetIsUnderwater(bool abIsUnderwater) { bIsUnderwater = abIsUnderwater; };
	__forceinline bool IsUnderwater() const { return bIsUnderwater; };

	__forceinline void SetWaterHeight(SInt32 aWaterHeight) { iWaterHeight = aWaterHeight; };
	__forceinline SInt32 GetWaterHeight() const { return iWaterHeight; };

	__forceinline void SetRenderDecals(bool abRenderDecals) { bRenderDecals = abRenderDecals; };
	__forceinline bool GetRenderDecals() const { return bRenderDecals; };

	__forceinline void SetRenderDOF(bool abRenderDOF) { bRenderDOF = abRenderDOF; };
	__forceinline bool GetRenderDOF() const { return bRenderDOF; };


	__forceinline void SetRenderMode(BSShaderManager::RenderMode aeRenderMode) { eRenderMode = aeRenderMode; };
	__forceinline void SetRenderMode(UInt32 aiRenderMode) { eRenderMode = (BSShaderManager::RenderMode)aiRenderMode; };
	__forceinline BSShaderManager::RenderMode GetRenderMode() const { return eRenderMode; };

	__forceinline void SetUseUnderwaterGroup(bool abValue) { bUseUnderwaterGroup = abValue; };
	__forceinline bool GetUseUnderwaterGroup() const { return bUseUnderwaterGroup; };

	ShadowSceneLight* GetSunLight() const;

	static UInt32 uiTotalLODStripCount;
	static UInt32 uiTotalLODTriCount;
	static UInt32 uiTotalLODVertCount;

	static bool bSkipAlpha;
#ifdef _DEBUG
	static bool bFrozenBatchAcquired;
	static bool bFreezeGeometryBatch;
#endif
	static bool bShowDecals;

	typedef bool(__cdecl* FN_RegisterObject)(BSShaderAccumulator*, NiGeometry*, NiPropertyState*, BSShaderProperty*, BSShader*);
	typedef void(__cdecl* FN_FinishAccumulating)(BSShaderAccumulator*);

	static FN_RegisterObject* const pRegisterObjectFunc;
	static FN_FinishAccumulating* const pFinishAccumulatingFunc;

	static FN_RegisterObject* const pRegisterObjectCurrent;
	static FN_FinishAccumulating* const pFinishAccumulatingCurrent;
};

ASSERT_SIZE(BSShaderAccumulator, 0x280);