#pragma once

#include "BaseFormComponent.hpp"
#include "BSSimpleList.hpp"
#include "ContainerObject.hpp"

class TESForm;
class TESObjectREFR;

class TESContainer {
public:
	static inline auto bs_rtti = RTTI_TESContainer;

	TESContainer() {
		ThisStdCall<TESContainer*>(0x481610, this);
	}

	~TESContainer() {
		ThisStdCall<TESContainer*>(0x481680, this);
	}

	static void Init(const TESContainer* container) {
		ThisStdCall<TESContainer*>(0x481610, container);
	}

	static void Destroy(const TESContainer* container) {
		ThisStdCall<TESContainer*>(0x481680, container);
	}

	uint32_t pad;

	BSSimpleList<ContainerObject*> kObjects;
	SInt32 GetCountForForm(TESForm* form);
	static bool CanHoldType(UInt32 auiFormType);
	void MoveAllItems(TESObjectREFR* apRef, bool abShowMessage);
	ContainerObject* AddItem(TESForm* item, int count, int extraData);
	void* UpdateHealthPercent(float a2);
};

ASSERT_SIZE(TESContainer, 0xC);