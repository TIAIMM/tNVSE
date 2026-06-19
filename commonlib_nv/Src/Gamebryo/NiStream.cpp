#include "NiStream.hpp"

// 0xA64900
void NiStream::RegisterLoader(const char* pcName, void* pfnFunc) {
	CdeclCall(0xA64900, pcName, pfnFunc);
}

// 0xA66150
NiStream* NiStream::Create(NiStream* apThis) {
	return ThisStdCall<NiStream*>(0xA66150, apThis);
}

// 0x43B200
NiObject* NiStream::GetObjectAt(UInt32 uiIndex) {
	return m_kTopObjects.GetAt(uiIndex);
}

// 0x43B1E0
UInt32 NiStream::GetObjectCount() {
	return m_kTopObjects.GetSize();
}

// 0xA66370
void NiStream::InsertObject(NiObject* apObject) {
	m_kTopObjects.Add(apObject);
}

// 0xA645B0
void NiStream::ReadLinkID() {
	ThisStdCall(0xA645B0, this);
}

// 0xA63430
NiObject* NiStream::GetObjectFromLinkID() {
	return ThisStdCall<NiObject*>(0xA63430, this);
}

// 0xA641F0
void NiStream::SaveFixedString(const NiFixedString& arStr) {
	ThisStdCall(0xA641F0, this, &arStr);
}

// 0xA64DF0
void NiStream::LoadFixedString(NiFixedString& arStr) {
	ThisStdCall(0xA64DF0, this, &arStr);
}
