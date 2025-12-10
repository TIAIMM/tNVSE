#pragma once

#include "PathingLocation.hpp"

class TESObjectREFR;

class PathingNode {
public:
	UInt32			uiFlags;
	PathingLocation kPathingLoc;
	NiPoint3		kPoint2C;
	TESObjectREFR*	pRef38;
};

ASSERT_SIZE(PathingNode, 0x3C);