#pragma once

#include "TESBoundObject.hpp"
#include "TESModelTextureSwap.hpp"
#include "TESLeveledList.hpp"

class TESLevCharacter : public TESBoundObject, public TESLeveledList, public TESModelTextureSwap {
public:
	static inline auto bs_rtti = RTTI_TESLevCharacter;

	TESLevCharacter();
	~TESLevCharacter();

	UInt32 unk6C;
};

ASSERT_SIZE(TESLevCharacter, 0x70);