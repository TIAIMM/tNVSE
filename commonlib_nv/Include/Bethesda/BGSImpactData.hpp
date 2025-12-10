#pragma once

#include "TESForm.hpp"
#include "TESModel.hpp"

class BGSTextureSet;
class TESSound;

struct DecalData {
	enum Flags {
		PARALLAX	= 1 << 0,
		ALPHA_BLEND = 1 << 1,
		ALPHA_TEST	= 1 << 2
	};
	float		fDecalMinWidth;
	float		fDecalMaxWidth;
	float		fDecalMinHeight;
	float		fDecalMaxHeight;
	float		fDepth;
	float		fShininess;
	float		fParallaxScale;
	UInt8		ucParallaxPasses;
	Bitfield8	ucFlags;
	UInt32		uiColor;
};

ASSERT_SIZE(DecalData, 0x024);

class BGSImpactData : public TESForm, public TESModel {
public:
	BGSImpactData();
	virtual ~BGSImpactData();

	enum Orientation {
		ORIENT_SURFACE_NORMAL	= 0,
		ORIENT_PROJ_VECTOR		= 1,
		ORIENT_PROJ_REFLECT		= 2,
		ORIENT_COUNT			= 3
	};

	struct Data {
		float		fEffectDuration;
		Orientation	eOrient;
		float		fAngleThreshold;
		float		fPlacementRadius;
		UInt32		eSoundLevel;
		UInt32		uiFlags;
	};

	Data			kData;
	BGSTextureSet*	pTextureSet;
	TESSound*		pSound1;
	TESSound*		pSound2;
	DecalData		kDecalData;
};

ASSERT_SIZE(BGSImpactData, 0x78);