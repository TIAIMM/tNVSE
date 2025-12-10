#pragma once

#include "BaseFormComponent.hpp"

class BGSAmmoForm : public BaseFormComponent {
public:
	BGSAmmoForm();
	~BGSAmmoForm();

	TESForm* pAmmo; // 04	either TESAmmo or BGSListForm
};