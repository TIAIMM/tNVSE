#pragma once

#include "TESForm.hpp"
#include "TESDescription.hpp"
#include "TESIcon.hpp"
#include "TESCondition.hpp"

class BGSPerkEntry;

class BGSPerk : public TESForm, public TESFullName, public TESDescription, public TESIcon {
public:
	static inline auto bs_rtti = RTTI_BGSPerk;

	BGSPerk();
	~BGSPerk();

	struct PerkData {
		bool	bTrait;
		UInt8	ucLevel;
		UInt8	ucNumRanks;
		bool	bPlayable;
		bool	bHidden;
	};

	PerkData					kData;
	TESCondition				kConditions;
	BSSimpleList<BGSPerkEntry*>	kEntries;
};

ASSERT_SIZE(BGSPerk, 0x50);