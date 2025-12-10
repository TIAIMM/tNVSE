#pragma once

#include "hkpAction.hpp"

class hkpUnaryAction : public hkpAction {
public:
	hkpEntity* m_entity;
};

ASSERT_SIZE(hkpUnaryAction, 0x1C);