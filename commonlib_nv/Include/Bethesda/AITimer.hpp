#pragma once

struct AITimer {
	float fStartTime	= 0.f;
	float fTargetTime	= 0.f;

	static float GetGlobalTimer();

	void Start(float afTargetTime);

	float GetElapsedTime() const;

	bool IsFinished() const;
};

ASSERT_SIZE(AITimer, 0x8);