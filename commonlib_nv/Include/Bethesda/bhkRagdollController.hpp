#pragma once

#include "hkaRaycastInterface.hpp"
#include "BSSimpleArray.hpp"
#include "NiFixedString.hpp"
#include "NiAVObject.hpp"
#include "BSPrecisionTimer.hpp"
#include "NiPoint4.hpp"
#include "hkMatrix3.hpp"
#include "hkArray.hpp"
#include "hkQuaternion.hpp"
#include "hkQsTransform.hpp"

class hkaRagdollInstance;
class bhkPoseArray;
class hkpWorldObject;
class hkaFootPlacementIkSolver;
class hkpCachingShapePhantom;
class hkaSkeleton;
class hkpRigidBody;
class hkaPose;

NiSmartPointer(bhkRagdollShareData);

__declspec(align(16)) class bhkRagdollController : public hkaRaycastInterface {
public:
	struct GrabIKData {
		NiAVObject*	pTarget;
		float		fGrabGain;
		float		fJointGain;
		bool		bStart;
		bool		bStop;
		hkVector4	kPoint;
		hkVector4	kPoint2;
	};

	struct GrabIKBones {
		UInt16	usUpperArmIdx;
		UInt16	usForeArmIdx;
		UInt16	usHandIdx;
	};

	enum GrabIKArm {
		ARM_LEFT	= 0,
		ARM_RIGHT	= 1,
		ARM_COUNT	= 2,
	};

	struct FootIKData {
		hkaFootPlacementIkSolver* pFootPlacementSolver;
		float fUnk4;
		DWORD unk8;
		DWORD unkC;
		__m128 unk10[3];
		__m128* unk40;
		DWORD unk44;
		DWORD unk48;
		DWORD unk4C;
		DWORD unk50;
		DWORD unk54;
		DWORD unk58;
		DWORD unk5C;
		DWORD unk60;
		DWORD unk64;
		DWORD unk68;
		DWORD unk6C;
		DWORD unk70;
		DWORD unk74;
		DWORD unk78;
		DWORD unk7C;
		DWORD unk80;
		DWORD unk84;
		DWORD unk88;
		DWORD unk8C;
		DWORD unk90;
		DWORD unk94;
		DWORD unk98;
		DWORD unk9C;
		DWORD unkA0;
		DWORD unkA4;
		DWORD unkA8;
		DWORD unkAC;
		DWORD unkB0;
		DWORD unkB4;
		DWORD unkB8;
		DWORD unkBC;
		UInt16 usBone;
		DWORD unkC4;
		DWORD unkC8;
		DWORD unkCC;
	};

	struct RagdollAnimData {
		struct RagdollParams {
			float fHierarchyGain;
			float fVelocityDamping;
			float fAccelerationGain;
			float fVelocityGain;
			float fPositionGain;
			float fPositionMaxLinearVelocity;
			float fPositionMaxAngularVelocity;
			float fSnapGain;
			float fSnapMaxLinearVelocity;
			float fSnapMaxAngularVelocity;
			float fSnapMaxLinearDistance;
			float fSnapMaxAngularDistance;
		};

		struct MotionData {
			hkVector4		kCenterOfMass1;
			hkQuaternion	kRotation1;
			hkVector4		kLinearVelocity;
			hkVector4		kAngularVelocity;
		};


		struct BodyArray {
			UInt32			uiCount;
			hkpRigidBody**  pData;
			UInt16*			pIndices;
			UInt32			kArray_0C_data_ptr;
			float*			unk028;
		};

		hkArray<RagdollParams>	kRagdollParams;
		hkArray<UInt32>			kArray_0C;
		BodyArray				kRigidBodies;
		hkArray<MotionData>		kBodyMotionData;
		hkaRagdollInstance*		pRagdollInstance;
		hkArray<UInt16>			kIndices;
	};

	struct LookIKData {
		float fUnk;
		float fUnk4;
		float fUnk8;
		float fUnkC;
		float fUnk10;
		float fUnk14;
		float fUnk18;
		float fUnk1C;
		float fUnk20;
		float fUnk24;
		float fUnk28;
		float fUnk2C;
		float fUnk30;
		float fUnk34;
		float fUnk38;
		float fUnk3C;
		float fUnk40;
		float fUnk44;
		float fUnk48;
		float fUnk4C;
	};


	struct BoneTransf {
		NiNode*		pBone;
		NiTransform kTransform;
	};

	struct UnkPoseData {
		hkArray<hkVector4>	unkArray0;
		__m128				oword10;
		UInt16				usNonAccumIndex;
		UInt16				usHeadIndex;
		UInt16				usUpperArmIndex;
	};


	bool						bDisableMotors;
	float						unk008;
	UInt32						unk00C;
	hkQsTransform				kWorldTransform;
	bool						bDisableIKSystems;
	bool						bGrabRelated_0x41;
	bool						bUseIKSystem;
	bool						bIK_0x43;
	bool						unk044;
	hkaRagdollInstance*			pRagdollInstance;
	RagdollAnimData*			pRagdollAnimData;
	UInt8						bStoreMotionData;
	UInt8						gap051;
	UInt8						gap052;
	UInt8						gap053;
	void*						pUnk054;
	NiNode*						pBip01Node;
	UInt32						unk05C;
	NiMatrix3					kMatrix060;
	NiPointer<NiNode>			spDebugLinesNode;
	hkaPose*					pPose_0x88;
	UInt8						unk08C[4];
	hkpCachingShapePhantom*		phkobject090;
	hkArray<hkMatrix3>			kMatrices;
	BSSimpleArray<NiAVObject*>	kBones;
	bool						bLookIKSetUp;
	bool						bLookIKEnabled;
	bool						bNoHeadTrack;
	bool						bLookIK_Active;
	bool						unk0B4;
	UInt32						unk0B8;
	UInt32						unk0BC;
	hkQuaternion				kQuaternion_0C0;
	hkQuaternion				kQuaternion_0xD0;
	hkQuaternion				kQuaternion_0xE0;
	UInt32*						unk0F0;
	UInt16						unk0F4;
	UInt16						unk0F6;
	UInt32						unk0F8;
	UInt32						unk0FC;
	hkQuaternion				kQuaternion_0x100;
	hkQuaternion				kQuaternion_0x110;
	hkQuaternion				kQuaternion_0x120;
	bool						unk130;
	bool						byte131;
	float						unk134;
	float						unk138;
	UInt32						unk13C;
	UInt32						unk140;
	UInt16						usBoneID_144;
	UInt16						unk146;
	UInt32						unk148;
	UInt32						unk14C;
	hkQuaternion				kQuaternion_0x150;
	hkQuaternion				kQuaternion_0x160;
	hkQuaternion				kQuaternion_0x170;
	bool						byte180;
	bool						byte181;
	UInt32						unk184;
	UInt32						unk188;
	UInt32						unk18C;
	bool						byte190;
	UInt8						byte191;
	UInt8						byte192;
	UInt8						byte193;
	UInt32						unk194;
	bool						unk198;
	float						fUnk19C;
	float						fUnk1A0;
	UInt8						byte1A4[4];
	float						fUnk1A8;
	UInt32						unk1AC;
	GrabIKData					kGrabIKDatas[ARM_COUNT];
	GrabIKBones					kGrabIKBones[ARM_COUNT];
	bool						bGrabIKSetUp;
	bool						bGrabIKEnabled;
	bool						bGrabIK_Active;
	bool						bFootIKSetUp;
	bool						bFootIKEnabled;
	bool						bNoPelvisDampening;
	bool						bFootIK_Active;
	bool						bNoFootOnOffGain;
	bool						byte224_falseIfLibertyPrime;
	UInt32						uiFootEaseOutFrames;
	hkArray<FootIKData>			hkArray_22C;
	UInt16						unk238;
	UInt16						unk23A;
	float						fUnk23C; // Foot height offset?
	bool						byte240_trueIfLibertyPrime;
	UInt8						byte241;
	UInt8						byte242;
	bool						bNoPelvisUpDownBias;
	float						fUnk244_FootIK;
	UnkPoseData*				pUnkPoseData248;
	NiFixedString				strDeathPSAName;
	NiPointer<bhkPoseArray>		spPoseArray;
	UInt8						byte254;
	UInt32						uiLastPose;
	UInt32						unk25C;
	BSPrecisionTimer			timer;
	UInt32						unk270;
	UInt32						unk274;
	UInt32						unk278;
	float						unk27C;
	float						unk280;
	bool						unk284;
	UInt32						uiCurrentPose;
	bool						bFeedbackSet;
	bool						bFeedbackEnabled;
	UInt8						byte28E;
	UInt8						byte28F;
	float						fLerpValue_290;
	UInt32						unk294;
	UInt32						unk298;
	UInt32						unk29C;
	NiFixedString				strBlockName;
	bhkRagdollShareDataPtr		spRagdollShareData;
	BSPrecisionTimer			kCounter2A8;
	UInt32						uiHeadBoneIndex; // for m_boneToRigidBodyMap in hkaRagdollInstance
	UInt32						unk2BC;

	void CreateDebugNodes();
	void CreateBoneLines();

	void UpdateGrabIK(hkQsTransform& arWorldTransform);
	void SetTargetGrabIKNode(GrabIKArm aeArm, NiAVObject* apTarget);
	void SetStartArmGrab(GrabIKArm aeArm, bool abValue);
	void SetStopArmGrab(GrabIKArm aeArm, bool abValue);
	void SetGrabGain(GrabIKArm aeArm, float afValue);
};

ASSERT_OFFSET(bhkRagdollController, unk130, 0x130);
ASSERT_SIZE(bhkRagdollController, 0x2C0);
ASSERT_SIZE(bhkRagdollController::BoneTransf, 0x38);