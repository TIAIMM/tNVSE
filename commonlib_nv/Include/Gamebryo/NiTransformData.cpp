#include "NiTransformData.hpp"

// GAME - 0xA4E350
void NiTransformData::ReplacePosData(NiPosKey* apKeys, UInt32 auiNumKeys, NiPosKey::KeyType aeType) {
	ThisStdCall(0xA4E350, this, apKeys, auiNumKeys, aeType);
}

// GAME - 0xA4E2E0
void NiTransformData::ReplaceRotData(NiRotKey* apKeys, UInt32 auiNumKeys, NiRotKey::KeyType aeType) {
	ThisStdCall(0xA4E2E0, this, apKeys, auiNumKeys, aeType);
}

// GAME - 0xA4E3B0
void NiTransformData::ReplaceScaleData(NiFloatKey* apKeys, UInt32 auiNumKeys, NiFloatKey::KeyType aeType) {
	ThisStdCall(0xA4E3B0, this, apKeys, auiNumKeys, aeType);
}

NiPosKey* NiTransformData::GetPosAnim(unsigned& iNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
{
	iNumKeys = m_usNumPosKeys;
	eType = m_ePosType;
	ucSize = m_ucPosSize;
	return m_pkPosKeys;
}

NiRotKey* NiTransformData::GetRotAnim(unsigned& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
{
	uiNumKeys = m_usNumRotKeys;
	eType = m_eRotType;
	ucSize = m_ucRotSize;
	return m_pkRotKeys;
}

NiFloatKey* NiTransformData::GetScaleAnim(unsigned& uiNumKeys, NiFloatKey::KeyType& eType, unsigned char& ucSize) const
{
	uiNumKeys = m_usNumScaleKeys;
	eType = m_eScaleType;
	ucSize = m_ucScaleSize;
	return m_pkScaleKeys;
}
