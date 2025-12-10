#pragma once

#include "BaseFormComponent.hpp"

class BGSTouchSpellForm : public BaseFormComponent {
public:
	BGSTouchSpellForm();
	~BGSTouchSpellForm();

	TESForm*	pUnarmedEffect;
	UInt16		usUnarmedAnim;
};

ASSERT_SIZE(BGSTouchSpellForm, 0xC);