#pragma once

#include "NiAnimationKey.hpp"
#include "NiPoint3.hpp"

class NiPosKey : public NiAnimationKey {
public:
    NiPosKey() {};
    ~NiPosKey() {};

    NiPoint3 m_Pos;

    static UInt8 GetKeySize(KeyType eType);

    NiPosKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize) const
    {
        return static_cast<NiPosKey*>(NiAnimationKey::GetKeyAt(uiIndex, ucKeySize));
    }
};

ASSERT_SIZE(NiPosKey, 0x10);