#pragma once

#include "hkpEntity.hpp"

class hkpCollidable;

class hkpRigidBody : public hkpEntity {
public:
	hkVector4& getLinearVelocity();
	hkVector4& getAngularVelocity();

	void setLinearVelocity(const hkVector4& vector);
	void setAngularVelocity(const hkVector4& vector);
};

hkpRigidBody* hkpGetRigidBody(const hkpCollidable* apCollidable);

ASSERT_SIZE(hkpRigidBody, 0x220);