#pragma once

#include "NiPSysModifier.hpp"
#include "NiColor.hpp"

NiSmartPointer(BSPSysSimpleColorModifier);

class BSPSysSimpleColorModifier : public NiPSysModifier {
public:
	BSPSysSimpleColorModifier();
	~BSPSysSimpleColorModifier();

	NiColorA	kColor1;
	NiColorA	kColor2;
	NiColorA	kColor3;
	float		fFadeIn;
	float		fFadeOut;
	float		fColor1End;
	float		fColor2Start;
	float		fColor2End;
	float		fColor3Start;

	NIRTTI_ADDRESS(0x1204390);
};

ASSERT_SIZE(BSPSysSimpleColorModifier, 0x60)