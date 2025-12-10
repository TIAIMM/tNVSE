#pragma once

enum COL_LAYER {
	LAYER_STATIC					= 1,
	LAYER_ANIM_STATIC				= 2,
	LAYER_TRANSPARENT				= 3,
	LAYER_CLUTTER					= 4,
	LAYER_WEAPON					= 5,
	LAYER_PROJECTILE				= 6,
	LAYER_SPELL						= 7,
	LAYER_BIPED						= 8,
	LAYER_TREES						= 9,
	LAYER_PROPS						= 10,
	LAYER_WATER						= 11,
	LAYER_TRIGGER					= 12,
	LAYER_TERRAIN					= 13,
	LAYER_TRAP						= 14,
	LAYER_NONCOLLIDABLE				= 15,
	LAYER_CLOUD_TRAP				= 16,
	LAYER_GROUND					= 17,
	LAYER_PORTAL					= 18,
	LAYER_DEBRIS_SMALL				= 19,
	LAYER_DEBRIS_LARGE				= 20,
	LAYER_ACOUSTIC_SPACE			= 21,
	LAYER_ACTORZONE					= 22,
	LAYER_PROJECTILEZONE			= 23,
	LAYER_GASTRAP					= 24,
	LAYER_SHELLCASING				= 25,
	LAYER_TRANSPARENT_SMALL			= 26,
	LAYER_INVISIBLE_WALL			= 27,
	LAYER_TRANSPARENT_SMALL_ANIM	= 28,
	LAYER_DEADBIP					= 29,
	LAYER_CHARCONTROLLER			= 30,
	LAYER_AVOIDBOX					= 31,
	LAYER_COLLISIONBOX				= 32,
	LAYER_CAMERASPHERE				= 33,
	LAYER_DOORDETECTION				= 34,
	LAYER_CAMERAPICK				= 35,
	LAYER_ITEMPICK					= 36,
	LAYER_LINEOFSIGHT				= 37,
	LAYER_PATHPICK					= 38,
	LAYER_CUSTOMPICK1				= 39,
	LAYER_CUSTOMPICK2				= 40,
	LAYER_SPELLEXPLOSION			= 41,
	LAYER_DROPPINGPICK				= 42,
	LAYER_MAX						= 43,
};

enum COL_LAYER_BIT : UInt64 {
	LAYER_B_STATIC					= 1ull << 1,
	LAYER_B_ANIM_STATIC				= 1ull << 2,
	LAYER_B_TRANSPARENT				= 1ull << 3,
	LAYER_B_CLUTTER					= 1ull << 4,
	LAYER_B_WEAPON					= 1ull << 5,
	LAYER_B_PROJECTILE				= 1ull << 6,
	LAYER_B_SPELL					= 1ull << 7,
	LAYER_B_BIPED					= 1ull << 8,
	LAYER_B_TREES					= 1ull << 9,
	LAYER_B_PROPS					= 1ull << 10,
	LAYER_B_WATER					= 1ull << 11,
	LAYER_B_TRIGGER					= 1ull << 12,
	LAYER_B_TERRAIN					= 1ull << 13,
	LAYER_B_TRAP					= 1ull << 14,
	LAYER_B_NONCOLLIDABLE			= 1ull << 15,
	LAYER_B_CLOUD_TRAP				= 1ull << 16,
	LAYER_B_GROUND					= 1ull << 17,
	LAYER_B_PORTAL					= 1ull << 18,
	LAYER_B_DEBRIS_SMALL			= 1ull << 19,
	LAYER_B_DEBRIS_LARGE			= 1ull << 20,
	LAYER_B_ACOUSTIC_SPACE			= 1ull << 21,
	LAYER_B_ACTORZONE				= 1ull << 22,
	LAYER_B_PROJECTILEZONE			= 1ull << 23,
	LAYER_B_GASTRAP					= 1ull << 24,
	LAYER_B_SHELLCASING				= 1ull << 25,
	LAYER_B_TRANSPARENT_SMALL		= 1ull << 26,
	LAYER_B_INVISIBLE_WALL			= 1ull << 27,
	LAYER_B_TRANSPARENT_SMALL_ANIM	= 1ull << 28,
	LAYER_B_DEADBIP					= 1ull << 29,
	LAYER_B_CHARCONTROLLER			= 1ull << 30,
	LAYER_B_AVOIDBOX				= 1ull << 31,
	LAYER_B_COLLISIONBOX			= 1ull << 32,
	LAYER_B_CAMERASPHERE			= 1ull << 33,
	LAYER_B_DOORDETECTION			= 1ull << 34,
	LAYER_B_CAMERAPICK				= 1ull << 35,
	LAYER_B_ITEMPICK				= 1ull << 36,
	LAYER_B_LINEOFSIGHT				= 1ull << 37,
	LAYER_B_PATHPICK				= 1ull << 38,
	LAYER_B_CUSTOMPICK1				= 1ull << 39,
	LAYER_B_CUSTOMPICK2				= 1ull << 40,
	LAYER_B_SPELLEXPLOSION			= 1ull << 41,
	LAYER_B_DROPPINGPICK			= 1ull << 42,
};

