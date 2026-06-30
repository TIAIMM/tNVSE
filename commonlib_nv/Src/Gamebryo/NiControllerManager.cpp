#include "NiControllerManager.hpp"

// 0xA2EFA0
NiControllerManager* NiControllerManager::Create(NiAVObject* apTarget, bool abCumulativeAnimations) {
	return NiCreate<NiControllerManager, 0xA2EFA0>(apTarget, abCumulativeAnimations);
}

// 0x490D90
NiAVObject* NiControllerManager::GetAccumRoot() const {
	if (m_bCumulative) {
		for (UInt32 i = 0; i < GetSequenceCount(); i++) {
			NiControllerSequence* pSequence = GetSequenceAt(i);
			if (pSequence)
				return pSequence->GetAccumRoot();
		}
	}
	return nullptr;
}

// 0x47A520
NiControllerSequence* NiControllerManager::GetSequenceByName(const NiFixedString& arName) {
	NiControllerSequence* pOut = nullptr;
	m_kIndexMap.GetAt(arName.m_kHandle, pOut);
	return pOut;
}

// 0x495D20
NiControllerSequence* NiControllerManager::GetSequenceAt(UInt32 auiIndex) const {
	return m_kSequenceArray.GetAt(auiIndex);
}

// 0x495D00
UInt32 NiControllerManager::GetSequenceCount() const {
	return m_kSequenceArray.GetSize();
}

// 0xA2E1B0
bool NiControllerManager::Morph(NiControllerSequence* apSourceSequence, NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority, float afSourceWeight, float afDestWeight) {
	return apSourceSequence->StartMorph(apDestSequence, afDuration, aiPriority, afSourceWeight, afDestWeight);
}

// 0xA2E280
bool NiControllerManager::CrossFade(NiControllerSequence* apSourceSequence, NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority, bool abStartOver, float afWeight, NiControllerSequence* apTimeSyncSeq) {
	if (apSourceSequence->m_eState == NiControllerSequence::INACTIVE || apDestSequence->m_eState != NiControllerSequence::INACTIVE)
		return false;

	apSourceSequence->Deactivate(afDuration, false);
	return apDestSequence->Activate(aiPriority, abStartOver, afWeight, afDuration, apTimeSyncSeq, false);
}

// 0xA2F170
NiControllerSequence* NiControllerManager::CreateTempBlendSequence(NiControllerSequence* apSequence, NiControllerSequence* apSequenceToSynchronize) {
	return ThisStdCall<NiControllerSequence*>(0xA2F170, this, apSequence, apSequenceToSynchronize);
}

// 0xA2F800
bool NiControllerManager::BlendFromPose(NiControllerSequence* apSequence, float afDestFrame, float afDuration, SInt32 aiPriority, NiControllerSequence* apSequenceToSynchronize) {
	if (apSequence->m_eState)
		return false;

	NiControllerSequence* pTempBlendSeq = CreateTempBlendSequence(apSequence, apSequenceToSynchronize);
	return pTempBlendSeq->StartBlend(apSequence, afDuration, afDestFrame, aiPriority, 1.0f, 1.0f, nullptr);
}

// 0xA2F0C0
bool NiControllerManager::AddSequence(NiControllerSequence* apSequence, const NiFixedString& arName, bool abStoreTargets) {
	return ThisStdCall<bool>(0xA2F0C0, this, apSequence, &arName, abStoreTargets);
}

// 0xA2EC50
void NiControllerManager::RemoveSequence(NiControllerSequence* apSequence) {
	NiControllerSequencePtr spSequence;
	RemoveSequence(apSequence, spSequence);
}

// 0xA2E7D0
void NiControllerManager::RemoveSequence(NiControllerSequence* apSequence, NiControllerSequencePtr& arOut) {
	ThisStdCall(0xA2E7D0, this, apSequence, &arOut);
}

// 0x47AAB0
bool NiControllerManager::ActivateSequence(NiControllerSequence* apSequence, char acPriority, bool abStartOver, float afWeight, float afEaseInTime, NiControllerSequence* apTimeSyncSeq) {
	return apSequence->Activate(acPriority, abStartOver, afWeight, afEaseInTime, apTimeSyncSeq, false);
}

// 0x47B220
bool NiControllerManager::DeactivateSequence(NiControllerSequence* apSequence, float afEaseOutTime) {
	return apSequence->Deactivate(afEaseOutTime, false);
}

// 0xA2E420
bool NiControllerManager::DeactivateTransSourceSequence(float afEaseOutTime) {
	for (UInt32 i = 0; i < m_kActiveSequences.GetSize(); i++) {
		NiControllerSequence* pSequence = m_kActiveSequences.GetAt(i);
		if (pSequence->m_eState == NiControllerSequence::TRANSSOURCE)
			pSequence->Deactivate(afEaseOutTime, false);
	}
	return false;
}

// 0x48FEF0
void NiControllerManager::DeactivateAll(float afEaseOutTime) {
	for (UInt32 i = 0; i < m_kActiveSequences.GetSize(); i++) {
		NiControllerSequence* pSequence = m_kActiveSequences.GetAt(i);
		DeactivateSequence(pSequence, afEaseOutTime);
	}
}
