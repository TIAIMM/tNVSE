#pragma once

#include "MagicItemForm.hpp"

class SpellItem : public MagicItemForm {
public:
	SpellItem();
	~SpellItem();

	virtual void	Endian();

	struct Data {
		MagicSystem::SpellType	eType;
		UInt32					uiCost;
		UInt32					uiLevel;
		UInt8					ucSpellFlags;
	};

	Data kData;
};

ASSERT_SIZE(SpellItem, 0x44 + 4);