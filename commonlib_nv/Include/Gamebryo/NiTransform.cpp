#include "NiTransform.hpp"

// 0x476A80
NiTransform::NiTransform() {
	m_Rotate = NiMatrix3();
	m_Translate = NiPoint3();
	m_fScale = 0.0f;
}

// 0xA86BF0
void NiTransform::MakeIdentity() {
	m_Rotate = NiMatrix3::IDENTITY;
	m_Translate = NiPoint3::ZERO;
	m_fScale = 1.0f;
}

// 0x4B4880
void NiTransform::Invert(NiTransform& kDest) {
	kDest.m_Rotate = m_Rotate.Transpose();
	kDest.m_fScale = 1.0f / m_fScale;
	kDest.m_Translate = kDest.m_fScale * (kDest.m_Rotate * -m_Translate);
}