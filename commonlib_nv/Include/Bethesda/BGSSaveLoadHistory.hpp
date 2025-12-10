#pragma once

#include "BSSimpleArray.hpp"

class BGSSaveLoadHistory {
public:
	BSSimpleArray<char*> kNotes;
};

ASSERT_SIZE(BGSSaveLoadHistory, 0x10);