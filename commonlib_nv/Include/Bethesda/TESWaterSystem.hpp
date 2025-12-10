#pragma once

#include "BSShaderAccumulator.hpp"
#include "BSSoundHandle.hpp"
#include "TESObjectREFR.hpp"

class TESWaterForm;
class WadingWaterData;
class Actor;

class TESWaterSystem {
public:
	TESWaterSystem();
	~TESWaterSystem();

	struct WaterGroup {
		TESWaterForm* pWaterForm;
		NiPlane kMainPlane;
		NiPlane kInvPlane;
		NiTPointerList<TESObjectREFR*> kWaterPlanes;
		NiTPointerList<TESObjectREFR*> kRefsInWater;
		NiTPointerList<Actor*> kActorsInWater;
		NiTPointerList<UInt32> kList048;
		BSRenderedTexturePtr spReflectionTexture;
		NiAVObjectPtr spRef058;
		bool bUsesWaterLevel;
		bool bIsVisible;
		bool bRenderDepth;
		bool bIsInterior;
		bool bAllowLowDetailReflections;
		NiTPointerList<NiNode*> kGeometryGroup;
		NiTPointerList<NiNode*> kExplosionList;
		NiTPointerList<NiNode*> kDepth_CellGeometryList;
		NiTPointerList<NiNode*> kList088;
		BSShaderAccumulatorPtr spShaderAccum;
		BSShaderAccumulatorPtr spDepthShaderAccum;
		UInt32 uiReflectionGroupCount;
		UInt32 uiDepthGroupCount;
		NiCameraPtr spCamera;
		NiCameraPtr spDepthCamera;
		UInt32 uiStencilMask;

		static WaterGroup* CreateObject();

		NiTPointerList<TESObjectREFR*>& GetPlanes() {
			return kWaterPlanes;
		};
	};

	struct WadingWaterData {
		NiPoint2 pair0;
		NiPoint2 pair8;
		NiPoint3 kPoint3;
	};


	UInt32 uiNumWaterGroups;
	UInt32 uiUnk004;
	NiObjectPtr spRT008;
	NiObjectPtr spRef00C;
	NiObjectPtr spRt010;
	NiObjectPtr spRt014;
	NiObjectPtr spRef018;
	NiSourceTexturePtr spWaterNoiseTexture;
	NiObjectPtr spRef020;
	float fBlendTimer;
	NiPoint2 kLastDisplaceOffset;
	UInt32 uiUnk30;
	BYTE bUnk34;
	float fUnk38;
	NiTPointerList<WaterGroup*> kWaterGroups;
	WaterGroup* pWaterLOD;
	NiTPointerMap<TESObjectREFR*, TESObjectREFR*> kRefMap04C;
	NiTPointerMap<TESObjectREFR*, TESObjectREFR*> kRefMap05C;
	NiTPointerMap<TESWaterForm*, bool> kWaterFormMap06C;
	NiTPointerMap<TESObjectREFR*, WadingWaterData*> kWadingWaterMap;
	BSSoundHandle hSound;
	float fLastSplashTime;
	bool bUnk09C;

	static bool bSkipNextUpdate;
	static bool* const bForceRender;
	static NiCamera* pCamera;
	static UInt32 uiActiveWaterGroups;
	static UInt32* const uiExteriorReflections;
	static UInt32* const uiCustomExteriorReflections;
	static bool* const bPauseTime;
	static NiPlane* const kWaterPlane;
	static NiPoint2* const kWaterFogDistances;

	static NiTPointerList<NiAVObject*> WaterLevelReflect_Nodes;
	static NiTPointerList<NiAVObject*> CustomWaterLevelReflect_Nodes;

	NiTPointerList<WaterGroup*>& GetWaterGroups() {
		return kWaterGroups;
	};

	static bool IsUnderwater();
	static void SetIsUnderwater(bool abIsUnderwater);

	static bool IsWaterEnabled();
	static void SetWaterEnabled(bool abEnabled);

	static float GetCameraHeight();
	static void SetCameraHeight(float afHeight);

	static bool IsInInterior();
	static void SetIsInInterior(bool abIsInInterior);

	static bool CanReflectAtCustomLvl();
	static void SetCanReflectAtCustomLvl(bool abCanReflect);

	static bool CanReflectAtWaterLvl();
	static void SetCanReflectAtWaterLvl(bool abCanReflect);

	static NiNode* GetWaterRoot() {
		return *reinterpret_cast<NiNodePtr*>(0x11C7C28);
	}

	// Worldspace height
	static NiCamera* GetWaterHeightCamera();
	static void SetWaterHeightCamera(NiCamera* apCamera);
	static BSShaderAccumulator* GetWaterHeightAccumulator();
	static void SetWaterHeightAccumulator(BSShaderAccumulator* apAccumulator);

