#pragma once

#include "MobileObjectTaskletData.hpp"

class Actor;

// Unused
class AnimationTaskData : public MobileObjectTaskletData {
public:

	void ExecuteTaskEx(Actor* apActor);
};

ASSERT_SIZE(AnimationTaskData, 0x38);