#pragma once

#include "MobileObjectTaskletData.hpp"

class Actor;

// Unused
class ActorsScriptTaskData : public MobileObjectTaskletData {
public:
	UInt32 unk38;
	UInt32 uiRunCount;

	void ExecuteTaskEx(Actor* apActor);
};

ASSERT_SIZE(ActorsScriptTaskData, 0x40);