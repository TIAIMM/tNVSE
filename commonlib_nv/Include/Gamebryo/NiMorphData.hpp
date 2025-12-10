#pragma once

#include "NiObject.hpp"
#include "NiPoint3.hpp"
#include "NiFixedString.hpp"
#include "NiInterpolator.hpp"

NiSmartPointer(NiMorphData);

class NiMorphData : public NiObject {
public:
    class MorphTarget : public NiMemObject {
    public:
        NiPoint3*           m_aTargetVerts;
        NiFixedString       m_kName;
        float               m_fLegacyWeight;
        NiInterpolatorPtr   m_spLegacyInterpolator;
    };

    UInt32          m_uiNumTargets;
    UInt32          m_uiNumVertsPerTarget;
    MorphTarget*    m_aMorphTargets;
    bool            m_bRelativeTargets;

    CREATE_OBJECT(NiMorphData, 0xA415A0);
    NIRTTI_ADDRESS(0x11F3E74);
};

ASSERT_SIZE(NiMorphData, 0x18);