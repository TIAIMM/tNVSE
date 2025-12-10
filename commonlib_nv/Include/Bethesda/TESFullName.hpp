#pragma once

#include "BaseFormComponent.hpp"
#include "BSString.hpp"

class TESFullName : public BaseFormComponent {
public:
	static inline auto bs_rtti = RTTI_TESFullName;

	TESFullName();
	~TESFullName();

	BSString	strFullName;

	const char* c_str() const {
		return strFullName.c_str();
	}
};

ASSERT_SIZE(TESFullName, 0xC);