#pragma once

#include "hkpShapePhantom.hpp"
#include "hkpCollisionAgent.hpp"

class hkpCachingShapePhantom : public hkpShapePhantom {
public:
	struct hkpCollisionDetail {
		hkpCollisionAgent*	m_agent;
		hkpCollidable*		m_collidable;
	};

	hkArray<hkpCollisionDetail> m_collisionDetails;
	bool						m_orderDirty;
};

ASSERT_SIZE(hkpCachingShapePhantom, 0x170);