#pragma once

#include "NiPoint3.hpp"
#include "NIQuaternion.hpp"

class NiQuatTransform {
public:
	NiQuatTransform() : m_kTranslate(-FLT_MAX), m_kRotate(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX), m_fScale(-FLT_MAX) { }
    ~NiQuatTransform() {};

    NiPoint3        m_kTranslate;
    NiQuaternion    m_kRotate;
    float           m_fScale;

    inline bool IsTranslateValid() const {
        return m_kTranslate.x != -FLT_MAX;
    }

    inline bool IsRotateValid() const {
        return m_kRotate.m_fX != -FLT_MAX;
    }

    inline bool IsScaleValid() const {
        return m_fScale != -FLT_MAX;
    }

    inline bool IsTransformInvalid() const {
        return !(IsScaleValid() || IsRotateValid() || IsTranslateValid());
    }
};

ASSERT_SIZE(NiQuatTransform, 0x20)