	// Custom height
	static NiCamera* GetCustomWaterHeightCamera();
	static void SetCustomWaterHeightCamera(NiCamera* apCamera);
	static BSShaderAccumulator* GetCustomWaterHeightAccumulator();
	static void SetCustomWaterHeightAccumulator(BSShaderAccumulator* apAccumulator);

	// Textures
	static BSRenderedTexturePtr GetReflectionTexture() {
		return *reinterpret_cast<BSRenderedTexturePtr*>(0x11C7AD4);
	}

	static void SetReflectionTexture(BSRenderedTexture* apTexture) {
		*reinterpret_cast<BSRenderedTexturePtr*>(0x11C7AD4) = apTexture;
	}

	static BSRenderedTexturePtr GetReflectionTextureCustom();
	static void SetReflectionTextureCustom(BSRenderedTexture* apTexture);

	static BSRenderedTexturePtr GetDepthTexture() {
		return *reinterpret_cast<BSRenderedTexturePtr*>(0x11C7B68);
	}

	static void SetDepthTexture(BSRenderedTexture* apTexture) {
		*reinterpret_cast<BSRenderedTexturePtr*>(0x11C7B68) = apTexture;
	}

	static BSRenderedTexturePtr GetWadeTexture();
	static void SetWadeTexture(BSRenderedTexture* apTexture);

	static BSRenderedTexture* GetRefractionTexture();
	static void SetRefractionTexture(BSRenderedTexture* apTexture);


	void ExteriorReflections_Accumulate(NiCamera* apCamera);
	void ExteriorReflections_Render();

	void CustomExteriorReflections_Accumulate(NiCamera* apCamera);
	void CustomExteriorReflections_Render();

	static void CalculateCameraPosition(NiCamera* apWorldCamera, NiPlane* apWaterPlane, NiCamera* apWaterCamera);

	static bool IsLODPresent();

	static void SetCurrentPlane(NiPlane akPlane);
	static NiPlane* GetCurrentPlane();

	void GetNoiseTexture(NiSourceTexturePtr& aspTexture) const;
	void SetNoiseTexture(NiSourceTexture* apTexture);

	void UpdateWaterSystem(NiCamera* apCamera, NiNode* apNode, BSShaderAccumulator* apAccum = nullptr);
	bool IsWaterObjectVisible(TESObjectREFR* apRefr, NiCamera* apCamera);
	NiTriShape* GetGeometry(TESObjectREFR* apRefr);

	static void* __fastcall RenderSurface_Hook(NiAVObjectPtr* apThis);
	void RenderSurface(WaterGroup* apWaterGroup);

	void AddReflectionToGroup(WaterGroup* apWaterGroup);
	void RemoveReflectionFromGroup(WaterGroup* apWaterGroup);
	void UpdateLOD(NiCamera* apCamera, bool abForceDisplay);
	void RenderWaterPlane(NiCamera* apCamera, WaterGroup* apWaterGroup, UInt16 usStencilMask);
	void RenderWater(NiCamera* apCamera, bool abForceDisplay) {
		ThisStdCall(0x4E21B0, this, apCamera, abForceDisplay);
	}
	void Depth_Accumulate(NiCamera* apCamera, WaterGroup* apWaterGroup);
	void RenderDepthStencil(WaterGroup* apWaterGroup, UInt16 ausStencilMask);
	bool IsPlayerInWaterQuad(WaterGroup* apWaterGroup);
	bool ShouldHandleWaterGroupDisplacements();
	void HandleWaterGroupDisplacements();
	void RenderStaticRipples();
	void UpdateWaterSounds();

	void UpdateShaderForGroup(WaterGroup* apGroup);
	void UpdateShader(WaterGroup* apGroup, TESObjectREFR* apRefr);

	void SetClipPlanes(NiPlane akPlane, int unk);

	void InteriorReflections_Accumulate(NiCamera* apCamera, WaterGroup* apWaterGroup, bool abUseLowDetail);
	void InteriorReflections_Render(WaterGroup* apWaterGroup);

	bool EnableWaterSystem();
	void ToggleVisibility(bool abEnable, bool abUnk1, bool abToggle);

	void UpdateStencilMasks();

	void RemoveSubmergedReference(TESObjectREFR* apWaterRefr, Actor* apRefr, float afHeight);
	void RemoveSubmergedReferenceFromGroup(Actor* apRefr, WaterGroup* apWaterGroup);
	WaterGroup* GetWaterGroupForReference(TESObjectREFR* apWaterRef, TESObjectREFR* apLookupRefr, float afHeight) const;

	void AddWaterRef(TESObjectREFR* apRef, NiPoint3& arPos, NiMatrix3& arRot);

	TESObjectREFR* AddLODWater(NiGeometry* apLODWater, TESWorldSpace* apWorldSpace, NiNode* apWaterLODRoot, BSMultiBoundNode* apBound, bool abLODWaterHeight);
};

ASSERT_SIZE(TESWaterSystem, 0xA0);
ASSERT_SIZE(TESWaterSystem::WaterGroup, 0xB0);
