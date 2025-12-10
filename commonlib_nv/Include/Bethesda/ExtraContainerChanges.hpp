#pragma once

#include <vector>

#include "BSExtraData.hpp"
#include "ExtraDataList.hpp"
#include "TESForm.hpp"
#include "InventoryChanges.hpp"
#include "map"

class TESObjectREFR;

class ExtraContainerChanges : public BSExtraData {
public:
	ExtraContainerChanges();
	virtual ~ExtraContainerChanges();

	EXTRADATATYPE(CONTAINERCHANGES);

	typedef std::vector<ItemChange*> DataArray;
	typedef std::vector<BSSimpleList<ItemChange*>*> ExtendDataArray;

	InventoryChanges* data;	// 00C

	// find the equipped item whose form matches the passed matcher
	struct FoundEquipData {
		TESForm* pForm;
		ExtraDataList* pExtraData;
	};

	BSSimpleList<ItemChange*>* GetEntryDataList() const {
		return data ? data->pItems : NULL;
	}
};

typedef ExtraContainerChanges::DataArray ExtraContainerDataArray;
typedef ExtraContainerChanges::ExtendDataArray ExtraContainerExtendDataArray;
typedef ExtraContainerChanges::FoundEquipData EquipData;
typedef std::map<TESForm*, InventoryItemData> InventoryItemsMap;