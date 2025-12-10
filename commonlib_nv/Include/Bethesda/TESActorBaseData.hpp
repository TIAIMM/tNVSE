#pragma once

#include "BaseFormComponent.hpp"
#include "BSSimpleList.hpp"

class BGSVoiceType;
class TESFaction;
class TESLevItem;

class TESActorBaseData : public BaseFormComponent {
public:
	TESActorBaseData();
	~TESActorBaseData();

	virtual void			CopyFromTemplateForm(TESForm* apForm);
	virtual bool			GetNoVATSmelee() const;
	virtual bool			GetAllowPCDialogue() const; 
	virtual bool			GetAllowPickpocket() const; 
	virtual bool			GetIsGhost() const;
	virtual bool			GetInvulnerable() const;
	virtual bool			GetCantOpenDoors() const;
	virtual bool			GetCanBeAllRaces() const;
	virtual bool			GetIsImmobile() const;
	virtual bool			Fn_0D() const;
	virtual bool			GetNoLeftArm() const;
	virtual bool			GetNoRightArn() const;
	virtual bool			GetNoHead() const;
	virtual bool			GetNoShadow() const;
	virtual bool			GetNoBloodSpray() const;
	virtual void			SetNoBloodSpray(bool abVal);
	virtual bool			GetNoBloodDecal() const;
	virtual void			SetNoBloodDecal(bool abVal);
	virtual UInt32			GetMaterialType() const;
	virtual void			SetMaterialType(UInt32 aeType);
	virtual UInt32			GetFatigue() const;
	virtual float			GetKarma() const;
	virtual BGSVoiceType*	GetVoiceType() const;

	enum BaseFlags : UInt32 {
		FEMALE						= 1u << 0,
		CREATURE_BIPED				= FEMALE,
		ESSENTIAL					= 1u << 1,
		HAS_CHARGEN_FACE			= 1u << 2,
		CREATURE_WEAPON_AND_SHIELD	= HAS_CHARGEN_FACE,
		RESPAWN						= 1u << 3,
		AUTOCALC_STATS				= 1u << 4,
		CREATURE_SWIMS				= AUTOCALC_STATS,
		CREATURE_FLIES				= 1u << 5,
		CREATURE_WALKS				= 1u << 6,
		PC_LEVEL_MULT				= 1u << 7,
		USE_TEMPLATE				= 1u << 8,
		NO_LOWLEVEL_PROCESSING		= 1u << 9,
		//							  1u << 10,
		NO_BLOOD_SPRAY				= 1u << 11,
		NO_BLOOD_DECAL				= 1u << 12,
		//							  1u << 13,
		//							  1u << 14,
		CREATURE_NO_HEAD			= 1u << 15,
		CREATURE_NO_RIGHT_ARM		= 1u << 16,
		CREATURE_NO_LEFT_ARM		= 1u << 17,
		CREATURE_NO_COMBAT_IN_WATER	= 1u << 18,
		CREATURE_NO_SHADOW			= 1u << 19,
		NO_VATS_MELEE				= 1u << 20,
		CREATURE_ALLOW_PC_DIALOGUE	= 1u << 21,
		CAN_BE_ALL_RACES			= 1u << 22,
		CREATURE_CANT_OPEN_DOOR		= CAN_BE_ALL_RACES,
		CREATURE_IMMOBILE			= 1u << 23,
		CREATURE_TILT_FRON_TBACK	= 1u << 24,
		CREATURE_TILT_LEFT_RIGHT	= 1u << 25,
		NO_KNOCKDOWNS				= 1u << 26,
		NOT_PUSHABLE				= 1u << 27,
		CREATURE_ALLOW_PICKPOCKET	= 1u << 28,
		CREATURE_IS_GHOST			= 1u << 29,
		NO_ROTATE_TO_HEADTRACK		= 1u << 30,
		CREATURE_INVULNERABLE		= 1u << 31,
	};

	enum TemplateUseFlags : UInt16 {
		TRAITS					= 1u << 0,
		STATS					= 1u << 1,
		FACTIONS				= 1u << 2,
		ACTOREFFECBSSIMPLELIST	= 1u << 3,
		AIDATA					= 1u << 4,
		AIPACKAGES				= 1u << 5,
		MODELANIMATION			= 1u << 6,
		BASEDATA				= 1u << 7,
		INVENTORY				= 1u << 8,
		SCRIPT					= 1u << 9,
		COPIED					= 1u << 15,
	};

	struct FactionListData {
		TESFaction* pFaction;
		UInt8		ucRank;
	};

	Bitfield32						uiBaseFlags;
	UInt16							usFatigue;
	UInt16							usBarterGold;
	SInt16							sLevel;
	UInt16							usCalcMin;
	UInt16							usCalcMax;
	UInt16							usSpeedMultiplier;
	float							fKarma;
	UInt16							usDispositionBase;
	UInt16							usTemplateFlags;
	TESLevItem*						pDeathItem;
	BGSVoiceType*					pVoiceType;
	TESForm*						pTemplateActor;
	UInt32							uiChangedFlags;
	BSSimpleList<FactionListData*>	kFactionList;

	SInt8 GetFactionRank(TESFaction* faction);

	bool GetBaseFlag(BaseFlags aeFlag) const { return uiBaseFlags.GetBit(aeFlag); }
	void SetBaseFlag(BaseFlags aeFlag, bool abVal) { uiBaseFlags.SetBit(aeFlag, abVal); }

	bool IsFemale() const { return GetBaseFlag(FEMALE); }
};

ASSERT_SIZE(TESActorBaseData, 0x34);