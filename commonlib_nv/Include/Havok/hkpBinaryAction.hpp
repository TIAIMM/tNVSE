#pragma once

#include "hkpAction.hpp"

class hkpBinaryAction : public hkpAction {
public:
	hkpEntity* m_entityA;
	hkpEntity* m_entityB;
};

ASSERT_SIZE(hkpBinaryAction, 0x20);