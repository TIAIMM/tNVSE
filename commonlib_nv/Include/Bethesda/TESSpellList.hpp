#pragma once

#include "BaseFormComponent.hpp"
#include "SpellItem.hpp"
#include "TESLevSpell.hpp"

class TESSpellList : public BaseFormComponent {
public:
	enum
	{
		kModified_BaseSpellList = 0x00000020,
		// CHANGE_ACTOR_BASE_SPELLLIST
		//	UInt16	numSpells;
		//	UInt32	spells[numSpells];
	};

	TESSpellList();
	~TESSpellList();

	virtual UInt32	GetSaveSize(UInt32 auiChangeFlags);
	virtual void	SaveGame(UInt32 auiChangeFlags);
	virtual void	LoadGame(UInt32 auiChangeFlags);

	BSSimpleList<SpellItem*>	kSpells;
	BSSimpleList<TESLevSpell*>	kLeveledSpells;

	UInt32	GetSpellCount() const {
		return kSpells.ItemsInList();
	}

	SpellItem* GetSpell(SInt32 whichSpell) const {
		auto entry = kSpells.GetAt(whichSpell);
		return entry ? entry->GetItem() : nullptr;
	}
};

ASSERT_SIZE(TESSpellList, 0x14);