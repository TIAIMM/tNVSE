#pragma once

#include "bhkCollisionObject.hpp"
#include "hkpMotion.hpp"

class bhkWorld;

class bhkBlendCollisionObject : public bhkCollisionObject {
public:
	bool					bUpdateSceneObject;
	float					fHeirGain;
	float					fVelGain;
	hkpMotion::motionType	ePrevType;
	bhkWorld*				pStoredWorld;
	SInt32					iForceAddCount;

	NIRTTI_ADDRESS(0x1267E64);
};

ASSERT_SIZE(bhkBlendCollisionObject, 0x2C);