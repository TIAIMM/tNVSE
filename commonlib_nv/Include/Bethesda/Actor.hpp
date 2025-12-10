#pragma once

#include "ActorMover.hpp"
#include "ActorValueOwner.hpp"
#include "CachedValuesOwner.hpp"
#include "ExtraContainerChanges.hpp"
#include "MagicCaster.hpp"
#include "MagicTarget.hpp"
#include "MobileObject.hpp"
#include "ModifierList.hpp"
#include "TESActorBase.hpp"
#include "BGSBodyPartData.hpp"
#include "BGSBodyPart.hpp"

struct FurnitureMark;
class TESTopic;
class BGSPerk;
class bhkRagdollController;
class bhkRagdollPenetrationUtil;
class TESActorBase;
class ContinuousBeamProjectile;
class TESPackage;
class TESRace;
class CombatGroup;
class CombatController;
class BGSEntryPointPerkEntry;
class SpellItem;
class ArrowProjectile;
class BSSoundHandle;
class ItemChange;
class TESObjectBOOK;

typedef std::vector<TESForm*> EquippedItemsList;

class Actor : public MobileObject, public MagicCaster, public MagicTarget, public ActorValueOwner, public CachedValuesOwner {
public:
	static inline auto bs_rtti = RTTI_Actor;

	Actor();
	virtual ~Actor();

