#pragma once

#include "Actor.hpp"
#include "BipedAnim.hpp"

class TESObjectARMO;

class Character : public Actor {
public:
	static inline auto bs_rtti = RTTI_PlayerCharacter;

	Character();
	virtual ~Character();

	virtual void		InitiateCannibalPackage(Actor* apTarget);
	virtual void		Unk_138(void);

	BipedAnim*	pBipedAnim;
	float		fTotalArmorDR;
	float		fTotalArmorDT;
	UInt8		bIsTrespassing;
	UInt8		bIsGuard;
	UInt16		unk1C2;
	float		fFlyInventoryWeight;
};

ASSERT_SIZE(Character, 0x1C8);