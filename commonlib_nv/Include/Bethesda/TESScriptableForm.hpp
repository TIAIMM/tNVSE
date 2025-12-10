#pragma once

#include "BaseFormComponent.hpp"
class TESScriptableForm : public BaseFormComponent {
public:
	TESScriptableForm();
	~TESScriptableForm();

	Script* pScript;
	bool	bResolved;
};

ASSERT_SIZE(TESScriptableForm, 0xC);