#pragma once

#include "hkReferencedObject.hpp"

class hkpCharacterStateManager;

class hkpCharacterContext : public hkReferencedObject {
public:
	enum CharacterState : UInt32 {
		ON_GROUND	= 0,
		JUMPING		= 1,
		IN_AIR		= 2,
		CLIMBING	= 3,
		FLYING		= 4,
		SWIMMING	= 5,
		PROJECTILE	= 6,
		COUNT		= 11,
	};

	UInt32						m_characterType;
	hkpCharacterStateManager*	m_stateManager;
	CharacterState				m_currentState;
	CharacterState				m_previousState;
	bool						m_filterEnable;
	float						m_maxLinearAcceleration;
	float						m_maxLinearVelocity;
	float						m_gain;

	CharacterState getState() const;
};

ASSERT_SIZE(hkpCharacterContext, 0x28);