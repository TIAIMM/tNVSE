#pragma once

#include "NiObject.hpp"
#include "NiFixedString.hpp"

NiSmartPointer(NiExtraData);

class NiExtraData : public NiObject {
public:
	NiExtraData();
	virtual ~NiExtraData();

	NiFixedString m_kName;

	const char* GetName() const { return m_kName.m_kHandle; }
	
	void SetName(const NiFixedString& arName);
	void LoadBinaryEx(NiStream* kStream);
};

ASSERT_SIZE(NiExtraData, 0xC);