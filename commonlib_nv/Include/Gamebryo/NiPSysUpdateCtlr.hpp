#pragma once

#include "NiTimeController.hpp"

NiSmartPointer(NiPSysUpdateCtlr);

class NiPSysUpdateCtlr : public NiTimeController {
public:
	NiPSysUpdateCtlr();
	virtual ~NiPSysUpdateCtlr();

	CREATE_OBJECT(NiPSysUpdateCtlr, 0xC270F0);
	NIRTTI_ADDRESS(0x1202690);
};

ASSERT_SIZE(NiPSysUpdateCtlr, 0x34);
