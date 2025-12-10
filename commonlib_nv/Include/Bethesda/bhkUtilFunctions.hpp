#pragma once

#include "hkArray.hpp"

class NiPoint3;
class NiMatrix3;
class NiTransform;
class NiAVObject;
class NiNode;
class NiRTTI;

class bhkNiCollisionObject;
class bhkCollisionObject;
class bhkPCollisionObject;
class bhkConstraint;

class hkSimplePropertyValue;
class hkReferencedObject;
class hkpWorldObject;
class hkpCollidable;
class hkaSkeleton;
class hkRotation;
class hkTransform;

class Actor;
class TESObjectREFR;

namespace bhkUtilFunctions {

	typedef int(__cdecl* BoneCompareFunc)(const char*, const char*);

	void					SetUseVelocityRecurse(NiAVObject* apObject, bool abUseVelocity);
	void					AddAngularVelocity(NiAVObject* apObject, NiPoint3& arVelocity, bool abReset);
	void					AddVelocity(NiAVObject* apObject, NiPoint3& arVelocity, bool abReset);
	void					FindBonesAndConstraints(const NiAVObject* apObject, hkArray<hkReferencedObject*>& arBones, hkArray<bhkConstraint*>& arConstraints);
	bhkNiCollisionObject*	FindCollisionObject(const NiAVObject* apObject);
	NiAVObject*				GetNiAVObject(const hkpCollidable* apCollidable);
	NiAVObject*				GetObjectFromCollidable(const hkpCollidable* apCollidable);
	TESObjectREFR*			GetReferenceFromCollidable(const hkpCollidable* apCollidable);
	NiPoint3*				GetActorVelocity(NiPoint3& arResult, const Actor* apActor);
	UInt16					GetBoneFromName(const hkaSkeleton* apSkeleton, const char* apName, BoneCompareFunc apFunc = _stricmp);
	void					ToggleConstraints(NiAVObject* apObject, bool abFix, bool abAddRemainingConstraints);

	bhkNiCollisionObject*	GetbhkNiCollisionObject(const hkpWorldObject* apObject);
	bhkNiCollisionObject*	GetbhkNiCollisionObject(const NiAVObject* apObject);
	bhkCollisionObject*		GetbhkCollisionObject(const hkpWorldObject* apObject);
	bhkCollisionObject*		GetbhkCollisionObject(const NiAVObject* apObject);

	UInt32					GetMaterialFromCollidable(const hkpCollidable* apCollidable, const NiPoint3& arLandPos);

	double					GetHeirGain(const NiNode* apNode, const char* apBoneName = nullptr);
	hkSimplePropertyValue*	GetPropertyFromRTTI(hkSimplePropertyValue& arResult, const hkpWorldObject* apObject, const NiRTTI* apRTTI);
	UInt32					GetTriangleCountOfShape(void* apShape);
	bhkPCollisionObject*	IsbhkPCollisionObject(const hkpWorldObject* apObject);
	void					PerformSync(NiAVObject* apObject, UInt32 aeControllerUpdate);
	void					RemoveHavokFromSceneGraph(NiAVObject* apObject);
	void					SetBoneNamesRecurse(NiAVObject* apObject);

	extern const __m128 kHK2NI;
	extern const __m128 kNI2HK;

	__m128& NI2HKMIGRATION_POINT3(__m128& aOut, const NiPoint3& aIn);

	void NI2HKMIGRATION_MATRIX3(hkRotation& aOut, const NiMatrix3& aIn);

	void NI2HKMIGRATION_TRANSFORM(hkTransform& aOut, const NiTransform& aIn);

	NiPoint3& HK2NIMIGRATION_POINT3(NiPoint3& aOut, const __m128& aIn);

	NiPoint3& HK2NIMIGRATION_POINT3_ROT(NiPoint3& aOut, const __m128& aIn);

	NiMatrix3& HK2NIMIGRATION_MATRIX3(NiMatrix3& aOut, const hkRotation& aIn);

	NiTransform& HK2NIMIGRATION_TRANSFORM(NiTransform& aOut, const hkTransform& aIn);
}