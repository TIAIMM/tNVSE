#pragma once

#include "hkReferencedObject.hpp"
#include "hkVector4.hpp"

class hkaPose;

class hkaTwoJointsIkSolver : public hkReferencedObject {
public:
	struct Setup {
		SInt16		m_firstJointIdx			= -1;
		SInt16		m_secondJointIdx		= -1;
		SInt16		m_endBoneIdx			= -1;
		hkVector4	m_hingeAxisLS			= hkVector4();
		float		m_cosineMaxHingeAngle	= -1.f;
		float		m_cosineMinHingeAngle	= 1.f;
		float		m_firstJointIkGain		= 1.f;
		float		m_secondJointIkGain		= 1.f;
		hkVector4	m_endTargetMS			= hkVector4();
	};

	static bool solve(bool& arResult, const Setup& arSetup, hkaPose& arPoseInOut);
};