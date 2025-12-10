#pragma once

#include "bhkRigidBody.hpp"

class bhkRigidBodyT : public bhkRigidBody {
public:
	hkQuaternion	kLocalRot;
	hkVector4		kLocalPos;

	NIRTTI_ADDRESS(0x12683C8);

	static bhkRigidBodyT* Create(bhkRigidBody* apBody);
};

ASSERT_SIZE(bhkRigidBodyT, 0x40);