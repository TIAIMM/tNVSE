#pragma once

#include "NiParticles.hpp"
#include "NiTPointerList.hpp"
#include "NiPSysModifier.hpp"

NiSmartPointer(NiParticleSystem);

class NiParticleSystem : public NiParticles {
public:
	NiParticleSystem();
	virtual ~NiParticleSystem();

	bool								m_bWorldSpace;
	NiTPointerList<NiPSysModifierPtr>	m_kModifierList;
	float								m_fLastTime;
	bool								m_bResetSystem;
	bool								m_bDynamicBounds;
	NiTransform							m_kUnmodifiedWorld;
	
	NIRTTI_ADDRESS(0x12024E0)
};

ASSERT_SIZE(NiParticleSystem, 0x110);