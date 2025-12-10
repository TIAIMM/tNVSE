#pragma once

#include "hkpShapeRayCastOutput.hpp"

class hkpCollidable;

__declspec(align(16)) class hkpWorldRayCastOutput : public hkpShapeRayCastOutput {
public:
	hkpCollidable* m_rootCollidable;
};

ASSERT_SIZE(hkpWorldRayCastOutput, 0x60);