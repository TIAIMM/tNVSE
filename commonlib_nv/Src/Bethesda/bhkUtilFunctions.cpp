#include "bhkUtilFunctions.hpp"
#include "TESObjectREFR.hpp"
#include "bhkCollisionObject.hpp"
#include "bhkRigidBody.hpp"

// GAME - 0xC80330
void bhkUtilFunctions::SetUseVelocityRecurse(NiAVObject* apObject, bool abUseVelocity) {
	CdeclCall(0xC80330, abUseVelocity, apObject);
}

// GAME - 0x62BA80
void bhkUtilFunctions::AddAngularVelocity(NiAVObject* apObject, NiPoint3& arVelocity, bool abReset) {
	CdeclCall(0x62BA80, apObject, &arVelocity, abReset);
}

// GAME - 0x62B8D0
void bhkUtilFunctions::AddVelocity(NiAVObject* apObject, NiPoint3& arVelocity, bool abReset) {
	CdeclCall(0x62B8D0, apObject, &arVelocity, abReset);
}

// GAME - 0xC7B140
void bhkUtilFunctions::FindBonesAndConstraints(const NiAVObject* apObject, hkArray<hkReferencedObject*>& arBones, hkArray<bhkConstraint*>& arConstraints) {
	CdeclCall(0xC7B140, apObject, &arBones, &arConstraints);
}

// GAME - 0xC802D0
bhkNiCollisionObject* bhkUtilFunctions::FindCollisionObject(const NiAVObject* apObject) {
	return CdeclCall<bhkNiCollisionObject*>(0xC802D0, apObject);
}

// GAME - 0xC7FA90
NiAVObject* bhkUtilFunctions::GetNiAVObject(const hkpCollidable* apCollidable) {
	return CdeclCall<NiAVObject*>(0xC7FA90, apCollidable);
}

// GAME - 0x4B5820
// GECK - 0x525F30
NiAVObject* bhkUtilFunctions::GetObjectFromCollidable(const hkpCollidable* apCollidable) {
	return CdeclCall<NiAVObject*>(0x4B5820, apCollidable);
}

// GAME - 0x9C69E0
TESObjectREFR* bhkUtilFunctions::GetReferenceFromCollidable(const hkpCollidable* apCollidable) {
	NiAVObject* pObject = GetNiAVObject(apCollidable);
	if (pObject)
		return TESObjectREFR::FindReferenceFor3D(pObject);

	return nullptr;
}

// GAME - 0x9A8660
NiPoint3* bhkUtilFunctions::GetActorVelocity(NiPoint3& arResult, const Actor* apActor) {
	return CdeclCall<NiPoint3*>(0x9A8660, &arResult, apActor);
}

// GAME - 0xCDD390
UInt16 bhkUtilFunctions::GetBoneFromName(const hkaSkeleton* apSkeleton, const char* apName, BoneCompareFunc apFunc) {
	return CdeclCall<UInt16>(0xCDD390, apSkeleton, apName, apFunc);
}

// GAME - 0x4B5A50
bhkNiCollisionObject* bhkUtilFunctions::GetbhkNiCollisionObject(const hkpWorldObject* apObject) {
	return CdeclCall<bhkNiCollisionObject*>(0x4B5A50, apObject);
}

// GAME - 0x4A8B00
bhkNiCollisionObject* bhkUtilFunctions::GetbhkNiCollisionObject(const NiAVObject* apObject) {
	return CdeclCall<bhkNiCollisionObject*>(0x4A8B00, apObject);
}

// GAME - 0x43B610
bhkCollisionObject* bhkUtilFunctions::GetbhkCollisionObject(const NiAVObject* apObject) {
	return CdeclCall<bhkCollisionObject*>(0x43B610, apObject);
}

// GAME - 0x62B150
UInt32 bhkUtilFunctions::GetMaterialFromCollidable(const hkpCollidable* apCollidable, const NiPoint3& arLandPos) {
	return CdeclCall<UInt32>(0x62B150, apCollidable, &arLandPos);
}

// GAME - 0xC81B90
double bhkUtilFunctions::GetHeirGain(const NiNode* apNode, const char* apBoneName) {
	return CdeclCall<double>(0xC81B90, apNode, apBoneName);
}

// GAME - 0xC85EF0
hkSimplePropertyValue* bhkUtilFunctions::GetPropertyFromRTTI(hkSimplePropertyValue& arResult, const hkpWorldObject* apObject, const NiRTTI* apRTTI) {
	return CdeclCall<hkSimplePropertyValue*>(0xC85EF0, &arResult, apObject, apRTTI);
}

// GAME - 0x4A9240
UInt32 bhkUtilFunctions::GetTriangleCountOfShape(void* apShape) {
	return CdeclCall<UInt32>(0x4A9240, apShape);
}

// GAME - 0x4B5AD0
bhkPCollisionObject* bhkUtilFunctions::IsbhkPCollisionObject(const hkpWorldObject* apObject) {
	return CdeclCall<bhkPCollisionObject*>(0x4B5AD0, apObject);
}

// GAME - 0xC6C3D0
void bhkUtilFunctions::PerformSync(NiAVObject* apObject, UInt32 aeControllerUpdate) {
	CdeclCall(0xC6C3D0, apObject, aeControllerUpdate);
}

// GAME - 0x62BC10
void bhkUtilFunctions::RemoveHavokFromSceneGraph(NiAVObject* apObject) {
	CdeclCall(0x62BC10, apObject);
}

// GAME - 0xC6C210
void bhkUtilFunctions::SetBoneNamesRecurse(NiAVObject* apObject) {
	CdeclCall(0xC6C210, apObject);
}