	virtual bool				IsGuard() const;
	virtual void				SetGuard(bool abVal);
	virtual void				SetFlyInventoryWeight(float afVal);
	virtual float				GetFlyInventoryWeight() const;
	virtual void				Unk_C5();
	virtual void				Unk_C6();
	virtual void				SetIgnoreCrime(bool abVal);
	virtual bool				GetIgnoreCrime() const;
	virtual void				Resurrect(bool, bool, bool);
	virtual void				Unk_CA();
	virtual void				Unk_CB();
	virtual void				Unk_CC();
	virtual UInt32				GetMaxActorValues();
	virtual void				DamageHealthAndFatigue(float afHealth, float afFatigue, Actor* apSource);
	virtual void				DamageActionPoints(float afAmount);
	virtual void				Unk_D0();
	virtual UInt32				GetDisposition(Actor* apTarget, void* arg2);
	virtual void				UpdateMovement(float afTimeDelta, bool);
	virtual void				Unk_D3();
	virtual void				Unk_D4();
	virtual double				GetTurningSpeed();
	virtual bool				IsOverencumbered();
	virtual void				Unk_D7();
	virtual bool				IsPlayerRef();
	virtual bool				GetIsInCanibalAction() const;
	virtual void				Unk_DA();
	virtual bool				GetIsInSandmanAction() const;
	virtual void				Unk_DC();
	virtual void				InitiateSandManPackage(Actor* apTarget, TESObjectREFR* apFurnitureRef, FurnitureMark* apFurnitureMark, UInt8 aucFurnitureMarkerIndex);
	virtual void				InitiateCannibalPackage(Actor* apTarget);
	virtual TESRace* GetRace();
	virtual float				GetReach() const;
	virtual void				SetRefraction(bool, float);
	virtual void				SetHasRagdoll(bool);
	virtual bool				GetHasRagdoll();
	virtual UInt32				GetActorType();	// Creature = 0, Character = 1, PlayerCharacter = 2
	virtual void				SetActorValueF(UInt32 auiAVCode, float afValue);
	virtual void				SetActorValueI(UInt32 auiAVCode, SInt32 aiValue);
	virtual void				MagicModActorValueF(UInt32 auiAVCode, float afModifier, Actor* apAttacker); // SANITY CHECK ALL THESE!!!
	virtual void				MagicModActorValueI(UInt32 auiAVCode, SInt32 aiModifier, Actor* apAttacker);
	virtual void				ScriptModActorValueF(UInt32 auiAVCode, float afModifier, Actor* apAttacker);
	virtual void				ScriptModActorValueI(UInt32 auiAVCode, SInt32 aiModifier, Actor* apAttacker);
	virtual void				GameModActorValueF(UInt32 auiAVCode, float afModifier, Actor* apAttacker);
	virtual void				GameModActorValueI(UInt32 auiAVCode, SInt32 aiModifier, Actor* apAttacker);
	virtual void				ModActorBaseValueF(UInt32 auiAVCode, float afModifier);
	virtual void				ModActorBaseValueI(UInt32 auiAVCode, SInt32 aiModifier);
	virtual ItemChange* GetBestWeapon(UInt32 aeWeaponType) const;
	virtual ItemChange* GetBestAmmo() const;
	virtual void				ResetArmorRating();
	virtual bool				DamageItem(ItemChange* apItem, float afDamage, int unused = 0);
	virtual TESObjectREFR* DropObject(TESForm* apItem, ExtraDataList* apExtraList, SInt32 aiCount, const NiPoint3* apPoint, const NiPoint3* apRotate);
	virtual void				PickUpObject(TESObjectREFR* apObject, SInt32 aiCount, bool abPlayPickUpSounds);
	virtual void				CastScroll(TESObjectBOOK* apBook, MagicTarget* apTarget);
	virtual void				CheckCastWhenStrikesEnchantment(ItemChange* apItemChange, Actor* apAttacker, ArrowProjectile* apArrow, bool&);
	virtual void				Unk_F7();
	virtual bool				AddSpell(SpellItem* apSpell);
	virtual bool				RemoveSpell(SpellItem* apSpell);
	virtual bool				Reload(TESObjectWEAP* apWeapon, UInt32, bool);
	virtual bool				ReloadAlt(TESObjectWEAP* apWeapon, UInt32, bool, bool);
	virtual void				DecreaseClipAmmo();
	virtual void				CleanOrphanedMagicModifiers();
	virtual CombatGroup* GetCombatGroup() const;
	virtual void				SetCombatGroup(CombatGroup* apGroup);
	virtual void				Unk_100();
	virtual void				Unk_101(); // RunUpdates()?
	virtual BSSoundHandle* VoiceSoundFunctionEx(BSSoundHandle& arResult, TESTopic* apTopic, Actor* apTarget, bool abIs2D, bool abNeverSubtitle, int unused, bool abSayTo);
	virtual void				InitiateSpectator();
	virtual void				InitiateFlee(TESObjectREFR* apFleeRef, bool abRunOnce, bool abKnowsTarget, bool abCombatMode, TESObjectCELL* apCell, TESObjectREFR* apRef, float afFleeFromDist, float afFleeToDist);
	virtual void				InitiateSearch(TESObjectREFR*, NiPoint3);
	virtual void				InitiateGetUpPackage();
	virtual void				SetAlpha(float afValue);
	virtual float				GetAlpha();
	virtual void				ForceAttackActor(bool, bool, bool, bool, bool, int, bool, int);
	virtual CombatController* GetCombatController();
	virtual Actor* GetCombatTarget();
	virtual void				UpdateCombat(float);
	virtual void				StopCombat(Actor*);
	virtual void				Unk_10E();
	virtual double				GetTotalArmorDR();
	virtual double				GetTotalArmorDT();
	virtual void				Unk_111();
	virtual bool				IsTrespassing();
	virtual void				SetTresPassing(bool abVal);
	virtual void				Unk_114();
	virtual void				Unk_115();
	virtual double				CalculateWalkSpeed();
	virtual double				CalculateRunSpeed();
	virtual void				ModDisposition(TESForm* apTarget, float afValue);
	virtual double				GetDispositionForTarget(TESForm* apTarget);
	virtual void				RemoveDispositionForTarget(int id);
	virtual void				SetActorStartingPosition();
	virtual bool				HasBeenAttacked() const;
	virtual void				SetBeenAttacked(bool abVal);
	virtual void				HitMe(ActorHitData* apHitData, float afHealthDmg, float afBlockDTMod, NiPoint3& unk, NiPoint3& arWeaponPos);
	virtual void				Unk_11F();
	virtual void				Unk_120();
	virtual void				Unk_121();
	virtual void				RewardXP();	//	0122	Check for LevelUp
	virtual float				InternalGetActorValue(ActorValue::Index aeIndex, bool& arFound);
	virtual bool				InternalSetActorValue(ActorValue::Index aeIndex, float afValue);
	virtual void				UpdateWeaponWobble();
	virtual void				SetPerkRank(BGSPerk* apPerk, UInt8 rank, bool alt);
	virtual void				RemovePerk(BGSPerk* apPerk, bool alt);
	virtual UInt8				GetPerkRank(BGSPerk* apPerk, bool alt);
	virtual void				AddPerkEntryPoint(BGSEntryPointPerkEntry* perkEntry, bool alt);
	virtual void				RemovePerkEntry(BGSEntryPointPerkEntry* perkEntry, bool alt);
	virtual void* GetPerkEntryListByID(UInt8 entryPointID, bool alt);
	virtual void				Unk_12C();
	virtual bool				IsImmobileCreature() const;
	virtual void				HandleHealthDamage(Actor* apAttacker, float afDamage);
	virtual void				AttackedBy(Actor* apAttacker, ActiveEffect* apEffect = nullptr);
	virtual void				HandleBlockedAttack(float, float, Actor*, ArrowProjectile*);
	virtual void				Unk_131();
	virtual void				HandleHeadTracking(float afTime);
	virtual void				UpdateHeadTrackingEmotions(int empty = 0);
	virtual void				CreateActorMover();
	virtual void				DestroyActorMover();
	virtual NiPoint3* GetAnticipatedLocation(NiPoint3& arPosition, float afUnk);

	struct Disposition {
		float	fValue;
		Actor* pTarget;
	};

