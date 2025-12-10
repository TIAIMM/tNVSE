#pragma once

#include "hkReferencedObject.hpp"
#include "hkpConstraintInfo.hpp"

class hkpSolverResults;
class hkpConstraintRuntime;
class hkpConstraintInstance;
class hkpConstraintQueryIn;
class hkpConstraintQueryOut;

class hkpConstraintData : public hkReferencedObject {
public:
	enum ConstraintType {
		CONSTRAINT_TYPE_BALLANDSOCKET		= 0,
		CONSTRAINT_TYPE_HINGE				= 1,
		CONSTRAINT_TYPE_LIMITEDHINGE		= 2,
		CONSTRAINT_TYPE_POINTTOPATH			= 3,
		CONSTRAINT_TYPE_PRISMATIC			= 6,
		CONSTRAINT_TYPE_RAGDOLL				= 7,
		CONSTRAINT_TYPE_STIFFSPRING			= 8,
		CONSTRAINT_TYPE_WHEEL				= 9,
		CONSTRAINT_TYPE_GENERIC				= 10,
		CONSTRAINT_TYPE_CONTACT				= 11,
		CONSTRAINT_TYPE_BREAKABLE			= 12,
		CONSTRAINT_TYPE_MALLEABLE			= 13,
		CONSTRAINT_TYPE_POINTTOPLANE		= 14,
		CONSTRAINT_TYPE_PULLEY				= 15,
		CONSTRAINT_TYPE_ROTATIONAL			= 16,

		CONSTRAINT_TYPE_HINGE_LIMITS		= 18,
		CONSTRAINT_TYPE_RAGDOLL_LIMITS		= 19,
		CONSTRAINT_TYPE_CUSTOM				= 20,

		// Constraint Chains
		BEGIN_CONSTRAINT_CHAIN_TYPES		= 100,
		CONSTRAINT_TYPE_STIFF_SPRING_CHAIN	= 100,
		CONSTRAINT_TYPE_BALL_SOCKET_CHAIN	= 101,
		CONSTRAINT_TYPE_POWERED_CHAIN		= 102
	};

	struct RuntimeInfo {
		SInt32 m_sizeOfExternalRuntime;
		SInt32 m_numSolverResults;
	};

	struct ConstraintInfo : public hkpConstraintInfo {
		struct hkpConstraintAtom* m_atoms;
		UInt32					  m_sizeOfAllAtoms;
	};

	virtual void				setMaxLinearImpulse(float maxImpulse);
	virtual float				getMaxLinearImpulse() const;
	virtual void				setBodyToNotify(SInt32 bodyIdx);
	virtual UInt8				getNotifiedBodyIndex() const;
	virtual bool&				isValid(bool& arResult) const;
	virtual ConstraintType		getType() const;
	virtual void				getRuntimeInfo(bool wantRuntime, RuntimeInfo& infoOut) const;
	virtual hkpSolverResults*	getSolverResults(hkpConstraintRuntime* runtime);
	virtual void				addInstance(hkpConstraintInstance* constraint, hkpConstraintRuntime* runtime, int sizeOfRuntime) const;
	virtual void				buildJacobian(const hkpConstraintQueryIn& in, hkpConstraintQueryOut& out);
	virtual bool				isBuildJacobianCallbackRequired() const;
	virtual void				buildJacobianCallback(const hkpConstraintQueryIn& in);
	virtual void				getConstraintInfo(ConstraintInfo& infoOut);

	void* m_userData;
};

ASSERT_SIZE(hkpConstraintData, 0xC);