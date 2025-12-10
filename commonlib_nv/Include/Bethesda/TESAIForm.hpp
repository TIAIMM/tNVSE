#pragma once

#include "TESForm.hpp"
#include "PackageList.hpp"

class TESPackage;

class TESAIForm : public BaseFormComponent {
public:
	TESAIForm();
	~TESAIForm();

	virtual UInt32	GetSaveSize(UInt32 auiChangeFlags);
	virtual void	SaveGame(UInt32 auiChangeFlags);
	virtual void	LoadGame(UInt32 auiChangeFlags);

	UInt8		ucAgression;
	UInt8		ucConfidence;			
	UInt8		ucEnergyLevel;		
	UInt8		ucResponsibility;		
	UInt8		ucMood;				
	UInt32		uiBuySellsAndServices;
	bool		bTeaches;			
	UInt8		ucMaximumTrainingLevel;
	UInt8		ucAssistance;			
	bool		bAggroRadiusBehavior;
	SInt32		iAggroRadius;		
	PackageList	kPackages;

	UInt32 GetPackageCount() const {
		return kPackages.ItemsInList();
	}

	TESPackage* GetPackage(SInt32 anIndex) const {
		auto entry = kPackages.GetAt(anIndex);
		return entry ? entry->GetItem() : nullptr;
	}
};

ASSERT_SIZE(TESAIForm, 0x20);