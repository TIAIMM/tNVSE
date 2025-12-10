#pragma once

#include "BSMemObject.hpp"

class TESSound;

class TESRegionSound : public BSMemObject {
public:
	TESSound*	pSound;
	UInt32		uiFlags;
	UInt32		uiChance;
};

ASSERT_SIZE(TESRegionSound, 0xC)