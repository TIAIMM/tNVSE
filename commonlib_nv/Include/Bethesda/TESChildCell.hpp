#pragma once
#include "TESObjectCELL.hpp"

class TESChildCell 
{
public:
	static inline auto bs_rtti = RTTI_TESChildCell;

	virtual TESObjectCELL* GetPersistentCell();
};
static_assert(sizeof(TESChildCell) == 0x4);