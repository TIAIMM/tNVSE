#pragma once

#include "BSExtraData.hpp"
#include "BSSimpleList.hpp"
#include "BSPointer.hpp"

class TESObjectREFR;
class TESObjectCELL;
class ExtraRefractionProperty;
class ExtraSayTopicInfoOnceADay;
class Script;
class ScriptLocals;
class InventoryChanges;

class ExtraDataList : public BaseExtraList {
public:
	static ExtraDataList* Create(BSExtraData* apData = nullptr);

	BSSimpleList<BSPointer<TESObjectREFR>*>* GetReflectedRefs() const;
	ExtraRefractionProperty* GetRefractionProperty() const;
	ExtraSayTopicInfoOnceADay* GetExtraSayTopicInfoOnceADayExtra() const;

	InventoryChanges* GetContainerChanges() const;

	Script* GetScript() const;
	void SetScript(Script* apScript);

	void SetScriptLocals(ScriptLocals* apLocals);

	void SetPersistentCell(TESObjectCELL* apCell);
	void SetCount(UInt16 ausCount);
};

ASSERT_SIZE(ExtraDataList, 0x20);