#pragma once

#include "NiTimeController.hpp"
#include "hkVector4.hpp"

class bhkForceController : public NiTimeController {
public:
	hkVector4	kVector;
	float		fDelaTime;
	float		fAlignmentPadding[7];

	NIRTTI_ADDRESS(0x12683F4);
};

ASSERT_SIZE(bhkForceController, 0x70);