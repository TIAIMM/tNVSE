#pragma once

#include "BSTempEffect.hpp"

class ActiveEffect;
class TESObjectREFR;

NiSmartPointer(MagicHitEffect);

class MagicHitEffect : public BSTempEffect {
public:
	MagicHitEffect();
	~MagicHitEffect();

	virtual bool	Unk_31();
	virtual void	Unk_32();
	virtual void	Unk_33();
	virtual UInt16	GetSaveSize(ActiveEffect* apEffect, TESObjectREFR* apRef) const;
	virtual void	Unk_35();
	virtual void	LoadGame(ActiveEffect* apEffect, TESObjectREFR* apRef);
	virtual void	InitLoadGame(ActiveEffect* apEffect, TESObjectREFR* apRef);
	virtual void	Unk_38();

	ActiveEffect*	pActiveEffect;	// 18
	TESObjectREFR*	pTarget;		// 1C
	float			fUnk20;			// 20	Init'd from ActiveEffect.timeElapsed
	Bitfield8		ucFlags;
};