#pragma once

class MagicSystem {
public:
	enum SpellType : UInt32 {
		SPELL,
		DISEASE,
		POWER,
		LESSER_POWER,
		ABILITY,
		POISON,
		ENCHANTMENT,
		POTION,
		WORTCRAFT,
		LEVELLED,
		ADDICTION,
	};

	enum Range : UInt32 {
		SELF,
		TOUCH,
		TARGET,
	};

	enum School : UInt32 {
		ALTERATION,
		CONJURATION,
		DESTRUCTION,
		ILLUSION,
		MYSTICISM,
		RESTORATION,
	};
};