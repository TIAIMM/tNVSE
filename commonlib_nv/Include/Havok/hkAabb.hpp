#pragma once

#include "hkVector4.hpp"

__declspec(align(16)) class hkAabb {
public:
	hkVector4 m_min;
	hkVector4 m_max;
};

ASSERT_SIZE(hkAabb, 0x20)