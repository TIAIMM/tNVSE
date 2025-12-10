#pragma once

#include "hkVector4.hpp"
#include "hkReferencedObject.hpp"
#include "hkpEntityListener.hpp"
#include "hkpPhantomListener.hpp"

class hkpRigidBody;
class hkpShapePhantom;
class hkpSurfaceConstraintInfo;
class hkpCharacterProxyListener;

class hkpCharacterProxy : public hkReferencedObject, public hkpEntityListener, public hkpPhantomListener {
public:
	virtual void updateManifold(const hkpAllCdPointCollector& startPointCollector, const hkpAllCdPointCollector& castCollector, bool isMultithreaded = false);
	virtual void extractSurfaceConstraintInfo(const hkpRootCdPoint& hit, hkpSurfaceConstraintInfo& surfaceOut, float timeTravelled) const;

	hkVector4							m_velocity;
	hkVector4							m_oldDisplacement;
	hkpShapePhantom*					m_shapePhantom;
	float								m_dynamicFriction;
	float								m_staticFriction;
	hkVector4							m_up;
	float								m_extraUpStaticFriction;
	float								m_extraDownStaticFriction;
	float								m_keepDistance;
	float								m_keepContactTolerance;
	float								m_contactAngleSensitivity;
	SInt32								m_userPlanes;
	float								m_maxCharacterSpeedForSolver;
	float								m_characterStrength;
	float								m_characterMass;
	hkArray<hkpRootCdPoint>				m_manifold;
	hkArray<hkpCharacterProxyListener*> m_listeners;
	hkArray<hkpRigidBody*>				m_bodies;
	hkArray<hkpPhantom*>				m_phantoms;
	float								m_maxSlopeCosine;
	float								m_penetrationRecoverySpeed;
	SInt32								m_maxCastIterations;
	bool								m_refreshManifoldInCheckSupport;
};

ASSERT_SIZE(hkpCharacterProxy, 0xC0);