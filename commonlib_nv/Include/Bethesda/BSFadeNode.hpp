#pragma once

#include "NiNode.hpp"
#include "BSSimpleList.hpp"

class TESObjectREFR;

NiSmartPointer(BSFadeNode);

enum ENUM_LOD_MULT : UInt32 {
	LOD_MULT_NONE				= 0,
	LOD_MULT_OBJECTS			= 1,
	LOD_MULT_ITEMS				= 2,
	LOD_MULT_ACTORS				= 3,
	LOD_MULT_TREES				= 4,
	LOD_MULT_LANDSCAPE			= 5,
	LOD_MULT_DISTANTLOD			= 6,
	LOD_MULT_ACTOR_BODY_PART	= 7,
	LOD_MULT_RENDEREDMENU		= 8,
	LOD_MULT_INVISIBLE			= 9,
	LOD_MULT_IMPOSTER			= 10,
	LOD_MULT_TOTAL,
};

class BSFadeNode : public NiNode {
public:
	BSFadeNode();
	~BSFadeNode();

	float				fNearDistSqr;
	float				fFarDistSqr;
	float				fLastFade;
	float				fCurrentFade;
	float				fBoundRadius;
	float				fTimeSinceUpdate;
	ENUM_LOD_MULT		eMultType;
	UInt32				uiFrameCounter;
	TESObjectREFR*		pLinkedObj;
	UInt32				unkD0;
	BSSimpleList<void*> unkD4;
	BSSimpleList<void*> unkDC;

	CREATE_OBJECT(BSFadeNode, 0xB4EAA0);
	NIRTTI_ADDRESS(0x11F9140);

	void OnVisibleEx(NiCullingProcess* apCuller);

	static void SetFadeEnabled(bool bEnabled) { *(bool*)0x11AD7B4 = bEnabled; }
	static bool IsFadeEnabled() { return *(bool*)0x11AD7B4; }
	static void SetFadeOutMultiplier(ENUM_LOD_MULT aeType, float afMult);
	static float GetFadeOutMultiplier(ENUM_LOD_MULT aeType);
	static float GetDistanceMultiplier() { return *(float*)0x11AD7F4; }
	static float GetFadeOutTime() { return *(float*)0x11AD7E8; }
	static float GetFadeInTime() { return *(float*)0x11AD7E4; }
	static UInt32 GetFadeCounter() { return *(UInt32*)0x11F9138; }

	static void SetAlphaRecurse(NiNode* apNode, float afAlpha);

	float GetLinkedActorAlpha();

	void UpdateSelectedDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags);

	struct FadeToggler {
		bool bOrgValue;

		FadeToggler(bool abVal = false) {
			bOrgValue = BSFadeNode::IsFadeEnabled();
			BSFadeNode::SetFadeEnabled(abVal);
		}

		~FadeToggler() {
			BSFadeNode::SetFadeEnabled(bOrgValue);
		}
	};
};

ASSERT_SIZE(BSFadeNode, 0xE4);