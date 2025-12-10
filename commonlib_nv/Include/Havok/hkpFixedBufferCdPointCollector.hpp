#pragma once

#include "hkpCdPointCollector.hpp"

class hkpRootCdPoint;

class hkpFixedBufferCdPointCollector : public hkpCdPointCollector {
public:
	hkpFixedBufferCdPointCollector();

	hkpRootCdPoint* m_pointsArrayBase;
	hkpRootCdPoint* m_nextFreePoint;
	SInt32			m_capacity;
	SInt32			m_numPoints;
};

ASSERT_SIZE(hkpFixedBufferCdPointCollector, 0x18);