#pragma once

#include "hkpWorldObject.hpp"
#include "hkpMaterial.hpp"
#include "hkSmallArray.hpp"
#include "hkpMotion.hpp"

class hkpSimulationIsland;
class hkpContactListener;
class hkpAction;
class hkpBreakableBody;
class hkLocalFrame;
class hkpEntityActivationListener;
class hkpEntityListener;
class hkpConstraintInstance;

class hkpEntity : public hkpWorldObject {
public:
	struct ExtendedListeners {
		hkSmallArray<hkpEntityActivationListener*>	m_activationListeners;
		hkSmallArray<hkpEntityListener*>			m_entityListeners;
	};

	struct SpuCollisionCallback {
		void*	m_util;
		UInt16	m_capacity;
		UInt8	m_eventFilter;
		UInt8	m_userFilter;
	};

	hkpMaterial							m_material;
	void*								m_limitContactImpulseUtil;
	float								m_damageMultiplier;
	hkpBreakableBody*					m_breakableBody;
	UInt32								m_solverData;
	UInt16								m_storageIndex;
	UInt16								m_contactPointCallbackDelay;
	hkSmallArray<void>					m_constraintsMaster;
	hkArray<hkpConstraintInstance*>		m_constraintsSlave;
	hkArray<UInt8>						m_constraintRuntime;
	hkpSimulationIsland*				m_simulationIsland;
	SInt8								m_autoRemoveLevel;
	UInt8								m_numShapeKeysInContactPointProperties;
	UInt8								m_responseModifierFlags;
	UInt32								m_uid;
	SpuCollisionCallback				m_spuCollisionCallback;
	hkpMotion							m_motion;
	hkSmallArray<hkpContactListener*>	m_contactListeners;
	hkSmallArray<hkpAction* >			m_actions;
	hkLocalFrame*						m_localFrame;
	ExtendedListeners*					m_extendedListeners;
	unsigned int						m_npData;
	void*								unk21C;
};

ASSERT_SIZE(hkpEntity, 0x220);