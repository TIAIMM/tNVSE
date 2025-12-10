#pragma once

#include "bhkWorld.hpp"

__declspec(align(16)) class bhkWorldM : public bhkWorld {
public:
	hkVector4 kWorldTotalSize;
	hkVector4 kBorderSize;
	hkVector4 kBroadPhaseSize;
};

ASSERT_SIZE(bhkWorldM, 0xD0);