#include "BGSSaveLoadFormIDMap.hpp"
#include "BGSSaveLoadGame.hpp"

// GAME - 0x846D80
UInt32 BGSSaveLoadFormIDMap::ConvertFormID(UInt32 auiFormID) const {
    if ((auiFormID >> 24) == 0xFF)
        return auiFormID;
    else
        return BGSSaveLoadGame::GetSingleton()->GetConvertedFormID(auiFormID);
}

// GAME - 0x846C90
UInt32 BGSSaveLoadFormIDMap::AddFormID(UInt32 auiFormID) {
	return ThisStdCall(0x846C90, this, auiFormID);
}

// GAME - 0x846D20
UInt32 BGSSaveLoadFormIDMap::GetFormID(UInt32 auiIndex) {
    if (auiIndex >= 0xFF000000)
        return auiIndex;

    UInt32 uiFormID = 0;
    if (kIndexToFormIDMap.GetAt(auiIndex, uiFormID))
        return uiFormID;
    else
        return 0;
}
