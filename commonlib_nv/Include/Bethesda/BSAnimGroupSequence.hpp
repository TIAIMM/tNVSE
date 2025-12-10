#pragma once

#include "NiControllerSequence.hpp"

class TESAnimGroup;

class BSAnimGroupSequence : public NiControllerSequence {
public:
	BSAnimGroupSequence();
	virtual ~BSAnimGroupSequence();

	enum Sequence
	{
		IDLE		= 0,
		MOVEMENT	= 1,
		LEFTARM		= 2,
		LEFTHAND	= 3,
		WEAPON		= 4,
		WEAPONUP	= 5,
		WEAPONDOWN	= 6,
		SPECIALIDLE = 7,
		DEATH		= 20,
	};

	NiPointer<TESAnimGroup> spAnimGroup;

	TESAnimGroup* GetAnimGroup() const;

	BSAnimGroupSequence *Reset() {
		return ThisStdCall<BSAnimGroupSequence*>(0x4EEE00, this);
	}
};