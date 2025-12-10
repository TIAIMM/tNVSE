#pragma once

#include "hkReferencedObject.hpp"
#include "hkVector4.hpp"
#include "hkArray.hpp"

class hkpMoppCode : public hkReferencedObject {
public:
	struct CodeInfo {
		hkVector4 m_offset;
	};

	CodeInfo		m_info;
	hkArray<UInt8>	m_data;
	UInt8			m_buildType;
};

ASSERT_SIZE(hkpMoppCode, 0x30);