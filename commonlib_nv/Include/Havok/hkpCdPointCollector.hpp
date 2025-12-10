#pragma once

class hkpCdPoint;

class hkpCdPointCollector {
public:
	hkpCdPointCollector();
	virtual ~hkpCdPointCollector();
	virtual void addCdPoint(const hkpCdPoint& point);
	virtual void reset();

	float m_earlyOutDistance;
};

ASSERT_SIZE(hkpCdPointCollector, 0x8);