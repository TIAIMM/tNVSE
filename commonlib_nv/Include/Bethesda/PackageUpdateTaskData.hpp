#pragma once

#include "MobileObjectTaskletData.hpp"

class TESObjectREFR;

// Unused
class PackageUpdateTaskData : public MobileObjectTaskletData {
public:
	UInt32 unk38;
	UInt32 unk3C;
	UInt32 unk40;

	void ExecuteTaskEx(TESObjectREFR* apRefr);
};

ASSERT_SIZE(PackageUpdateTaskData, 0x44);