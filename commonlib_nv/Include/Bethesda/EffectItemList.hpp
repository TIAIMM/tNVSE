#pragma once

#include "EffectItem.hpp"
#include "BSSimpleList.hpp"

class Actor;

class EffectItemList : public BSSimpleList<EffectItem*> {
public:
	EffectItemList();
	~EffectItemList();

	virtual bool	Unk_00();
	virtual bool	IsFood() const;
	virtual float	GetCost(Actor* apActor) const;
	virtual UInt32	GetMaxEffectCount() const;
	virtual UInt32	GetLevel() const;

	UInt32 uiHostileCount;

	const char* GetEffectName(UInt32 auiIndex) const;
};

ASSERT_SIZE(EffectItemList, 0x10);