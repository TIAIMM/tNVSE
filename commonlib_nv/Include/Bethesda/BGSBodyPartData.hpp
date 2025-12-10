#pragma once

#include "TESForm.hpp"
#include "TESModel.hpp"
#include "BGSPreloadable.hpp"

class BGSBodyPart;
class BGSRagdoll;

class BGSBodyPartData : public TESForm, public TESModel, public BGSPreloadable {
public:
	BGSBodyPartData();
	~BGSBodyPartData();

	enum
	{
		eBodyPart_Torso = 0,
		eBodyPart_Head1,
		eBodyPart_Head2,
		eBodyPart_LeftArm1,
		eBodyPart_LeftArm2,
		eBodyPart_RightArm1,
		eBodyPart_RightArm2,
		eBodyPart_LeftLeg1,
		eBodyPart_LeftLeg2,
		eBodyPart_LeftLeg3,
		eBodyPart_RightLeg1,
		eBodyPart_RightLeg2,
		eBodyPart_RightLeg3,
		eBodyPart_Brain,
		eBodyPart_Weapon,
	};

	BGSBodyPart*	pBodyParts[15];
	BGSRagdoll*		pRagDoll;

	BGSBodyPart* GetBodyPart(const uint32_t a2) {
		return ThisStdCall<BGSBodyPart*>(0x5E50F0, this, a2);
	}
};

ASSERT_SIZE(BGSBodyPartData, 0x74);