enum BIPED_PART : UInt32 {
	P_OTHER			= 0,
	P_HEAD			= 1,
	P_BODY			= 2,
	P_SPINE1		= 3,
	P_SPINE2		= 4,
	P_LUPPERARM		= 5,
	P_LFOREARM		= 6,
	P_LHAND			= 7,
	P_LTHIGH		= 8,
	P_LCALF			= 9,
	P_LFOOT			= 10,
	P_RUPPERARM		= 11,
	P_RFOREARM		= 12,
	P_RHAND			= 13,
	P_RTHIGH		= 14,
	P_RCALF			= 15,
	P_RFOOT			= 16,
	P_TAIL			= 17,
	P_SHIELD		= 18,
	P_QUIVER		= 19,
	P_WEAPON		= 20,
	P_PONYTAIL		= 21,
	P_WING			= 22,
	P_PACK			= 23,
	P_CHAIN			= 24,
	P_ADDONHEAD		= 25,
	P_ADDONCHEST	= 26,
	P_ADDONLEG		= 27,
	P_ADDONARM		= 28,
	NUMFILTERPARTS	= 32,
};


enum COL_GROUP : UInt32 {
	GROUP_LANDSCAPE = 1,
	GROUP_PATH_PICK = 3,
	GROUP_PLAYER	= 9,
};

class CFilter {
public:
	enum : UInt32 {
		GROUP_MASK		= 0xFFFF0000,
		MOPP_SCALED		= 0x2000,
		DISABLED		= 0x4000,
		LINKED_GROUP	= 0x8000,
	};

	CFilter() : uiFilter(0) {};
	CFilter(const UInt32 auiFilter) : uiFilter(auiFilter) {}
	// 0x622290
	CFilter(COL_LAYER aeLayer, UInt32 aiGroup, BIPED_PART aePart) {
		Init(aeLayer, aiGroup, aePart);
	}
	~CFilter() {};

	UInt32 uiFilter;

	// 0x6222C0
	void Init(COL_LAYER aeLayer, UInt32 aiGroup, BIPED_PART aePart) {
		uiFilter = aeLayer | (aiGroup << 16) | (aePart << 8);
	}

	// 0x4A39F0
	void SetLayer(COL_LAYER auiLayer) {
		uiFilter &= ~0x7Fu;
		uiFilter |= auiLayer & 0x7F;
	}

	// 0x43B4D0
	COL_LAYER GetLayer() const {
		return COL_LAYER(uiFilter & 0x7F);
	}

	// 0x59CE80
	void SetGroup(UInt32 auiGroup) {
		uiFilter = uiFilter & 0xFFFF;
		uiFilter |= auiGroup << 16;
	}

	// 0x4A3A20
	UInt32 GetGroup() const {
		return HIWORD(uiFilter);
	}

	// 0x4D9DA0
	UInt32 GetPart() const {
		return (uiFilter >> 8) & 0x1F;
	}

	void SetPart(BIPED_PART aePart) {
		uiFilter &= ~0x1F00;
		uiFilter |= (aePart << 8) & 0x1F00;
	}

	// 0x6240B0
	UInt32 GetBipedPart() const {
		return GetPart();
	}

	bool IsMoppScaled() const {
		return (uiFilter & MOPP_SCALED) != 0;
	}

	void SetMopScaled(bool abScaled) {
		if (abScaled)
			uiFilter |= MOPP_SCALED;
		else
			uiFilter &= ~MOPP_SCALED;
	}

	bool IsDisabled() const {
		return (uiFilter & DISABLED) != 0;
	}

	// 0x810F90
	void SetDisabled(bool abDisabled) {
		if (abDisabled)
			uiFilter |= DISABLED;
		else
			uiFilter &= ~DISABLED;
	}

	bool IsLinkedGroup() const {
		return (uiFilter & LINKED_GROUP) != 0;
	}

	void SetLinkedGroup(bool abLinked) {
		if (abLinked)
			uiFilter |= LINKED_GROUP;
		else
			uiFilter &= ~LINKED_GROUP;
	}

	bool IsBipLayer() const {
		const COL_LAYER eLayer = GetLayer();
		return eLayer == LAYER_BIPED || eLayer == LAYER_DEADBIP;
	}

	bool IsLinked(CFilter aInfo) const {
		return (uiFilter & aInfo.uiFilter & LINKED_GROUP) != 0;
	}

	bool IsDifferentGroup(CFilter aInfo) const {
		return (uiFilter ^ aInfo.uiFilter) & GROUP_MASK;
	}
};

ASSERT_SIZE(CFilter, 0x4);