#pragma once

#include "bhkCharacterListener.hpp"
#include "bhkCharacterProxy.hpp"
#include "BSSimpleList.hpp"
#include "hkpCharacterContext.hpp"
#include "hkpCharacterProxyListener.hpp"
#include "hkpSurfaceInfo.hpp"
#include "hkStepInfo.hpp"

class Projectile;
class bhkCharControllerShape;
class bhkCharacterControllerCinfo;

NiSmartPointer(hkpRigidBody);
NiSmartPointer(bhkCapsuleShape);
NiSmartPointer(bhkCachingShapePhantom);
NiSmartPointer(bhkCharacterController);

__declspec(align(16))
class bhkCharacterController : public bhkCharacterProxy, public hkpCharacterContext, public bhkCharacterListener, public hkpSurfaceInfo {
public:
	enum CharacterSize : UInt32 {
		NORMAL	= 0,
		SIZED	= 1,
		SIZING	= 2,
	};

	struct MovementParameters {
		float		fDeltaTime;
		NiPoint3	kRotation;
		NiPoint3	kVelocity;
		UInt32		uiMoveFlags;
		float		fMovementSpeed;
		bool		bShouldUpdate;
	};


	virtual void InitWithParams(bhkCharacterControllerCinfo* apInfo);
	virtual void Move(MovementParameters& arParams);
	virtual void Unk_51();
	virtual void SetDebugDisplay(void* apUnk = nullptr);


	hkVector4					kUpVec;
	hkVector4					kForwardVec;
	hkStepInfo					kStepInfo;
	hkVector4					kOutVelocity;
	hkVector4					kVelocityMod;
	hkVector4					kDirection;
	CharacterState				eWantState;
	float						fVelocityTime;
	float						fRotMod;
	float						fRotModTime;
	float						fCalculatePitchTimer;
	float						fAcrobatics;
	float						fCenter;
	float						fWaterHeight;
	float						fJumpHeight;
	float						fFallStartHeight;
	float						fFallTime;
	float						fGravity;
	float						fPitchAngle;
	float						fRollAngle;
	float						fPitchMult;
	float						fScale;
	float						fSwimFloatHeight;
	float						fActorHeight;
	float						fSpeedPct;
	UInt32						unk56C;
	hkVector4					kRotCenter;
	hkVector4					kPushDelta;
	UInt32						iPushCount;
	bhkCachingShapePhantomPtr	spChrPhantom;
	UInt32						unk598;
	UInt32						eShapeType;
	UInt32						eSizedShapeType;
	bhkCapsuleShapePtr			spShapes[2];
	UInt32						unk5AC;
	float						unk5B0;
	float						unk5B4;
	float						unk5B8;
	float						unk5BC;
	float						unk5C0;
	float						unk5C4;
	float						unk5C8;
	float						unk5CC;
	float						unk5D0;
	float						unk5D4;
	float						unk5D8;
	float						unk5DC;
	float						unk5E0;
	float						unk5E4;
	float						unk5E8;
	float						unk5EC;
	float						fRadius;
	float						fHeight;
	float						fDestRadius;
	float						fDistFromPlayersEyes;
	CharacterSize				eSize;
	UInt32						iPriority;
	UInt32						unk608;
	hkpRigidBodyPtr				spSupportBody;
	bool						bFakeSupport;
	UInt32						unk614;
	UInt32						unk618;
	UInt32						unk61C;
	hkVector4					kFakeSupportStart;
	UInt32						spBumpedBody;
	float						fBumpedForce;
	UInt32						spBumpedCharCollisionObject;
	hkArray<hkpCdPoint>			arr63C;
	BSSimpleList<UInt32>		damageImpacts;
	Projectile*					pProjectile; // Projectile

	NiAVObject* GetNiObject() const;

	NiNode* GetCollisionDebugNode();
	void RemoveCollisionDebug();

	void GetVelocity(NiPoint3& arVelocity) const;

	hkVector4* GetPosition(hkVector4& arPosition) const;
	void GetPosition(NiPoint3& arPosition) const;

	inline bool IsFlying() const {
		return ThisStdCall<bool>(0x5C0860, this);
	}

	inline void SetPosition(const NiPoint3 *point) {
		ThisStdCall(0x5620E0, this, point);
	}
};

ASSERT_SIZE(bhkCharacterController, 0x660);