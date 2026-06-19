#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"

NiFixedString::NiFixedString() {
	m_kHandle = nullptr;
}

// GAME - 0x438170
NiFixedString::NiFixedString(const char* pcString) {
	if (pcString)
		m_kHandle = NiGlobalStringTable::AddString(pcString);
	else
		m_kHandle = nullptr;
}

NiFixedString::NiFixedString(const NiFixedString& kString) {
	m_kHandle = kString.m_kHandle;
}

// GAME - 0x4381B0
NiFixedString::~NiFixedString() {
	NiGlobalStringTable::DecRefCount(m_kHandle);
}

// GAME - 0xA2E750
NiFixedString& NiFixedString::operator=(const char* pcString) {
	if (m_kHandle != pcString) {
		NiGlobalStringTable::GlobalStringHandle kHandle = m_kHandle;
		m_kHandle = NiGlobalStringTable::AddString(pcString);
		NiGlobalStringTable::DecRefCount(kHandle);
	}
	return *this;
}

NiFixedString& NiFixedString::operator=(const NiFixedString& arString) {
	if (m_kHandle != arString.m_kHandle) {
		NiGlobalStringTable::GlobalStringHandle kHandle = arString.m_kHandle;
		NiGlobalStringTable::IncRefCount(kHandle);
		NiGlobalStringTable::DecRefCount(m_kHandle);
		m_kHandle = kHandle;
	}
	return *this;
}

bool NiFixedString::operator==(const NiFixedString& akString) const {
	return !strcmp(m_kHandle, akString.m_kHandle);
}

bool NiFixedString::operator==(const char* pcString) const {
	return !strcmp(m_kHandle, pcString);
}

NiFixedString::operator const char* () const {
	return m_kHandle;
}

NiFixedString::operator bool() const {
	return m_kHandle != nullptr;
}

const char* NiFixedString::c_str() const {
	return m_kHandle;
}

UInt32 NiFixedString::GetLength() const {
	return NiGlobalStringTable::GetLength(m_kHandle);
}

bool NiFixedString::Includes(const char* apToFind) const {
	if (!m_kHandle || !apToFind)
		return false;

	return strstr(m_kHandle, apToFind) != nullptr;
}
