#pragma once

#include "BSExtraData.hpp"

class ExtraWeaponModFlags : public BSExtraData {
public:
	EXTRADATATYPE(WEAPONMODFLAGS);

	ExtraWeaponModFlags();
	~ExtraWeaponModFlags();

	UInt8	flags; // 00C

	static ExtraWeaponModFlags* Create() {
		const auto mem = BSNew<ExtraWeaponModFlags>(0x10);
		ThisStdCall(0x42CC40, mem);
		return mem;
	}
};