#pragma once

#include "NiTMap.hpp"

class BGSSaveLoadFormIDMap {
public:
	NiTMap<UInt32, UInt32>	kFormIDToIndexMap;
	NiTMap<UInt32, UInt32>	kIndexToFormIDMap;
	UInt32					uiCurrentIndex;

	UInt32 ConvertFormID(UInt32 auiFormID) const;
	UInt32 AddFormID(UInt32 auiFormID);
	UInt32 GetFormID(UInt32 auiIndex);
};

ASSERT_SIZE(BGSSaveLoadFormIDMap, 0x24);