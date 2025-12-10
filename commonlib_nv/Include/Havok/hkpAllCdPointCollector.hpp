#pragma once

#include "hkpCdPointCollector.hpp"
#include "hkInplaceArray.hpp"
#include "hkpRootCdPoint.hpp"

class hkCdBody;

__declspec(align(16)) class hkpAllCdPointCollector : public hkpCdPointCollector {
public:
	virtual void addCdPointAlt(const hkpCdPoint& event);

	hkInplaceArray<hkpRootCdPoint, 8> m_hits;
};

ASSERT_SIZE(hkpAllCdPointCollector, 0x3A0);