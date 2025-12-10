#pragma once

#include "hkVector4.hpp"

class hkpWorldRayCastInput {
public:
	hkVector4	m_from;
	hkVector4	m_to;
	bool		m_enableShapeCollectionFilter;
	UInt32		m_filterInfo;
};

ASSERT_SIZE(hkpWorldRayCastInput, 0x30);