#pragma once

#include "NiPSysEmitterCtlr.hpp"

NiSmartPointer(BSPSysMultiTargetEmitterCtlr);

class BSPSysMultiTargetEmitterCtlr : public NiPSysEmitterCtlr {
public:
	BSPSysMultiTargetEmitterCtlr();
	~BSPSysMultiTargetEmitterCtlr();

	BSMasterParticleSystem *pMasterParticleSystem;
	UInt16 usMaxFullEmitters;

	NIRTTI_ADDRESS(0x1203340)
	CREATE_OBJECT(BSPSysMultiTargetEmitterCtlr, 0xC5CBB0)
};

ASSERT_SIZE(BSPSysMultiTargetEmitterCtlr, 0x64)