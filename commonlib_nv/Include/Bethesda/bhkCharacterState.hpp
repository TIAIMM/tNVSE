#pragma once

#include "hkpCharacterState.hpp"

class bhkCharacterController;

class bhkCharacterState : public hkpCharacterState {
public:
	virtual void Update(bhkCharacterController* apController);
};

ASSERT_SIZE(bhkCharacterState, 0x8);