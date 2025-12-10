#pragma once

#include "TESDescription.hpp"
#include "TESForm.hpp"
#include "TESIcon.hpp"
#include "ActorValue.hpp"

class ActorValueOwner;

class ActorValueInfo : public TESForm, public TESFullName, public TESDescription, public TESIcon {
public:
	const char*			pcInfoName;		// 38
	BSString			strAbbreviation;			// 3C
	UInt32				uiAVFlags;		// 44
	//		bit 0x01	used in list of modified ActorValue for Player and others. Either can damage or "special damage", see 0x00937280
	//		bit 0x03
	//		bit 0x04
	//		bit 0x07
	//		bit 0x08
	//		bit 0x0B
	//		bit 0x0C
	//		bit 0x0E	canModify
	UInt32				unk48;			// 48
	UInt32				initDefaultCallback;		// 4C
	UInt32				unk50;			// 50
	void(__cdecl*		pModifiedCallback)(ActorValueOwner*, int, float, float, ActorValueOwner*);
	UInt32				uiNumDependentActorValues;
	ActorValue::Index	kDependentActorValues[15];
	UInt32				uiEnumCount;
	const char*			pcEnumNames[10];
};

ASSERT_SIZE(ActorValueInfo, 0xC4);

extern const ActorValueInfo** ActorValueInfoPointerArray;

typedef ActorValueInfo* (*_GetActorValueInfo)(UInt32 actorValueCode);
extern const _GetActorValueInfo GetActorValueInfo;