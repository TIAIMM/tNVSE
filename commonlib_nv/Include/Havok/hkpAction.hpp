#pragma once

#include "hkReferencedObject.hpp"
#include "hkArray.hpp"
#include "hkStringPtr.hpp"

class hkStepInfo;
class hkpEntity;
class hkpPhantom;
class hkpWorld;
class hkpSimulationIsland;

class hkpAction : public hkReferencedObject {
public:
	virtual void		applyAction(const hkStepInfo& stepInfo);
	virtual void		getEntities(hkArray<hkpEntity*>& entitiesOut);
	virtual void		getPhantoms(hkArray<hkpPhantom*>*& phantomsOut);
	virtual void		entityRemovedCallback(hkpEntity* entity);
	virtual hkpAction*	clone(const hkArray<hkpEntity*>& newEntities, const hkArray<hkpPhantom*>& newPhantoms);

	hkpWorld*				m_world;
	hkpSimulationIsland*	m_island;
	void*					m_userData;
	hkStringPtr				m_name;
};

ASSERT_SIZE(hkpAction, 0x18);