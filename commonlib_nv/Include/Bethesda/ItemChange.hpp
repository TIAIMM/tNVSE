#pragma once

#include "ExtraDataList.hpp"

class Actor;
class TESForm;

class ItemChange : public BSMemObject {
public:
	ItemChange(TESForm* apForm, SInt32 aiDelta = 0);
	~ItemChange();

	BSSimpleList<ExtraDataList*>*	pExtraLists;
	SInt32							iCountDelta;
	TESForm*						pObject;


	void							Cleanup();
	static ItemChange*				Create(TESForm* pForm, UInt32 count = 1, BSSimpleList<ExtraDataList*>* pExtendDataList = nullptr);
	BSSimpleList<ExtraDataList*>*	Add(ExtraDataList* newList);
	bool							Remove(ExtraDataList* toRemove, bool bFree = false);
	bool							HasExtraLeveledItem() const;
	void							RemoveCannotWear();
	ExtraDataList*					GetEquippedExtra();
	ExtraDataList*					GetCustomExtra(UInt32 whichVal);
	BSExtraData*					GetExtraData(UInt32 whichVal);
	float							CalculateWeaponDamage(float condition, TESForm* ammo);
	float							GetValue();
	bool							HasWeaponMod(UInt32 modEffect) { return ThisStdCall<bool>(0x4BDA70, this, modEffect); }
	UInt32							GetWeaponNumProjectiles(Actor* owner);
	bool							ShouldDisplay();

	UInt8							GetWeaponMod();
	__forceinline float				GetHealthPercent(char a1 = 0) { return ThisStdCall<float>(0x4BCDB0, this, a1); };
	float							GetHealthPercentAlt(bool axonisFix = false, bool checkDestruction = true);
	bool							GetEquipped();
	ExtraDataList*					GetExtraData() const;
	UInt32							GetClipSize();
	void							Equip(Actor* actor, ExtraDataList* extra = nullptr);

	enum {
		kHotkeyMin = 0,
		kHotkey0 = kHotkeyMin,
		kHotkey1,
		kHotkey2,
		kHotkey3,
		kHotkey4,
		kHotkey5,
		kHotkey6,
		kHotkey7,
		kHotkeyMax = kHotkey7,
		kHotkey8,
		kHotkeyStewie = kHotkey8
	};
};
ASSERT_SIZE(ItemChange, 0xC);