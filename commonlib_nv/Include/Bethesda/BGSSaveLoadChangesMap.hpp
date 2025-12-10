#pragma once

#include "NiTMap.hpp"
#include "BGSFormChanges.hpp"

class BGSSaveLoadChangesMap {
public:
	BGSSaveLoadChangesMap() {};
	~BGSSaveLoadChangesMap() {};

	NiTPointerMap<UInt32, BGSFormChanges*> kChangeMap;
};