#pragma once

#include "hkReferencedObject.hpp"

class hkpContactMgr;

class hkpCollisionAgent : public hkReferencedObject {
public:
	hkpContactMgr* m_contactMgr;
};

ASSERT_SIZE(hkpCollisionAgent, 0xC);