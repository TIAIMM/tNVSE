#include "AITimer.hpp"

float AITimer::GetGlobalTimer() {
    return *(float*)0x11F1BF0;
}

// 0x8D7F40
void AITimer::Start(float afTargetTime) {
	fStartTime = GetGlobalTimer();
	fTargetTime = afTargetTime;
}

float AITimer::GetElapsedTime() const {
	return GetGlobalTimer() - fStartTime;
}

// 0x8D7F80
bool AITimer::IsFinished() const {
	return fTargetTime < GetElapsedTime();
}
