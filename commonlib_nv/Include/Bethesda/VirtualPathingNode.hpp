#pragma once

#include "PathingLocation.hpp"

class VirtualPathingNode {
public:
	DWORD			dword0;
	PathingLocation kPathingLocation;
	TESForm*		pUnkForm;
};

ASSERT_SIZE(VirtualPathingNode, 0x30);