#pragma once

#include "NiObject.hpp"
#include "NiFixedString.hpp"
#include "NiParticles.hpp"

class NiParticleInfo;
class NiParticleSystem;

NiSmartPointer(NiPSysModifier);

struct NiPSysData : NiParticlesData
{
	NiParticleInfo *m_pkParticleInfo;
	float *m_pfRotationSpeeds;
	unsigned __int16 m_usNumAddedParticles;
	unsigned __int16 m_usAddedParticlesBase;
};

class NiPSysModifier : public NiObject {
public:
	NiPSysModifier();
	virtual ~NiPSysModifier();

	virtual void Update(float afTime, NiPSysData *apData);
	virtual void Initialize(NiPSysData *apData, UInt16 ausNewParticle);
	virtual void HandleReset();
	virtual void SetActive(bool bActive);
	virtual void SetSystemPointer(NiParticleSystem* pkTarget);
	virtual void Unk28();

	NiFixedString		m_kName;
	UInt32				m_uiOrder;
	NiParticleSystem*	m_pkTarget;
	bool				m_bActive;
};

ASSERT_SIZE(NiPSysModifier, 0x18)