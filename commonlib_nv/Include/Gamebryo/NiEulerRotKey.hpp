#pragma once

#include "NiRotKey.hpp"
#include "NiFloatKey.hpp"

class NiEulerRotKey : public NiRotKey {
public:
    UInt32              m_uiNumKeys[3];
    NiFloatKey::KeyType m_eType[3];
    UInt8               m_ucSizes[3];
    NiFloatKey*         m_apkKeys[3];
    UInt32              m_uiLastIdx[3];

    inline UInt8 GetKeySize(UInt8 aucIndex) const {
        return m_ucSizes[aucIndex];
    }
    inline UInt32 GetNumKeys(UInt8 aucIndex) const {
        return m_uiNumKeys[aucIndex];
    }
};