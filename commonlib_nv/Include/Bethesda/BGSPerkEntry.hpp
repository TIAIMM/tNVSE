#pragma once


enum PerkEntryPointID : __int32 {
	kPerkEntry_CalculateWeaponDamage = 0x0,
	kPerkEntry_CalculateMyCriticalHitChance = 0x1,
	kPerkEntry_CalculateMyCriticalHitDamage = 0x2,
	kPerkEntry_CalculateWeaponAttackAPCost = 0x3,
	kPerkEntry_CalculateMineExplodeChance = 0x4,
	kPerkEntry_AdjustRangePenalty = 0x5,
	kPerkEntry_AdjustLimbDamage = 0x6,
	kPerkEntry_CalculateWeaponRange = 0x7,
	kPerkEntry_CalculateToHitChance = 0x8,
	kPerkEntry_AdjustExperiencePoints = 0x9,
	kPerkEntry_AdjustGainedSkillPoints = 0xA,
	kPerkEntry_AdjustBookSkillPoints = 0xB,
	kPerkEntry_ModifyRecoveredHealth = 0xC,
	kPerkEntry_CalculateInventoryAPCost = 0xD,
	kPerkEntry_GetDisposition = 0xE,
	kPerkEntry_GetShouldAttack = 0xF,
	kPerkEntry_GetShouldAssist = 0x10,
	kPerkEntry_CalculateBuyPrice = 0x11,
	kPerkEntry_GetBadKarma = 0x12,
	kPerkEntry_GetGoodKarma = 0x13,
	kPerkEntry_IgnoreLockedTerminal = 0x14,
	kPerkEntry_AddLeveledListOnDeath = 0x15,
	kPerkEntry_GetMaxCarryWeight = 0x16,
	kPerkEntry_ModifyAddictionChance = 0x17,
	kPerkEntry_ModifyAddictionDuration = 0x18,
	kPerkEntry_ModifyPositiveChemDuration = 0x19,
	kPerkEntry_AdjustDrinkingRadiation = 0x1A,
	kPerkEntry_Activate = 0x1B,
	kPerkEntry_MysteriousStranger = 0x1C,
	kPerkEntry_HasParalyzingPalm = 0x1D,
	kPerkEntry_HackingScienceBonus = 0x1E,
	kPerkEntry_IgnoreRunningDuringDetection = 0x1F,
	kPerkEntry_IgnoreBrokenLock = 0x20,
	kPerkEntry_HasConcentratedFire = 0x21,
	kPerkEntry_CalculateGunSpread = 0x22,
	kPerkEntry_PlayerKillAPReward = 0x23,
	kPerkEntry_ModifyEnemyCriticalHitChance = 0x24,
	kPerkEntry_ReloadSpeed = 0x25,
	kPerkEntry_EquipSpeed = 0x26,
	kPerkEntry_ActionPointRegen = 0x27,
	kPerkEntry_ActionPointCost = 0x28,
	kPerkEntry_MissFortune = 0x29,
	kPerkEntry_ModifyRunSpeed = 0x2A,
	kPerkEntry_ModifyAttackSpeed = 0x2B,
	kPerkEntry_ModifyRadiationConsumed = 0x2C,
	kPerkEntry_HasPipHacker = 0x2D,
	kPerkEntry_HasMeltdown = 0x2E,
	kPerkEntry_SeeEnemyHealth = 0x2F,
	kPerkEntry_HasJuryRigging = 0x30,
	kPerkEntry_ModifyThreatRange = 0x31,
	kPerkEntry_ModifyThread = 0x32,
	kPerkEntry_HasFastTravelAlways = 0x33,
	kPerkEntry_KnockdownChance = 0x34,
	kPerkEntry_ModifyWeaponStrengthReq = 0x35,
	kPerkEntry_ModifyAimingMoveSpeed = 0x36,
	kPerkEntry_ModifyLightItems = 0x37,
	kPerkEntry_ModifyDamageThresholdDefender = 0x38,
	kPerkEntry_ModifyChanceforAmmoItem = 0x39,
	kPerkEntry_ModifyDamageThresholdAttacker = 0x3A,
	kPerkEntry_ModifyThrowingVelocity = 0x3B,
	kPerkEntry_ChanceforItemonFire = 0x3C,
	kPerkEntry_HasUnarmedForwardPowerAttack = 0x3D,
	kPerkEntry_HasUnarmedBackPowerAttack = 0x3E,
	kPerkEntry_HasUnarmedCrouchedPowerAttack = 0x3F,
	kPerkEntry_HasUnarmedCounterAttack = 0x40,
	kPerkEntry_HasUnarmedLeftPowerAttack = 0x41,
	kPerkEntry_HasUnarmedRightPowerAttack = 0x42,
	kPerkEntry_VATSHelperChance = 0x43,
	kPerkEntry_ModifyItemDamage = 0x44,
	kPerkEntry_HasImprovedDetection = 0x45,
	kPerkEntry_HasImprovedSpotting = 0x46,
	kPerkEntry_HasImprovedItemDetection = 0x47,
	kPerkEntry_AdjustExplosionRadius = 0x48,
	kPerkEntry_AdjustHeavyWeaponWeight = 0x49,
};

class BGSPerkEntry {
public:
	BGSPerkEntry();

	virtual void	Fn_00();
	virtual void	Fn_01();
	virtual void	Fn_02();
	virtual			~BGSPerkEntry();
	virtual UInt32	GetType();
	virtual void	Fn_05();
	virtual void	Fn_06();
	virtual void	Fn_07();
	virtual void	Fn_08();
	virtual void	Fn_09();
	virtual void	Fn_0A();
	virtual void	Fn_0B();
	virtual void	Fn_0C();
	virtual void	Fn_0D();

	UInt8				rank;				// 04 +1 for value shown in GECK
	UInt8				priority;			// 05
	UInt16				type;				// 06 (Quest: 0xC24, Ability: 0xB27, Entry Point: 0xD16)
};