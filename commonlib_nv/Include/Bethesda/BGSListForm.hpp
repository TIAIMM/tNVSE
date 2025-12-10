#pragma once

#include "TESForm.hpp"

class BGSListForm : public TESForm {
public:
	BGSListForm();
	~BGSListForm();

	BSSimpleList<TESForm*>	kList;
	UInt32					uiNumAddedObjects;

	UInt32 Count() const {
		return kList.ItemsInList();
	}

	TESForm* GetAt(UInt32 auID) const {
		auto pResult = kList.GetAt(auID);
		return pResult ? pResult->GetItem() : nullptr;
	}
};

ASSERT_SIZE(BGSListForm, 0x024);