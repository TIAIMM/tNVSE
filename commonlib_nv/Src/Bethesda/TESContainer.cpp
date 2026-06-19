#include "TESContainer.hpp"

SInt32 TESContainer::GetCountForForm(TESForm* form) {
	SInt32 result = 0;
	for (const auto* iter : kObjects) {
		if (iter->pForm == form) {
			result += iter->iCount;
		}
	}
	return result;
}

// GAME - 0x481F30
bool TESContainer::CanHoldType(const UInt32 auiFormType) {
	return CdeclCall<bool>(0x481F30, auiFormType);
}

// GAME - 0x4821A0
void TESContainer::MoveAllItems(TESObjectREFR* apRef, bool abShowMessage) {
	ThisStdCall(0x4821A0, this, apRef, abShowMessage);
}

ContainerObject* TESContainer::AddItem(TESForm* item, int count, int extraData) {
	return ThisStdCall<ContainerObject*>(0x4818E0, this, item, count, extraData);
}

void* TESContainer::UpdateHealthPercent(float a2) {
	return ThisStdCall<void*>(0x482090, this, a2);
}