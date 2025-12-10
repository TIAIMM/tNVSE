#pragma once

#include "hkReferencedObject.hpp"

class hkpContactMgr : public hkReferencedObject {
public:
	UInt32 m_type;
};

ASSERT_SIZE(hkpContactMgr, 0xC);