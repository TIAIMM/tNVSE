#pragma once

#include "NiTimeController.hpp"
#include "NiTLargeArray.hpp"

struct BLENDKEY {
	float unk0;
	float fHeirGain;
	float fVelGain;

	bool operator==(const BLENDKEY& other) const {
		return unk0 == other.unk0 && fHeirGain == other.fHeirGain && fVelGain == other.fVelGain;
	}

	bool operator!=(const BLENDKEY& other) const {
		return !(*this == other);
	}
};

class bhkBlendController : public NiTimeController {
public:
	UInt32								uiSomeIndex34;
	NiTLargePrimitiveArray<BLENDKEY>	kBlendKeys;
	float								fHeirGain;
	float								fVelGain;
	DWORD								dword58;

	CREATE_OBJECT(bhkBlendController, 0xC9B020);
	NIRTTI_ADDRESS(0x12681F0);
};

ASSERT_SIZE(bhkBlendController, 0x5C);