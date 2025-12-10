#pragma once

#include "MobileObjectTaskletData.hpp"

class Actor;

// Unused
class ActorUpdateTaskData : public MobileObjectTaskletData {
public:
	UInt32 uiRunCount;

	void ExecuteTaskEx(Actor* apActor);
};

ASSERT_SIZE(ActorUpdateTaskData, 0x3C);