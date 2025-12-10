#include "TESLeveledList.hpp"
#include "TESLevCreature.hpp"
#include "TESLevCharacter.hpp"
#include "TESLevItem.hpp"
#include "TESLevSpell.hpp"

// GAME - 0x487BE0
TESLeveledList* TESLeveledList::GetFormAsLeveledList(TESForm* apForm) {
    if (apForm) {
        switch (apForm->ucTypeID) {
	        case kFormType_TESLevCreature:
	            return reinterpret_cast<TESLevCreature*>(apForm);
	        case kFormType_TESLevCharacter:
	            return reinterpret_cast<TESLevCharacter*>(apForm);
	        case kFormType_TESLevItem:
	            return reinterpret_cast<TESLevItem*>(apForm);
	        case kFormType_TESLevSpell:
	            return reinterpret_cast<TESLevSpell*>(apForm);
        	default:
				return nullptr;
        }
    }

	return nullptr;
}

// GAME - 0x4877E0
bool TESLeveledList::GetCalcAllBelow() const {
    return ucFlags.GetBit(CALC_ALL_BELOW);
}

bool TESLeveledList::GetUseAll() const {
	return ucFlags.GetBit(USE_ALL);
}

// GAME - 0x487F70
void TESLeveledList::CalculateCurrentFormList(UInt16 ausLevel, UInt16 ausCount, TESContainer* apOut, UInt32 aeAllBelowForce) {
	ThisStdCall(0x487F70, this, ausLevel, ausCount, apOut, aeAllBelowForce);
}

// GAME - 0x487D00
void TESLeveledList::CalculateCurrentForm(UInt16 ausLevel, TESForm*& apOutForm, UInt16& asOutCount, ContainerItemExtra*& apOutExtra, bool abRecurse, UInt32 aeAllBelowForce) {
    ThisStdCall(0x487D00, this, ausLevel, apOutForm, asOutCount, apOutExtra, abRecurse, aeAllBelowForce);
}
