#pragma once

#include "hkReferencedObject.hpp"
#include "hkaSkeletonMapperData.hpp"

class hkaPose;

class hkaSkeletonMapper : public hkReferencedObject {
public:
	enum ConstraintSource {
		NO_CONSTRAINTS	= 0,
		REFERENCE_POSE	= 1,
		CURRENT_POSE	= 2,
	};

	hkaSkeletonMapperData m_mapping;

	void mapPose(hkaPose* poseAIn, hkaPose* poseBInOut, ConstraintSource source);
	void mapPose(const hkQsTransform* poseAModelSpace, const hkQsTransform* originalPoseBLocalSpace, hkQsTransform* poseBModelSpace, ConstraintSource source);
	void mapPoseLocalSpace(const hkQsTransform* poseALocalSpace, hkQsTransform* poseBLocalSpaceInOut, bool additive);
	void mapPoseLocalSpace(const hkQsTransform* poseALocalSpaceOriginal, hkQsTransform* poseBLocalSpaceInOut, const SInt16* boneToTrackIndicesA, const SInt16* boneToTrackIndicesB, bool additive);
};