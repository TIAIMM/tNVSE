#pragma once

#include "TESBoundObject.hpp"
#include "TESModelTextureSwap.hpp"
#include "TESLeveledList.hpp"

class TESLevCreature : public TESBoundObject, public TESLeveledList, public TESModelTextureSwap {
public:
	TESLevCreature();
	~TESLevCreature();

	UInt32 unk6C;
};

ASSERT_SIZE(TESLevCreature, 0x70);