#pragma once

#include "hkReferencedObject.hpp"

class hkpWorld;
class hkpEntity;
class hkpAgentNnEntry;
class hkStepInfo;

class hkpSimulation : public hkReferencedObject {
public:
	virtual UInt32	stepDeltaTime(float physicsDeltaTime);
	virtual UInt32	integrate(float physicsDeltaTime);
	virtual UInt32	collide();
	virtual UInt32	advanceTime();
	virtual void	collideEntitiesDiscrete(hkpEntity** entities, int numEntities, hkpWorld* world, int* stepInfo, int findExtraContacts);
	virtual void	resetCollisionInformationForEntities(hkpEntity** entities, int numEntities, hkpWorld* world, int resetInfo);
	virtual void	assertThereIsNoCollisionInformationForEntities(hkpEntity** entities, int numEntities, hkpWorld* world);
	virtual void	removeCollisionInformationForAgent(hkpAgentNnEntry* agent);
	virtual void	assertThereIsNoCollisionInformationForAgent(hkpAgentNnEntry* agent);
	virtual void	reintegrateAndRecollideEntities(hkpEntity** entityBatch, int numEntities, hkpWorld* world, int reintegrateRecollideMode);
	virtual void	collideInternal(const hkStepInfo& stepInfoIn);
	virtual void	warpTime(float deltaTime);

	UInt32		m_determinismCheckFrameCounter;
	hkpWorld*	m_world;
	UInt8		m_lastProcessingStep;
	float		m_currentTime;
	float		m_currentPsiTime;
	float		m_physicsDeltaTime;
	float		m_simulateUntilTime;
	float		m_frameMarkerPsiSnap;
	UInt32		m_previousStepResult;
};

ASSERT_SIZE(hkpSimulation, 0x2C);