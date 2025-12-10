#pragma once

#include "hkVector4.hpp"
#include "hkpWorldRayCastInput.hpp"
#include "hkpWorldRayCastOutput.hpp"

class hkpRayHitCollector;
class SpecificItemCollector;
class NiAVObject;
class hkpRigidBody;
class hkpPhantom;
class CFilter;

class bhkPickData : public hkpWorldRayCastInput, public hkpWorldRayCastOutput {
public:
	bhkPickData();
	~bhkPickData();

	hkVector4				hkvector4f90;
	char*					pCache;
	SpecificItemCollector*	pSpecificItemCollector;
	hkpRayHitCollector*		pRayHitCollector;
	bool					bFailed;

	void NiSetFrom(const NiPoint3& arPoint);
	void NiSetTo(const NiPoint3& arPoint);

	void SetHitCollector(hkpRayHitCollector* apRayHitCollector);
	void SetSpecificItemCollector(SpecificItemCollector* apSpecificItemCollector);

	void SetFilterInfo(CFilter& arFilterInfo);

	NiAVObject*		GetNiAVObject() const;
	hkpRigidBody*	GethkpRigidBody() const;
	hkpPhantom*		GethkpPhantom() const;

	void NiGetHitPosition(NiPoint3& arPoint) const;
};

ASSERT_SIZE(bhkPickData, 0xB0);