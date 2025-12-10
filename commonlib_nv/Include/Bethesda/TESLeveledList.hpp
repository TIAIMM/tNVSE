#pragma once

#include "BaseFormComponent.hpp"
#include "BSExtraData.hpp"
#include "BSSimpleList.hpp"

class TESObjectREFR;
class ContainerItemExtra;
class TESContainer;
class TESGlobal;

struct LEVELED_OBJECT_FILE {
	UInt16	usLevel;
	UInt32	uiFormID;
	UInt16	usCount;
};

struct LEVELED_OBJECT {
	TESForm*			pForm;
	SInt16				usCount;
	SInt16				usLevel;
	ContainerItemExtra* pItemExtra;
};

class TESLeveledList : public BaseFormComponent {
public:
	virtual UInt8	GetChanceNone() const;
	virtual bool	GetMultCalc() const;
	virtual UInt32	GetMaxLevelDifference() const;

	enum Flags {
		CALC_ALL_BELOW	= 1 << 0,
		MULT_CALC		= 1 << 1,
		USE_ALL			= 1 << 2,
	};

	BSSimpleList<LEVELED_OBJECT*>	kLeveledObjects;
	UInt8							ucChanceNone;
	Bitfield8						ucFlags;
	TESGlobal*						pChanceGlobal;
	BSSimpleList<LEVELED_OBJECT*>	kScriptAddedObjects;

	static TESLeveledList* GetFormAsLeveledList(TESForm* apForm);

	bool GetCalcAllBelow() const;
	bool GetUseAll() const;

	void CalculateCurrentFormList(UInt16 ausLevel, UInt16 ausCount, TESContainer* apOut, UInt32 aeAllBelowForce);
	void CalculateCurrentForm(UInt16 ausLevel, TESForm*& apOutForm, UInt16& asOutCount, ContainerItemExtra*& apOutExtra, bool abRecurse, UInt32 aeAllBelowForce);
};
ASSERT_SIZE(TESLeveledList, 0x1C);