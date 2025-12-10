#pragma once

class Modifier {
public:
	Modifier(UInt8 aucActorValue, float afValue) : ucActorValue(aucActorValue), fValue(afValue) {}
	~Modifier() {}

	UInt8 ucActorValue;
	float fValue;
};

ASSERT_SIZE(Modifier, 0x8);