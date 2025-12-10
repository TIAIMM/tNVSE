#pragma once

#include "BGSCellNumericIDArrayMap.hpp"
#include "NiTPointerMap.hpp"

class BGSSaveLoadReferencesMap {
public:
	NiTPointerMap<UInt32, UInt32>						kMovedReferencesMap;
	BGSCellNumericIDArrayMap							kInteriorReferencesMap;
	NiTPointerMap<UInt32, BGSCellNumericIDArrayMap*>	kWorldspaceReferencesMap;
};

ASSERT_SIZE(BGSSaveLoadReferencesMap, 0x30);