#include "NiTransformInterpolator.hpp"
#include "NiTransformData.hpp"

NiFloatKey* NiTransformInterpolator::GetScaleData(unsigned& uiNumKeys, NiFloatKey::KeyType& eType,
    unsigned char& ucSize) const
{
    if (m_spData)
    {
        return m_spData->GetScaleAnim(uiNumKeys, eType, ucSize);
    }

    uiNumKeys = 0;
    eType = NiFloatKey::NOINTERP;
    ucSize = 0;
    return nullptr;
}

NiPosKey* NiTransformInterpolator::GetPosData(unsigned& uiNumKeys, NiAnimationKey::KeyType& eType,
    unsigned char& ucSize) const
{
    if (m_spData)
    {
        return m_spData->GetPosAnim(uiNumKeys, eType, ucSize);
    }

    uiNumKeys = 0;
    eType = NiAnimationKey::NOINTERP;
    ucSize = 0;
    return nullptr;
}

NiRotKey* NiTransformInterpolator::GetRotData(unsigned& uiNumKeys, NiAnimationKey::KeyType& eType,
    unsigned char& ucSize) const
{
    if (m_spData)
    {
        return m_spData->GetRotAnim(uiNumKeys, eType, ucSize);
    }

    uiNumKeys = 0;
    eType = NiAnimationKey::NOINTERP;
    ucSize = 0;
    return nullptr;
}