	bhkRagdollController* pRagdollController;
	bhkRagdollPenetrationUtil* pRagdollPenetrationUtil;
	UInt32						unk0B4;
	float						flt0B8;
	bool						bIsAIProcessing;
	UInt8						byte0BD;
	UInt8						byte0BE;
	UInt8						byte0BF;
	Actor* pKiller;
	UInt8						byte0C4;
	UInt8						byte0C5;
	UInt8						byte0C6;
	UInt8						byte0C7;
	float						fTime0C8;
	float						fCheckDeadBodyTimer;
	ModifierList				kForceAVList;
	ModifierList				kSetAVList;
	UInt8						byte0F0;
	UInt8						byte0F1;
	UInt8						byte0F2;
	UInt8						byte0F3;
	BSSimpleList<UInt32>		list0F4;
	BSSimpleList<Disposition*>	kDispositions;
	bool						bIsInCombat;
	Bitfield8					ucJIPActorFlags1;
	Bitfield8					ucJIPActorFlags2;
	Bitfield8					ucJIPActorFlags3;
	UInt32						uiLifeState;
	UInt32						uiCriticalStage;
	UInt32						uiQueuedAttackAnim;
	float						fPainSoundTimer;
	UInt8						byte118;
	UInt8						byte119;
	UInt16						usJIP11A;
	Bitfield32					uiFlags11C;
	UInt32						hoursPassed120;
	bool						bForceRun;
	bool						bForceSneak;
	bool						byte126;
	bool						bHasSearch;
	Actor* pCombatTarget;
	BSSimpleArray<Actor*>* pCombatTargets;
	BSSimpleArray<Actor*>* pCombatAllies;
	UInt8						byte134;
	UInt8						byte135;
	UInt16						usJIP136;
	UInt32						unk138;
	UInt32						uiMinorCrimeCount;
	Bitfield32					uiActorFlags;
	bool						bIgnoreCrime;
	bool						byte145;
	bool						bIsResetAI;
	bool						byte147;
	TESActorBase* pActorbase148;
	bool						bInWater;
	bool						bIsSwimming;
	bool						bAddToShadowSceneNode;
	bool						gap14F;
	UInt32						iActionValue;
	float						fTimerOnAction;
	float						fUpdateTargetTimer;
	UInt8						byte15C;
	UInt8						bShouldRotateToTrack;
	UInt16						usJIP15E;
	NiPoint3					kStartingPos;
	float						fStartingZRot;
	TESForm* pStartingWorldOrCell;
	UInt8						byte174;
	UInt8						byte175;
	UInt16						usJIP176;
	float						fCachedSpread_Projectile;
	float						fCachedSpread_Scoped;
	float						fCachedSpread_WeaponModel;
	float						fCachedSpread_VATS;
	float						flt188;
	UInt8						isCameraRagdollAnimDist;
	bool						bIsTeammate;
	bool						byte18E;
	bool						byte18F;
	ActorMover* pActorMover;
	UInt32						unk194;
	TESPackage* pPackage198;
	float						fOneMinusRadResist;
	ContinuousBeamProjectile* pContinuousBeamProjectile;
	UInt32						uiSpeechExpression;
	UInt32						uiEmotionValue;
	UInt32						eSitSleepState;
	bool						bIsTurret;
	bool						bForceHit;
	bool						byte1B2;
	bool						byte1B3;

	bool isDead() {
		return ThisStdCall(0x8844F0, this, false);
	}

	bool IsTeammate() const {
		return bIsTeammate;
	}
	void SetIsTeammate(bool isTeammate) {
		ThisStdCall(0x8BCA90, this, isTeammate);
	}

	double GetHeadingAngle(Actor *target) {
		double result = 0;
		CdeclCall(0x5A0410, this, target, 0, &result);
		return result;
	}

	void RefreshAnimation(bool a2) {
		ThisStdCall(0x8B0B00, this, a2);
	}

	TESObjectWEAP* GetEquippedWeapon() {
		return ThisStdCall<TESObjectWEAP*>(0x8A1710, this);
	}

	void DoHitMe(ActorHitData *hitData) {
		ThisStdCall(0x89A760, this, hitData, 0);
	}

	void ObjectHit(bool bPowerAttack) {
		ThisStdCall(0x8AE660, this, bPowerAttack);
	}

	double GetTargetDistance(Actor *a2, const char a3) const {
		return CdeclCall<double>(0x9A64D0, this, a2, a3);
	}

	bool CanHaveBlood() const {
		return ThisStdCall<bool>(0x87F260, this);
	}

	NiAVObjectPtr GetLimbNode(const uint32_t index) const {
		const auto base = DYNAMIC_CAST<TESForm, TESActorBase>(this->pBaseForm);
		if (!base) {
			return nullptr;
		}

		const auto bpData = base->GetBodyPartData();
		if (!bpData) {
			return nullptr;
		}

		const auto bodyPart = bpData->GetBodyPart(index);
		if (!bodyPart) {
			return nullptr;
		}

		const auto nif = this->Get3D();
		if (!nif) {
			return nullptr;
		}

		return nif->GetObjectByName(bodyPart->partNode.pString);
	}

	bool IsAiming() const {
		return ThisStdCall<bool>(0x8BBC10, this);
	}
};

ASSERT_SIZE(Actor, 0x1B4);
ASSERT_OFFSET(Actor, pRagdollController, 0xAC);
ASSERT_OFFSET(Actor, pActorMover, 0x190);