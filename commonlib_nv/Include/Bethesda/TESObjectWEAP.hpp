#pragma once

#include "TESBoundObject.hpp"
#include "TESModelTextureSwap.hpp"
#include "TESIcon.hpp"
#include "TESScriptableForm.hpp"
#include "TESEnchantableForm.hpp"
#include "TESValueForm.hpp"
#include "TESWeightForm.hpp"
#include "TESHealthForm.hpp"
#include "BGSPickupPutdownSounds.hpp"
#include "BGSBipedModelList.hpp"
#include "BGSMessageIcon.hpp"
#include "BGSPreloadable.hpp"
#include "BGSEquipType.hpp"
#include "BGSRepairItemList.hpp"
#include "BGSDestructibleObjectForm.hpp"
#include "BGSClipRoundsForm.hpp"
#include "BGSAmmoForm.hpp"
#include "TESAttackDamageForm.hpp"

class BGSProjectile;
class SpellItem;
class BGSImpactDataSet;
class TESObjectSTAT;
class TESObjectIMOD;

class TESObjectWEAP : public TESBoundObject, public TESFullName, public TESModelTextureSwap, public TESIcon, public TESScriptableForm, public TESEnchantableForm,
						public TESValueForm, public TESWeightForm, public TESHealthForm, public TESAttackDamageForm, public BGSAmmoForm, public BGSClipRoundsForm, 
							public BGSDestructibleObjectForm, public BGSRepairItemList, public BGSEquipType, public BGSPreloadable, public BGSMessageIcon, public BGSBipedModelList, 
								public BGSPickupPutdownSounds {
public:
	static inline auto bs_rtti = RTTI_TESObjectWEAP;

	TESObjectWEAP();
	~TESObjectWEAP();

	enum EWeaponType {
		kWeapType_HandToHandMelee = 0,
		kWeapType_OneHandMelee,
		kWeapType_TwoHandMelee,
		kWeapType_OneHandPistol,
		kWeapType_OneHandPistolEnergy,
		kWeapType_TwoHandRifle,
		kWeapType_TwoHandAutomatic,
		kWeapType_TwoHandRifleEnergy,
		kWeapType_TwoHandHandle,
		kWeapType_TwoHandLauncher,
		kWeapType_OneHandGrenade,
		kWeapType_OneHandMine,
		kWeapType_OneHandLunchboxMine,
		kWeapType_OneHandThrown,
		kWeapType_Last	// During animation analysis, player weapon can be classified as 0x0C = last
	};

	enum EWeaponSounds {
		kWeapSound_Shoot3D = 0,
		kWeapSound_Shoot2D,
		kWeapSound_Shoot3DLooping,
		kWeapSound_NoAmmo,
		kWeapSound_Swing = kWeapSound_NoAmmo,
		kWeapSound_Block,
		kWeapSound_Idle,
		kWeapSound_Equip,
		kWeapSound_Unequip
	};

	enum EHandGrip {
		eHandGrip_Default = 0xFF,
		eHandGrip_1 = 0xE6,
		eHandGrip_2 = 0xE7,
		eHandGrip_3 = 0xE8,
		eHandGrip_4 = 0xE9,
		eHandGrip_5 = 0xEA,
		eHandGrip_6 = 0xEB,
		eHandGrip_Count = 7,
	};

	enum EAttackAnimations {
		eAttackAnim_Default = 255,
		eAttackAnim_Attack3 = 38,
		eAttackAnim_Attack4 = 44,
		eAttackAnim_Attack5 = 50,
		eAttackAnim_Attack6 = 56,
		eAttackAnim_Attack7 = 62,
		eAttackAnim_Attack8 = 68,
		eAttackAnim_Attack9 = 144,
		eAttackAnim_AttackLeft = 26,
		eAttackAnim_AttackLoop = 74,
		eAttackAnim_AttackRight = 32,
		eAttackAnim_AttackSpin = 80,
		eAttackAnim_AttackSpin2 = 86,
		eAttackAnim_AttackThrow = 114,
		eAttackAnim_AttackThrow2 = 120,
		eAttackAnim_AttackThrow3 = 126,
		eAttackAnim_AttackThrow4 = 132,
		eAttackAnim_AttackThrow5 = 138,
		eAttackAnim_AttackThrow6 = 150,
		eAttackAnim_AttackThrow7 = 156,
		eAttackAnim_AttackThrow8 = 162,
		eAttackAnim_PlaceMine = 102,
		eAttackAnim_PlaceMine2 = 108,
		eAttackAnim_Count = 23,
	};

	enum ReloadAnim {
		eReload_A = 0,
		eReload_B,
		eReload_C,
		eReload_D,
		eReload_E,
		eReload_F,
		eReload_G,
		eReload_H,
		eReload_I,
		eReload_J,
		eReload_K,
		eReload_L,
		eReload_M,
		eReload_N,
		eReload_O,
		eReload_P,
		eReload_Q,
		eReload_R,
		eReload_S,
		eReload_W,
		eReload_X,
		eReload_Y,
		eReload_Z,
		eReload_Count,
	};
	STATIC_ASSERT(eReload_Count == 23);

	enum EWeaponFlags1 {
		eFlag_IgnoresNormalWeapResist = 0x1,
		eFlag_IsAutomatic = 0x2,
		eFlag_HasScope = 0x4,
		eFlag_CantDrop = 0x8,
		eFlag_HideBackpack = 0x10,
		eFlag_EmbeddedWeapon = 0x20,
		eFlag_No1stPersonISAnims = 0x40,
		Eflag_NonPlayable = 0x80
	};

	enum EWeaponFlags2 {
		eFlag_PlayerOnly = 0x1,
		eFlag_NPCsUseAmmo = 0x2,
		eFlag_NoJamAfterReload = 0x4,
		eFlag_ActionPointOverride = 0x8,
		eFlag_MinorCrime = 0x10,
		eFlag_FixedRange = 0x20,
		eFlag_NotUsedNormalCombat = 0x40,
		eFlag_DamageToWeaponOverride = 0x80,
		eFlag_No3rdPersonISAnims = 0x100,
		eFlag_BurstShot = 0x200,
		eFlag_RumbleAlternate = 0x400,
		eFlag_LongBurst = 0x800,
	};

	enum EEmbedWeapAV {
		eEmbedAV_Perception = 0,
		eEmbedAV_Endurance,
		eEmbedAV_LeftAttack,
		eEmbedAV_RightAttack,
		eEmbedAV_LeftMobility,
		eEmbedAV_RightMobility,
		eEmbedAV_Brain,
	};

	enum EOnHit {
		eOnHit_Normal = 0,
		eOnHit_DismemberOnly,
		eOnHit_ExplodeOnly,
		eOnHit_NoDismemberOrExplode,
	};

	enum ERumblePattern {
		eRumblePattern_Constant = 0,
		eRumblePattern_Square,
		eRumblePattern_Triangle,
		eRumblePattern_Sawtooth
	};

	enum ECritDamageFlags {
		eCritDamage_OnDeath = 0x1
	};

	enum
	{
		kWeaponModEffect_None = 0,
		kWeaponModEffect_IncreaseDamage,
		kWeaponModEffect_IncreaseClipCapacity,
		kWeaponModEffect_DecreaseSpread,
		kWeaponModEffect_DecreaseWeight,
		kWeaponModEffect_Unused05,
		kWeaponModEffect_RegenerateAmmo,
		kWeaponModEffect_DecreaseEquipTime,
		kWeaponModEffect_IncreaseRateOfFire,		// 8
		kWeaponModEffect_IncreaseProjectileSpeed,
		kWeaponModEffect_IncreaseMaxCondition,
		kWeaponModEffect_Silence,
		kWeaponModEffect_SplitBeam,
		kWeaponModEffect_VATSBonus,
		kWeaponModEffect_IncreaseZoom,				// 14
	};

	UInt8				eWeaponType;		// 0F4
	UInt8				pad[3];
	float				animMult;			// 0F8
	float				reach;				// 0FC
	Bitfield8			weaponFlags1;		// 100
	UInt8				handGrip;			// 101
	UInt8				ammoUse;			// 102
	UInt8				reloadAnim;			// 103
	float				minSpread;			// 104
	float				spread;				// 108
	UInt32				unk10C;				// 10C
	float				sightFOV;			// 110
	UInt32				unk114;				// 114
	TESForm*			projectile;			// 118
	UInt8				baseVATSChance;		// 11C
	UInt8				attackAnim;			// 11D
	UInt8				numProjectiles;		// 11E
	UInt8				embedWeaponAV;		// 11F
	float				minRange;			// 120
	float				maxRange;			// 124
	UInt32				onHit;				// 128
	Bitfield32			weaponFlags2;		// 12C
	float				animAttackMult;		// 130
	float				fireRate;			// 134
	float				AP;					// 138
	float				rumbleLeftMotor;	// 13C
	float				rumbleRightMotor;	// 140
	float				rumbleDuration;		// 144
	float				damageToWeaponMult;	// 148
	float				animShotsPerSec;	// 14C
	float				animReloadTime;		// 150
	float				animJamTime;		// 154
	float				aimArc;				// 158
	UInt32				weaponSkill;		// 15C - actor value
	UInt32				rumblePattern;		// 160 - reload anim?
	float				rumbleWavelength;	// 164
	float				limbDamageMult;		// 168
	UInt32				resistType;			// 16c - actor value
	float				sightUsage;			// 170
	float				semiAutoFireDelayMin;	// 174
	float				semiAutoFireDelayMax;	// 178
	UInt32				unk17C;				// 17C - 0-0x10: 0x8:str req 0x10: - skill req  - 0xb:kill impulse B158 - mod 1 val B15C - Mod 2 val Effects: 0x1: e(zoom) 0x2: a 0x3:0 0x4-6: Values c-e Mod Effects Val2:1-3
	UInt32				effectMods[3];		// 180
	float				value1Mod[3];		// 18C
	UInt32				powerAttackAnimOverride;	// 198
	UInt32				strRequired;		// 19C
	UInt8				unk1A0;				// 1A0
	UInt8				modReloadAnim;		// 1A1
	UInt8				pad1A2[2];			// 1A2
	float				regenRate;			// 1A4
	float				killImpulse;		// 1A8
	float				value2Mod[3];		// 1AC
	float				impulseDist;		// 1B8
	UInt32				skillRequirement;	// 1BC
	UInt16				criticalDamage;		// 1C0
	UInt8				unk1C2[2];			// 1C2
	float				criticalPercent;	// 1C4
	UInt8				critDamageFlags;	// 1C8
	UInt8				pad1C9[3];			// 1C9
	SpellItem*			criticalEffect;	// 1CC
	TESModel			shellCasingModel;	// 1DO
	TESModel			targetNIF;			// 1E8 - target NIF
	TESModel			model200;			// 200 - could be a texture swap
	UInt32				unk218;				// 218
	TESSound*			sounds[12];		// 21C
	BGSImpactDataSet*	impactDataSet;	// 24C
	TESObjectSTAT*		worldStatic;		// 250
	TESObjectSTAT*		mod1Static;		// 254
	TESObjectSTAT*		mod2Static;		// 258
	TESObjectSTAT*		mod3Static;		// 25C
	TESObjectSTAT*		mod12Static;		// 260
	TESObjectSTAT*		mod13Static;		// 264
	TESObjectSTAT*		mod23Static;		// 268
	TESObjectSTAT*		mod123Static;		// 26C
	TESModelTextureSwap	textureMod1;		// 270 Mod 1
	TESModelTextureSwap	textureMod2;		// 290 Mod 2
	TESModelTextureSwap	textureMod3;		// 2B0 Mod 3
	TESModelTextureSwap	textureMod12;		// 2D0 Mod 1-2
	TESModelTextureSwap	textureMod13;		// 2F0 Model 1-3
	TESModelTextureSwap	textureMod23;		// 310 Model 2-3
	TESModelTextureSwap	textureMod123;		// 330 Model 1-2-3
	TESObjectIMOD*		itemMod1;			// 350
	TESObjectIMOD*		itemMod2;			// 354
	TESObjectIMOD*		itemMod3;			// 358
	UInt32				unk35C;				// 35C
	UInt32				unk360;				// 360
	UInt32				soundLevel;			// 364
	BSString			VATSAttackName;		// 368, credits to JIP
	SpellItem*			VATSEffect;			// 370, credits to JIP
	float				vatsSkill;			// 374, credits to JIP
	float				vatsDmgMult;		// 378, credits to JIP
	float				vatsAP;				// 37C, credits to JIP
	UInt32				recharge;			// 380 maybe recharge
	UInt8				unk384;				// 384
	UInt8				unk385[3];			// 385

	/*
	TESAmmo* GetAmmo() const { 
		TESAmmo* pAmmo = DYNAMIC_CAST(pAmmoForm, TESForm, TESAmmo);
		if (pAmmo) {
			pWeapon->ammo.ammo = pAmmo;
		}
		else 
		return ; }
	BGSListForm* GetAmmoList() const {

	}*/
	bool IsAutomatic() const { return weaponFlags1.GetBit(eFlag_IsAutomatic); }
	void SetIsAutomatic(bool bAuto) { weaponFlags1.SetBit(eFlag_IsAutomatic, bAuto); }
	bool LongBursts() const { return weaponFlags2.GetBit(eFlag_LongBurst); }
	void SetLongBursts(bool longBursts) { weaponFlags2.SetBit(eFlag_LongBurst, longBursts); }
	bool HasScope() const { return weaponFlags1.GetBit(eFlag_HasScope); }
	bool IsNonPlayable() { return weaponFlags1.GetBit(0x80); }
	bool IsPlayable() { return !IsNonPlayable(); }
	UInt8 HandGrip() const;
	void SetHandGrip(UInt8 handGrip);
	UInt8 AttackAnimation() const;
	void SetAttackAnimation(UInt8 attackAnim);
	// 1-indexed
	TESObjectIMOD* GetNthItemMod(UInt8 which) {
		switch (which) { case 1: return itemMod1; case 2: return itemMod2; case 3: return itemMod3;
		default: return nullptr; }
	}
	// 1-indexed
	bool SetNthItemMod(UInt8 which, TESObjectIMOD* newMod) {
		switch (which) {
		case 1: itemMod1 = newMod; break; case 2: itemMod2 = newMod; break; case 3: itemMod3 = newMod; break;
		default: return false;
		} return true;
	}
	UInt32 GetNthItemModEffect(UInt8 which) const { return effectMods[which]; }
	void SetNthItemModEffect(UInt8 which, UInt32 newEffect) { effectMods[which] = newEffect; }
	float GetNthItemModValue1(UInt8 which) const { return value1Mod[which]; }
	float GetNthItemModValue2(UInt8 which) const { return value2Mod[which]; }
	void SetNthItemModValue1(UInt8 which, float newVal) { value1Mod[which] = newVal; }
	void SetNthItemModValue2(UInt8 which, float newVal) { value2Mod[which] = newVal; }

	double GetAnimShotsPerSec() {
		return ThisStdCall<double>(0x645CA0, this);
	}

	void EjectShellCasing(TESObjectREFR* apReference);
};

ASSERT_OFFSET(TESObjectWEAP, handGrip, 0x101);
ASSERT_OFFSET(TESObjectWEAP, projectile, 0x118);
ASSERT_OFFSET(TESObjectWEAP, animShotsPerSec, 0x14C);
ASSERT_OFFSET(TESObjectWEAP, impactDataSet, 0x24C);
ASSERT_SIZE(TESObjectWEAP, 0x388);