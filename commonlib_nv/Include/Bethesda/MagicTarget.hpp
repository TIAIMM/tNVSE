#pragma once

#include "BSSimpleList.hpp"

class TESObjectREFR;
class EffectItem;
class ActiveEffect;
class MagicCaster;
class MagicItem;

typedef BSSimpleList<ActiveEffect*> EffectList;

class MagicTarget {
public:
	MagicTarget();
	virtual bool		AddTarget(MagicCaster* apCaster, MagicItem* apItem, ActiveEffect* apEffect, bool);
	virtual Actor*		GetTargetAsActor() const;
	virtual EffectList* GetActiveEffectList() const;
	virtual bool		MagicTargetIsActor() const;
	virtual bool		GetTargetStatsObject();
	virtual void		EffectAdded(ActiveEffect* apEffect);
	virtual void		Unk_06();
	virtual void		EffectAbsorbed(MagicCaster* apCaster, ActiveEffect* apEffect);
	virtual void		EffectReflected(MagicCaster* apCaster, ActiveEffect* apEffect);
	virtual float		CheckResistance(MagicCaster* apCaster, MagicItem* apItem, ActiveEffect* apEffect);
	virtual bool		CheckAbsorb(MagicCaster* apCaster, MagicItem* apItem, ActiveEffect* apEffect, bool);
	virtual bool		CheckReflect(MagicCaster* apCaster, MagicItem* apItem, ActiveEffect* apEffect);


	UInt8					byt004;		// 004
	UInt8					byt005;		// 005
	UInt8					byt006[2];	// 006-7
	BSSimpleList<UInt32>	lst008;		// 008

	void RemoveEffect(EffectItem* effItem);

	void StopEffect(void* arg0, bool arg1)
	{
		ThisStdCall(0x8248E0, this, arg0, arg1);
	}
};

ASSERT_SIZE(MagicTarget, 0x010);