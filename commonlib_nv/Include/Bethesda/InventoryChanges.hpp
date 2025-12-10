#pragma once

#include "BSSimpleList.hpp"
#include "ItemChange.hpp"

class ExtraDataList;
class TESObjectREFR;
class ItemChange;

class InventoryChanges {
public:
	InventoryChanges(TESObjectREFR* apOwner);
	~InventoryChanges();

	BSSimpleList<ItemChange*>*			pItems;
	TESObjectREFR*						pOwner;
	float								fTotalWgCurrent;
	float								fTotalWgLast;
	bool								bChanged;

	void								Cleanup();

	static TESObjectREFR** const pScriptRef;
};
ASSERT_SIZE(InventoryChanges, 0x14);

struct InventoryItemData {
	SInt32				iCount;
	ItemChange*			pItem;

	InventoryItemData(SInt32 count, ItemChange* entry) : iCount(count), pItem(entry) {}
};
ASSERT_SIZE(InventoryItemData, 0x8);