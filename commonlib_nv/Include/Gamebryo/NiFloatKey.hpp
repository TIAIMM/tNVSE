#pragma once

#include "NiAnimationKey.hpp"

class NiFloatKey : public NiAnimationKey {
public:
	float m_fValue;
	float ms_fDefault;

	static UInt8 GetKeySize(KeyType eType);

	NiFloatKey* GetKeyAt(UInt32 uiIndex, UInt8 ucKeySize) const;

	static NiFloatKey::InterpFunction GetInterpFunction(KeyType eType);

	static float GenInterp(float fTime, NiFloatKey* pkKeys, NiFloatKey::KeyType eType, UInt32 uiNumKeys, UInt32& uiLastIdx, UInt8 ucSize);
};

ASSERT_SIZE(NiFloatKey, 0xC);