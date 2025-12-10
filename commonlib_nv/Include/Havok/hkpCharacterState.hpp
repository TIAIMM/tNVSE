#pragma once

#include "hkReferencedObject.hpp"

class hkpCharacterContext;
struct hkpCharacterInput;
struct hkpCharacterOutput;

enum hkpCharacterStateType {
	HK_CHARACTER_ON_GROUND		= 0,
	HK_CHARACTER_JUMPING		= 1,
	HK_CHARACTER_IN_AIR			= 2,
	HK_CHARACTER_CLIMBING		= 3,
	HK_CHARACTER_FLYING			= 4,
	HK_CHARACTER_SWIMMING		= 5,
	HK_CHARACTER_PROJECTILE		= 6,
	HK_CHARACTER_USER_STATE_2	= 7,
	HK_CHARACTER_USER_STATE_3	= 8,
	HK_CHARACTER_USER_STATE_4	= 9,
	HK_CHARACTER_USER_STATE_5	= 10,
	HK_CHARACTER_MAX_STATE_ID	= 11,
};


class hkpCharacterState : public hkReferencedObject {
public:
	virtual hkpCharacterStateType getType() const;
	virtual void enterState(hkpCharacterContext& context, hkpCharacterStateType prevState, const hkpCharacterInput& input, hkpCharacterOutput& output);
	virtual void leaveState(hkpCharacterContext& context, hkpCharacterStateType nextState, const hkpCharacterInput& input, hkpCharacterOutput& output);
	virtual void update(hkpCharacterContext& context, const hkpCharacterInput& input, hkpCharacterOutput& output);
	virtual void change(hkpCharacterContext& context, const hkpCharacterInput& input, hkpCharacterOutput& output);
};