#pragma once

#include "NiAnimationKey.hpp"
#include "NiQuaternion.hpp"

class NiStream;

class NiRotKey : public NiAnimationKey {
public:
	NiQuaternion m_quat;

	static UInt8 GetKeySize(KeyType eType);
	static void SaveBinary(NiStream& arStream, NiAnimationKey* apKey);

	NiRotKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize) const
	{
		return static_cast<NiRotKey*>(NiAnimationKey::GetKeyAt(uiIndex, ucKeySize));
	}
};

ASSERT_SIZE(NiRotKey, 0x14);