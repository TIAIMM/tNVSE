#pragma once

#include "hkContactPoint.hpp"
#include "NiPoint4.hpp"

class hkpCollidable;

class hkpRootCdPoint {
	hkContactPoint			m_contact;
	const hkpCollidable*	m_rootCollidableA;
	UInt32					m_shapeKeyA;
	NiPoint4				kUnkPointsA[2];
	const hkpCollidable*	m_rootCollidableB;
	UInt32					m_shapeKeyB;
	NiPoint4				kUnkPointsB[2];
};

ASSERT_SIZE(hkpRootCdPoint, 0x70);