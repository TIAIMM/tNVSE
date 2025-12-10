#pragma once

#include "ContainerItemExtra.hpp"

class ContainerObject {
public:
	SInt32				iCount;
	TESForm*			pForm;
	ContainerItemExtra* pItemExtra;
};

ASSERT_SIZE(ContainerObject, 0xC);