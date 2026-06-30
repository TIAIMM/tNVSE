#include "BGSNumericIDIndex.hpp"
#include "BGSSaveLoadGame.hpp"

// GAME - 0x853570
void BGSNumericIDIndex::SetNumericID(UInt32 auiID) {
#if ESL_SUPPORT
	UInt8 ucIndex = auiID >> 24;
	UInt32 uiFormID = 0;
#if 0
	if (BGSSaveLoadGame::GetSingleton()->ucCurrentMinorVersion <= 27) {
		uiFormID = BGSSaveLoadGame::GetSingleton()->pFormIDMap->AddFormID(auiID);
		if (ucIndex == 255)
			uiFormID = (uiFormID | 0x800000);

		ucData1 = uiFormID >> 16;
		ucData2 = uiFormID >> 8;
		ucData3 = uiFormID;
		return;
	}
#endif
	if (auiID && (!ucIndex || ucIndex == 0xFF)) {
		if ((auiID & 0xC00000) != 0)
			_MESSAGE("[ BGSNumericIDIndex::SetNumericID ] Flag already set on FormID %08X", auiID);

		UInt32 uiMask = 0x800000;
		if (!ucIndex)
			uiMask = 0x400000;

		uiFormID = auiID | uiMask;
	}
	else {
		uiFormID = BGSSaveLoadGame::GetSingleton()->pFormIDMap->AddFormID(auiID);
	}
	ucData1 = uiFormID >> 16;
	ucData2 = uiFormID >> 8;
	ucData3 = uiFormID;
#else
	UInt32 uiFormID = BGSSaveLoadGame::GetSingleton()->pFormIDMap->AddFormID(auiID);
	if ((uiFormID >> 24) == 0xFF)
		uiFormID = (uiFormID | 0x800000);
	ucData1 = uiFormID >> 16;
	ucData2 = uiFormID >> 8;
	ucData3 = uiFormID;
#endif
}

// GAME - 0x853500
UInt32 BGSNumericIDIndex::GetNumericID() const {
#if ESL_SUPPORT
	const UInt32 uiID = ucData3 + (ucData2 << 8) + (ucData1 << 16);
#if 0
	if (BGSSaveLoadGame::GetSingleton()->ucCurrentMinorVersion <= 27) {
		UInt32 uiFormID = 0;
		if ((uiID & 0x800000) != 0)
			uiFormID = uiID & 0x7FFFFF | 0xFF000000;
		else
			uiFormID = BGSSaveLoadGame::GetSingleton()->pFormIDMap->GetFormID(uiID);

		return uiFormID;
	}
#endif
	if ((uiID & 0xC00000) == 0)
		return BGSSaveLoadGame::GetSingleton()->pFormIDMap->GetFormID(uiID);

	UInt32 uiMask = 0xFF000000;
	if ((uiID & 0x400000) != 0)
		uiMask = 0;

	return uiID & 0xFF3FFFFF | uiMask;
#else
	const UInt32 uiID = ucData3 + (ucData2 << 8) + (ucData1 << 16);
	if ((uiID & 0x800000u) != 0)
		return uiID & 0x7FFFFFu | 0xFF000000u;
	
	return BGSSaveLoadGame::GetSingleton()->pFormIDMap->GetFormID(uiID);
#endif
}
