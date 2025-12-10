#pragma once

#include "BSExtraData.hpp"

class ExtraHealth : public BSExtraData {
public:
	EXTRADATATYPE(HEALTH);

	ExtraHealth();
	virtual ~ExtraHealth();
	float health;

	static ExtraHealth* Create() {
		const auto mem = BSNew<ExtraHealth>(0x10);
		ThisStdCall(0x42C6A0, mem);
		return mem;
	}
};