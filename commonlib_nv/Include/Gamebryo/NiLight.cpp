#include "NiLight.hpp"

// 0x4BC280
void NiLight::SetAmbientColor(const NiColor& arColor) {
	m_kAmb = arColor;
	IncRevisionID();
}

// 0x4BC2E0
void NiLight::SetDiffuseColor(const NiColor& arColor) {
	m_kDiff = arColor;
	IncRevisionID();
}

// 0x50DD50
void NiLight::SetSpecularColor(const NiColor& arColor) {
	m_kSpec = arColor;
	IncRevisionID();
}

void NiLight::SetRadius(float afRadius) {
	m_fRadius = afRadius;
	IncRevisionID();
}
