#pragma once

#include "TESBoundObject.hpp"
#include "TESLeveledList.hpp"

class TESLevSpell : public TESBoundObject, public TESLeveledList {
public:
};

ASSERT_SIZE(TESLevSpell, 0x4C)