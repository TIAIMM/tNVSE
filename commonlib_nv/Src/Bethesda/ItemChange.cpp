#include "ItemChange.hpp"

// 0x4BC550
ItemChange::ItemChange(TESForm* apForm, SInt32 aiDelta) {
	pObject = apForm;
	pExtraLists = new BSSimpleList<ExtraDataList*>();
	iCountDelta = aiDelta;
}

// 0x4BC5F0
ItemChange::~ItemChange() {
	delete pExtraLists;
	pExtraLists = nullptr;
}

// 0x4BC780
void ItemChange::Cleanup() {
	
	for (BSSimpleList<ExtraDataList*>* pIter = pExtraLists; pIter && pIter->GetItem(); pIter = pIter->GetNext()) {
		ExtraDataList* pExtraList = pIter->GetItem();
		pExtraList->RemoveAll(true);
		pIter->Remove(pExtraList);

		delete pExtraList;
	}
}