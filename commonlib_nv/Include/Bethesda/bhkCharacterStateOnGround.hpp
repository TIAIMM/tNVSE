#pragma once

#include "bhkCharacterState.hpp"

class bhkCharacterStateOnGround : public bhkCharacterState {
public:
	bool bKillVelocityOnLaunch;
};

ASSERT_SIZE(bhkCharacterStateOnGround, 0xC);