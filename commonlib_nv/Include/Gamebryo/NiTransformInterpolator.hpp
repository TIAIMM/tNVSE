#pragma once

#include "NiFloatKey.hpp"
#include "NiKeyBasedInterpolator.hpp"
#include "NiQuatTransform.hpp"
#include "NiTransformData.hpp"
#include "NiPoint3.hpp"

NiSmartPointer(NiTransformData);
NiSmartPointer(NiTransformInterpolator);

class NiTransformInterpolator : public NiKeyBasedInterpolator {
public:
	NiQuatTransform		m_kTransformValue;
	NiTransformDataPtr	m_spData;
	UInt16				m_uiLastTransIdx;
	UInt16				m_uiLastRotIdx;
	UInt16				m_uiLastScaleIdx;
	NiPoint3			m_kDefaultTranslate;
	bool				bPose;

	CREATE_OBJECT(NiTransformInterpolator, 0xA403F0);
	NIRTTI_ADDRESS(0x11F3E50);

	NiFloatKey* GetScaleData(unsigned int& uiNumKeys, NiFloatKey::KeyType& eType, unsigned char& ucSize) const;

	NiPosKey* GetPosData(
		unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const;

	NiRotKey* GetRotData(unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const;
};

ASSERT_SIZE(NiTransformInterpolator, 0x48);
