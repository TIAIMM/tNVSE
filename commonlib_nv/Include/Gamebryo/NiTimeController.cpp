#include "NiTimeController.hpp"

void NiTimeController::SetAnimType(AnimType aeType) {
	m_usFlags.SetField(aeType, Flags::ANIMTYPE_MASK, Flags::ANIMTYPE_POS);
	SetActive(aeType == APP_TIME);
}

// NiTimeController::AnimType NiTimeController::GetAnimType() const {
// 	return (AnimType)m_usFlags.GetField(Flags::ANIMTYPE_MASK, Flags::ANIMTYPE_POS);
// }

void NiTimeController::SetCycleType(CycleType aeType) {
	m_usFlags.SetField(aeType, Flags::CYCLETYPE_MASK, Flags::CYCLETYPE_POS);
}

// NiTimeController::CycleType NiTimeController::GetCycleType() const {
// 	return (CycleType)m_usFlags.Get(Flags::CYCLETYPE_MASK, Flags::CYCLETYPE_POS);
// }

// 0x47AA40
void NiTimeController::SetActive(bool abActive) {
	m_usFlags.SetBit(Flags::ACTIVE_MASK, abActive);
}

// 0x4F05A0
// bool NiTimeController::GetActive() const {
// 	return m_usFlags.GetBit(Flags::ACTIVE_MASK);
// }

// 0x566930
void NiTimeController::SetComputeScaledTime(bool abComputeScaledTime) {
	m_usFlags.SetBit(Flags::COMPUTESCALEDTIME_MASK, abComputeScaledTime);
}

// bool NiTimeController::GetComputeScaledTime() const {
// 	return m_usFlags.GetBit(Flags::COMPUTESCALEDTIME_MASK);
// }

// 0xA6D2D0
void NiTimeController::StartAnimations(NiAVObject* apObject) {
	CdeclCall(0xA6D2D0, apObject);
}

void NiTimeController::StartAnimations2(NiAVObject* apObject, float fTime) {
	CdeclCall(0xA6D390, apObject, fTime);
}
