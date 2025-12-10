#include "BSCompoundFrustum.hpp"
#include "MemoryManager.hpp"
#include "NiCamera.hpp"


BSCompoundFrustum::BSCompoundFrustum() {
	iFreePlane = 0;
	iFreeOp = 0;
	kViewPosition = NiPoint3::ZERO;
	kPlanes.SetSize(4, true);
	kFunctionOperators.SetSize(10, true);
	bPrethreaded = false;
	iFirstOp = 0;
	bSkipViewFrustum = false;
}

BSCompoundFrustum::~BSCompoundFrustum() {
	kPlanes.Clear(true);
	kFunctionOperators.Clear(true);
}

BSCompoundFrustum* BSCompoundFrustum::Create(BSCompoundFrustum* apThis) {
	return ThisStdCall<BSCompoundFrustum*>(0xC47660, apThis);
}

BSCompoundFrustum* BSCompoundFrustum::CreateObject() {
	return BSCreate<BSCompoundFrustum, 0xC47660>();
}

// GAME - 0xC477F0
void BSCompoundFrustum::CopyMembers(BSCompoundFrustum* apTarget) {
	ThisStdCall(0xC477F0, this, apTarget);
}

// GAME - 0xC4A6E0
bool BSCompoundFrustum::CullCheck(NiBound* apBound, NiFrustumPlanes* apPlanes) {
	return ThisStdCall<bool>(0xC477F0, this, apBound, apPlanes);
}

// GAME - 0xC4A790
bool BSCompoundFrustum::CullCheckAlt(NiBound* apBound, NiFrustumPlanes* apPlanes) {
	return ThisStdCall<bool>(0xC4A790, this, apBound, apPlanes);
}

// GAME - 0xC491A0
bool BSCompoundFrustum::Process(NiAVObject* apObject) {
	return ThisStdCall<bool>(0xC491A0, this, apObject);
}

// GAME - 0xC49560
bool BSCompoundFrustum::ProcessPlane(BSOcclusionPlane* apPlane) {
	return ThisStdCall(0xC49560, this, apPlane);
}

// GAME - 0xC49050
UInt32* BSCompoundFrustum::GetActivePlaneState() {
	return ThisStdCall<UInt32*>(0xC49050, this);
}

// GAME - 0xC49100
void BSCompoundFrustum::SetActivePlaneState(UInt32* apState) {
	ThisStdCall(0xC49050, this, apState);
}

// GAME - 0xC49850
void BSCompoundFrustum::PrethreadOpList() {
	ThisStdCall(0xC49850, this);
}

// GAME - 0xC47980
void BSCompoundFrustum::SetCamera(const NiCamera* apCamera) {
	ThisStdCall(0xC47980, apCamera);
}

// GAME - 0xC47A00
void BSCompoundFrustum::StartGroupIntersection() {
	ThisStdCall(0xC47A00, this);
}

// GAME - 0xC47D20
void BSCompoundFrustum::AddOcclusionPlane(BSOcclusionPlane* apPlane) {
	ThisStdCall(0xC47D20, this, apPlane);
}

// GAME - 0xC47AE0
void BSCompoundFrustum::EndGroup() {
	ThisStdCall(0xC47AE0, this);
}

// GAME - 0xC48B90
void BSCompoundFrustum::MakeIntoUnion() {
	ThisStdCall(0xC48B90, this);
}

// GAME - 0xC48DB0
void BSCompoundFrustum::BuildForPortal(BSCompoundFrustum* apExistingFrustum, BSPortal* apPortal) {
	ThisStdCall(0xC48DB0, this, apExistingFrustum, apPortal);
}

// GAME - 0xC48800
void BSCompoundFrustum::AddCompoundFrustum(BSCompoundFrustum* apFrustum) {
	ThisStdCall(0xC48800, this, apFrustum);
}

// GAME - 0xC47B50
void BSCompoundFrustum::AddPortal(BSPortal* apPortal) {
	ThisStdCall(0xC47B50, this, apPortal, 0);
}
