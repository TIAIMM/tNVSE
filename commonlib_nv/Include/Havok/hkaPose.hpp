#pragma once

#include "hkArray.hpp"
#include "hkQsTransform.hpp"

class hkaSkeleton;

class hkaPose {
public:
	enum PropagateOrNot {
		DONT_PROPAGATE = 0,
		PROPAGATE = 1
	};


	hkaSkeleton*			m_skeleton;
	hkArray<hkQsTransform>	m_localPose;
	hkArray<hkQsTransform>	m_modelPose;
	hkArray<UInt32>			m_boneFlags;
	bool					m_modelInSync;
	bool					m_localInSync;
	hkArray<float>			m_floatSlotValues;

	void syncLocalSpace();
	void syncModelSpace();

	hkQsTransform& accessBoneModelSpace(SInt32 boneIdx, PropagateOrNot propagateOrNot = DONT_PROPAGATE);

	const hkQsTransform& calculateBoneModelSpace(SInt32 boneIdx) const;

	hkArray<hkQsTransform>& accessSyncedPoseLocalSpace();
	hkArray<hkQsTransform>& accessSyncedPoseModelSpace();

	hkArray<hkQsTransform>& accessUnsyncedPoseModelSpace();

	hkArray<hkQsTransform>& getSyncedPoseLocalSpace();
	hkArray<hkQsTransform>& getSyncedPoseModelSpace();

	void enforceSkeletonConstraintsModelSpace();
};

ASSERT_SIZE(hkaPose, 0x38);