#pragma once

class NiCriticalSection {
public:
	NiCriticalSection() {
		InitializeCriticalSection(&m_kCriticalSection);
		m_ulThreadOwner = 0;
		m_uiLockCount = 0;
	}

	~NiCriticalSection() {
		DeleteCriticalSection(&m_kCriticalSection);
	}

	CRITICAL_SECTION	m_kCriticalSection;
	UInt32				m_ulThreadOwner;
	UInt32				m_uiLockCount;

	void Lock() {
		EnterCriticalSection(&m_kCriticalSection); 
	}

	void Unlock() {
		LeaveCriticalSection(&m_kCriticalSection);
	}
};

ASSERT_SIZE(NiCriticalSection, 0x20);

struct NiCriticalSectionScope {
	NiCriticalSectionScope(NiCriticalSection& aCriticalSection) : rCriticalSection(aCriticalSection) {
		rCriticalSection.Lock();
	}
	~NiCriticalSectionScope() {
		rCriticalSection.Unlock();
	}

	NiCriticalSection& rCriticalSection;
};