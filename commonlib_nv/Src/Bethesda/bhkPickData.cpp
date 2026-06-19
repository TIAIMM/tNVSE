#include "bhkPickData.hpp"
#include "bhkUtilFunctions.hpp"
#include "hkpCollidable.hpp"
#include "hkpRigidBody.hpp"
#include "CFilter.hpp"
#include <hkpPhantom.hpp>

// GAME - 0x4A3C20
bhkPickData::bhkPickData() {
	hkvector4f90			= hkVector4::zero;
	pCache					= nullptr;
	pSpecificItemCollector	= nullptr;
	pRayHitCollector		= nullptr;
	bFailed					= false;
}

bhkPickData::~bhkPickData() {
}

// GAME - 0x4A3DA0
void bhkPickData::NiSetFrom(const NiPoint3& arPoint) {
	ThisStdCall(0x4A3DA0, this, &arPoint);
}

// GAME - 0x4A3EB0
void bhkPickData::NiSetTo(const NiPoint3& arPoint) {
	ThisStdCall(0x4A3EB0, this, &arPoint);
}

// GAME - 0x4A3FB0
void bhkPickData::SetHitCollector(hkpRayHitCollector* apRayHitCollector) {
    pRayHitCollector = apRayHitCollector;
    pSpecificItemCollector = nullptr;
}

// GAME - 0x59CEB0
void bhkPickData::SetSpecificItemCollector(SpecificItemCollector* apSpecificItemCollector) {
    pSpecificItemCollector = apSpecificItemCollector;
	pRayHitCollector = nullptr;
}

// GAME - 0x4A3F70
void bhkPickData::SetFilterInfo(CFilter& arFilterInfo) {
	m_filterInfo = arFilterInfo.uiFilter;
}

// GAME - 0xC66FB0
NiAVObject* bhkPickData::GetNiAVObject() const {
    if (m_rootCollidable)
	    return bhkUtilFunctions::GetNiAVObject(m_rootCollidable);

    return nullptr;
}

// GAME - 0xC66FD0
hkpRigidBody* bhkPickData::GethkpRigidBody() const {
	return hkpGetRigidBody(m_rootCollidable);
}

hkpPhantom* bhkPickData::GethkpPhantom() const {
	return hkpGetPhantom(m_rootCollidable);
}

// GAME - 0x5DBE60
void bhkPickData::NiGetHitPosition(NiPoint3& arPoint) const {
	ThisStdCall(0x5DBE60, this, &arPoint);
}
