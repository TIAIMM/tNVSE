#pragma once

#include "NiObject.hpp"
#include "NiRotKey.hpp"
#include "NiPosKey.hpp"
#include "NiFloatKey.hpp"

class NiRotKey;
class NiPosKey;
class NiFloatKey;

NiSmartPointer(NiTransformData);

class NiTransformData : public NiObject {
public:
	UInt16				m_usNumRotKeys;
	UInt16				m_usNumPosKeys;
	UInt16				m_usNumScaleKeys;
	NiRotKey::KeyType	m_eRotType;
	NiPosKey::KeyType	m_ePosType;
	NiFloatKey::KeyType	m_eScaleType;
	UInt8				m_ucRotSize;
	UInt8				m_ucPosSize;
	UInt8				m_ucScaleSize;
	NiRotKey*			m_pkRotKeys;
	NiPosKey*			m_pkPosKeys;
	NiFloatKey*			m_pkScaleKeys;

	CREATE_OBJECT(NiTransformData, 0xA4E040);
	NIRTTI_ADDRESS(0x11F3FBC);

	void ReplacePosData(NiPosKey* apKeys, UInt32 auiNumKeys, NiPosKey::KeyType aeType);
	void ReplaceRotData(NiRotKey* apKeys, UInt32 auiNumKeys, NiRotKey::KeyType aeType);
	void ReplaceScaleData(NiFloatKey* apKeys, UInt32 auiNumKeys, NiFloatKey::KeyType aeType);

	NiPosKey* GetPosAnim(unsigned int& iNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const;
	NiRotKey* GetRotAnim(unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const;
	NiFloatKey* GetScaleAnim(unsigned int& uiNumKeys, NiFloatKey::KeyType& eType, unsigned char& ucSize) const;
};

ASSERT_SIZE(NiTransformData, 0x2C);