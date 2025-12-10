#pragma once

#include "BGSNumericIDIndex.hpp"

#pragma pack(push, 1)
struct BGSSaveLoadFormHeader {
  BGSNumericIDIndex kFormIDIndex;
  UInt32			uiChangeFlags;
  UInt8				ucFormInfo;
  UInt8				ucVersion;
};
#pragma pack(pop)

ASSERT_SIZE(BGSSaveLoadFormHeader, 0x9);