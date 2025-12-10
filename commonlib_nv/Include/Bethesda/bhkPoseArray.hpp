#pragma once

#include "NiObject.hpp"
#include "NiFixedString.hpp"

class hkMatrix3;
class hkaSkeleton;
class hkQsTransform;

class bhkPoseArray : public NiObject {
public:
	struct Pose {
		UInt32			uiBoneCount;
		hkQsTransform*	pBoneTransforms;
	};


	UInt32			uiNameCount;
	NiFixedString*	pBoneNames;
	UInt32			uiPoseCount;
	Pose**			ppPoses;

	CREATE_OBJECT(bhkPoseArray, 0xCC1290);

	bool Validate(hkaSkeleton* apSkeleton);
	void GetPose(hkQsTransform* apBoneTransforms, hkaSkeleton* apSkeleton, UInt32 auiPoseIndex);
};

ASSERT_SIZE(bhkPoseArray, 0x18);