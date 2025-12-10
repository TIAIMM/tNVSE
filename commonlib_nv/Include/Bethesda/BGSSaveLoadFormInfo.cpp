#include "BGSSaveLoadFormInfo.hpp"

// GAME - 0x853670
void BGSSaveLoadFormInfo::SetFormType(UInt32 aeFormType) {
	ThisStdCall(0x853670, this, aeFormType);
}

// GAME - 0x853640
UInt32 BGSSaveLoadFormInfo::GetFormType() {
	return ThisStdCall<UInt32>(0x853640, this);
}
