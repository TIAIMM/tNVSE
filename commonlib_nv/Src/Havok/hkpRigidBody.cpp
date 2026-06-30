#include "hkpRigidBody.hpp"

// GAME - 0x560DC0
hkVector4& hkpRigidBody::getLinearVelocity() {
	return m_motion.m_linearVelocity;
}

// GAME - 0x560E50
hkVector4& hkpRigidBody::getAngularVelocity() {
	return m_motion.m_angularVelocity;
}

// GAME - 0x5616D0
// GECK - 0x679BA0
void hkpRigidBody::setLinearVelocity(const hkVector4& vector) {
	ThisStdCall(0x5616D0, this, &vector);
}

// GAME - 0x561800
// GECK - 0x679C00
void hkpRigidBody::setAngularVelocity(const hkVector4& vector) {
	ThisStdCall(0x561800, this, &vector);
}

// GAME - 0x4B59F0
hkpRigidBody* hkpGetRigidBody(const hkpCollidable* apCollidable) {
	return CdeclCall<hkpRigidBody*>(0x4B59F0, apCollidable);
}
