#pragma once

#include "bhkBlendCollisionObject.hpp"

class bhkBlendCollisionObjectAddRotation : public bhkBlendCollisionObject {
public:
	NiMatrix3 kRotation;

	NIRTTI_ADDRESS(0x12681CC);
};

ASSERT_SIZE(bhkBlendCollisionObjectAddRotation, 0x50);