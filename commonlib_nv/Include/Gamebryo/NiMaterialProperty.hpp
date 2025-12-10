#pragma once

#include "NiProperty.hpp"
#include "NiColor.hpp"

NiSmartPointer(NiMaterialProperty);

class NiMaterialProperty : public NiProperty {
public:
	NiMaterialProperty();
	virtual ~NiMaterialProperty();

	SInt32		m_iIndex;
	NiColor		m_spec;
	NiColor		m_emit;
	NiColor*	m_pExternalEmittance;
	float		m_fShine;
	float		m_fAlpha;
	float		m_fEmitMult;
	UInt32		m_uiRevID;
	void*		m_pvRendererData;

	NiColor* GetEmittance();

	static NiMaterialProperty* CreateObject() {
		return CdeclCall<NiMaterialProperty*>(0xA756D0);
	}
};

ASSERT_SIZE(NiMaterialProperty, 0x4C)