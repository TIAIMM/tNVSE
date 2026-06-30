#include "NiControllerSequence.hpp"

// 0xA32A10
NiControllerSequence* NiControllerSequence::Create(const NiFixedString& arName, UInt32 auiArraySize, UInt32 auiArrayGrowBy) {
	return NiCreate<NiControllerSequence, 0xA32A10>(&arName, auiArraySize, auiArrayGrowBy);
}

// 0xA34F20
bool NiControllerSequence::Activate(char acPriority, bool abStartOver, float afWeight, float afEaseInTime, NiControllerSequence* apTimeSyncSeq, bool asbTransition) {
    return ThisStdCall<bool>(0xA34F20, this, acPriority, abStartOver, afWeight, afEaseInTime, apTimeSyncSeq, asbTransition);
}

// 0xA351D0
bool NiControllerSequence::StartMorph(NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority, float afSourceWeight, float afDestWeight) {
	return ThisStdCall<bool>(0xA351D0, this, apDestSequence, afDuration, aiPriority, afSourceWeight, afDestWeight);
}

// 0xA350D0
bool NiControllerSequence::StartBlend(NiControllerSequence* apDestSequence, float afDuration, float afDestFrame, SInt32 aiPriority, float afSourceWeight, float afDestWeight, NiControllerSequence* apTimeSyncSeq) {
	return ThisStdCall<bool>(0xA350D0, this, apDestSequence, afDuration, afDestFrame, aiPriority, afSourceWeight, afDestWeight, apTimeSyncSeq);
}

void NiControllerSequence::GetInterpInfoAt(UInt32 auiIndex, const char*& pcAVObjectName, const char*& pcPropertyType, const char*& pcCtlrType, const char*& pcCtlrID, const char*& pcInterpolatorID) {
    IDTag& kTag = m_pkIDTagArray[auiIndex];
    pcAVObjectName = kTag.m_kAVObjectName.m_kHandle;
    pcPropertyType = kTag.m_kPropertyType.m_kHandle;
    pcCtlrType = kTag.m_kCtlrType.m_kHandle;
    pcCtlrID = kTag.m_kCtlrID.m_kHandle;
    pcInterpolatorID = kTag.m_kInterpolatorID.m_kHandle;
}

NiAVObject* NiControllerSequence::GetAccumRoot() const {
    return m_pkAccumRoot;
}

// 0xA32BC0
UInt32 NiControllerSequence::AddInterpolator(NiInterpolator* apInterpolator, IDTag& arTag, UInt8 aucPriority) {
    return ThisStdCall<UInt32>(0xA32BC0, this, apInterpolator, &arTag, aucPriority);
}

NiInterpolator* NiControllerSequence::GetInterpolatorAt(UInt32 auiIndex) const {
    return m_pkInterpArray[auiIndex].m_spInterpolator;
}

void NiControllerSequence::SetControllerAt(NiInterpController* apInterpCtlr, UInt32 auiIndex) {
	m_pkInterpArray[auiIndex].m_spInterpCtlr = apInterpCtlr;
}

void NiControllerSequence::ReplaceInterpolatorAt(NiInterpolator* apInterp, UInt32 auiIndex) {
    m_pkInterpArray[auiIndex].m_spInterpolator = apInterp;
}

NiInterpController* NiControllerSequence::GetControllerAt(UInt32 auiIndex) const {
    return m_pkInterpArray[auiIndex].m_spInterpCtlr;
}

NiControllerManager* NiControllerSequence::GetOwner() const {
    return m_pkOwner;
}

bool NiControllerSequence::SetOwner(NiControllerManager* pkOwner) {
	return m_pkOwner = pkOwner;
}

void NiControllerSequence::SetName(const char* apName) {
    m_kName = apName;
}

const char* NiControllerSequence::GetName() const {
    return m_kName.m_kHandle;
}