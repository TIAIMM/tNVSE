#pragma once

#include "NiMemObject.hpp"
#include "NiSmartPointer.hpp"
#include "NiGlobalStringTable.hpp"

class NiFixedString : public NiMemObject {
public:
	NiFixedString();
	NiFixedString(const char* pcString);
	NiFixedString(const NiFixedString& kString);
	~NiFixedString();

	NiGlobalStringTable::GlobalStringHandle m_kHandle;

	NiFixedString& operator=(const char* pcString);
	NiFixedString& operator=(const NiFixedString& arString);
	bool operator==(const NiFixedString& akString) const;
	bool operator==(const char* pcString) const;

	operator const char*() const;

	operator bool() const;

	const char* c_str() const;

	UInt32 GetLength() const;

	bool Includes(const char* apToFind) const;
};

ASSERT_SIZE(NiFixedString, 0x4)