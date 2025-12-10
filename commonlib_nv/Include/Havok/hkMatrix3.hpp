#pragma once

#include "hkVector4.hpp"

class hkMatrix3 {
public:
	hkVector4 m_col0;
	hkVector4 m_col1;
	hkVector4 m_col2;

	hkVector4& getColumn(UInt32 auiIndex);
	const hkVector4& getColumn(UInt32 auiIndex) const;

	void setColumn(UInt32 auiIndex, const hkVector4& akColumn);
};

ASSERT_SIZE(hkMatrix3, 0x30);