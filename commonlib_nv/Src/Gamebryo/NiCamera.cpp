#include "NiCamera.hpp"
#include "NiBound.hpp"
#include "NiDX9Renderer.hpp"

void NiCamera::SetViewFrustum(const NiFrustum& kFrustum) {
    m_kViewFrustum.m_fNear = kFrustum.m_fNear;

    float fMinNear = kFrustum.m_fFar / m_fMaxFarNearRatio;
    if (m_kViewFrustum.m_fNear < fMinNear)
        m_kViewFrustum.m_fNear = fMinNear;

    if (m_kViewFrustum.m_fNear < m_fMinNearPlaneDist)
        m_kViewFrustum.m_fNear = m_fMinNearPlaneDist;

    m_kViewFrustum.m_fLeft = kFrustum.m_fLeft;
    m_kViewFrustum.m_fRight = kFrustum.m_fRight;
    m_kViewFrustum.m_fTop = kFrustum.m_fTop;
    m_kViewFrustum.m_fBottom = kFrustum.m_fBottom;
    m_kViewFrustum.m_fFar = kFrustum.m_fFar;
    m_kViewFrustum.m_bOrtho = kFrustum.m_bOrtho;
}

// 0xA701B0
bool NiCamera::LookAtWorldPoint(const NiPoint3& kWorldPt, const NiPoint3& kWorldUp) {
	return ThisStdCall<bool>(0xA701B0, this, &kWorldPt, &kWorldUp);
}
