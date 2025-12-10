#pragma once

#include "NiNode.hpp"
#include "NiTPointerMap.hpp"
#include "BSSimpleList.hpp"
#include "TESAnimGroup.hpp"

class Actor;
class AnimSequenceBase;
class BSAnimGroupSequence;
class TESIdleForm;

NiSmartPointer(AnimIdle);
NiSmartPointer(NiControllerManager);

class Animation : public BSMemObject {
public:
	enum Bones {
		PELVIS		= 0,
		BIP01COPY	= 1,
		LFOREARM	= 2,
		HEAD		= 3,
		WEAPON		= 4,
		UNUSED_5	= 5,
		UNUSED_6	= 6,
		NECK1		= 7,
		COUNT
	};

	Bitfield32									uiFlags;
	Actor*										pActor;
	NiNodePtr									spSceneRoot;
	NiNode*										pBip01;
	NiPoint3									kMovementVector;
	NiPoint3									kBip01Pos;

	union {
		struct {
			NiNode* pPelvis;
			NiNode* pBip01Copy;
			NiNode* pLForearm;
			NiNode* pHead;
			NiNode* pWeapon;
			NiNode* pUNUSED03C;
			NiNode* pUNUSED040;
			NiNode* pNeck1;
		};

		NiNode* pNodes[Bones::COUNT];
	};

	float										fUnk048;
	TESAnimGroup::Type							usGroupIDs[8];
	UInt32										uiActions[8];
	UInt32										sequenceState2[8];
	TESAnimGroup::Type							usGroupToMorphTo[8];
	UInt32										unk0AC[8];
	UInt8										byte0CC;
	UInt8										byte0CD;
	UInt8										byte0CE;
	UInt8										byte0CF;
	float										fTimePassed;
	float										fTimeUnk0D4;
	NiControllerManagerPtr						spControllerManager;
	NiTPointerMap<UInt16, AnimSequenceBase*>*	pAnimSequenceBases;
	BSAnimGroupSequence*						pAnimSequence[8];
	BSAnimGroupSequence*						pAnimSeq100;
	BSSimpleList<void*>							kAnimationCloneList;
	float										fMovementSpeedMult;
	float										fRateOfFire;
	float										fTurboSpeedMult;
	float										fWeaponReloadSpeed;
	float										fEquipSpeed;
	UInt8										byte120;
	UInt8										byte121;
	UInt16										usQueuedReloadGroup;
	AnimIdlePtr									spIdleAnim;
	AnimIdlePtr									spIdleAnimQueued;
	AnimIdlePtr									spIdleAnims12C[2];
	BSSimpleList<TESIdleForm*>					kList134;

	void Delete(bool abFree);

	bool GetDelta(NiPoint3& arVector, const Actor* apActor, bool abNoRotation, bool abNoHeight);
};

ASSERT_SIZE(Animation, 0x13C)