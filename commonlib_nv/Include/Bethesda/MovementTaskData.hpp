#pragma once

#include "MobileObjectTaskletData.hpp"

class TESObjectREFR;

// Unused
class MovementTaskData : public MobileObjectTaskletData {
public:
	UInt32 uiRunCount;

	void ExecuteTaskEx(TESObjectREFR* apRefr);
};

ASSERT_SIZE(MovementTaskData, 0x3C);