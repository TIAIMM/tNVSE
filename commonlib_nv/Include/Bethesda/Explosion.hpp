#pragma once

#include "MobileObject.hpp"

class NiPointLight;
class NonActorMagicCaster;
class bhkSimpleShapePhantom;
class BSSoundHandle;
class ActorCause;

class Explosion : public MobileObject {
public:
	static inline auto bs_rtti = RTTI_Explosion;

	float unk088;
	float unk08C;
	float fInnerRadius;
	float fOuterRadius;
	float fImageSpaceRadius;
	float fDamageMult;
	NiPointer<bhkSimpleShapePhantom> spPhantom0A0;
	BSSimpleList<void*> kTargets;
	BSSoundHandle* pSoundHandles[6];
	NiPointer<NiPointLight> spLight;
	TESObjectREFR* pOwnerRef;
	TESObjectREFR* pRefCC;
	NiPointer<ActorCause> spActorCause;
	bool bDecalsPlaced;
	bool bTargetsFound;
	bool bTargetsProcessed;
	bool bForcesApplied;
	bool bSkipSwap;
	bool bUnderwater;
	NonActorMagicCaster* pCaster;
	int gapE0;
	int iFrameCount;
	NiPoint3 kClosestPoint;
	NiPoint3 kClosestPointNormal;
	float fCalculatedDamage;

	static NiTPointerList<TESObjectREFR*>* GetWaterReflectedRefs();
};

ASSERT_SIZE(Explosion, 0x104)
