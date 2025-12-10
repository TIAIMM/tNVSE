#pragma once

#include "NiMemObject.hpp"
#include "NiTArray.hpp"
#include "NiCriticalSection.hpp"

class NiFixedString;

class NiGlobalStringTable : public NiMemObject {
public:
	typedef char* GlobalStringHandle;

	NiTPrimitiveArray<GlobalStringHandle> m_kHashArray[512];
	void* unk2000[32];
	NiCriticalSection m_kCriticalSection;
	void* unk20A0[24];

	static GlobalStringHandle AddString(const char* pcString);
	static void IncRefCount(const GlobalStringHandle& arHandle);
	static void DecRefCount(const GlobalStringHandle& arHandle);
	static UInt32 GetLength(const GlobalStringHandle& arHandle);

	static char* GetRealBufferStart(const GlobalStringHandle& arHandle);

	inline GlobalStringHandle* FindString(const char* str, uint32_t length) {
		return ThisStdCall<GlobalStringHandle*>(0xA5B300, this, str, length);
	}
};

ASSERT_SIZE(NiGlobalStringTable, 0x2100);
