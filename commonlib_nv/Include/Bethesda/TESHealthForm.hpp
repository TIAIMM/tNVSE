#pragma once

#include "BaseFormComponent.hpp"

class TESHealthForm : public BaseFormComponent {
public:
	static inline auto bs_rtti = RTTI_TESHealthForm;

	TESHealthForm();
	~TESHealthForm();

	virtual UInt32	GetHealth(void);	// 0004

	UInt32	uiHealth;		// 004
};

ASSERT_SIZE(TESHealthForm, 0x8);