#pragma once

#include "ActorPackage.hpp"
#include "NiPoint3.hpp"
#include "NiSmartPointer.hpp"
#include "BSSimpleList.hpp"
#include "ActorValue.hpp"

class TESForm;
class Actor;
class ActiveEffect;
struct ActorHitData;
class BSAnimGroupSequence;
class BSBound;
class BSFaceGenAnimationData;
class BSFaceGenNiNode;
class DetectionData;
class NiNode;
class NiTriShape;
class TESAmmo;
class TESIdleForm;
class TESObjectWEAP;
class bhkCharacterController;
class MobileObject;
class BSSoundHandle;
class TESEffectShader;
class KFModel;
class BSShaderPPLightingProperty;
class NiAVObject;
class NiRefObject;
class TESObjectCELL;
class TESWorldSpace;
class Projectile;
class MuzzleFlash;
class BGSSaveGameBuffer;
class BGSLoadFormBuffer;
class TESTopic;
class Crime;
class ItemChange;
class SpellItem;
class MagicItem;
class MagicTarget;
class MagicCaster;
class TESActorBase;
class BipedAnim;
struct FurnitureMark;

class Animation;
class ExtraDataList;

class BaseProcess {
public:
	BaseProcess();

	struct AmmoInfo {
		void*			unk00;	// 00
		UInt32			uiCount;	// 04
		TESAmmo*		pAmmo;	// 08
		UInt32			unk0C;	// 0C
		UInt32			unk10;	// 10
		UInt32			unk14;	// 14
		UInt32			unk18;	// 18
		UInt32			unk1C;	// 1C
		UInt32			unk20;	// 20
		UInt32			unk24;	// 24
		UInt32			unk28;	// 28
		UInt32			unk2C;	// 2C
		UInt32			unk30;	// 30
		UInt32			unk34;	// 34
		UInt32			unk38;	// 38
		UInt32			unk3C;	// 3C
		UInt32			unk40;	// 40
		TESObjectWEAP*	pWeapon;	// 44
	};

	struct WeaponInfo {
		ExtraDataList** xData;
		UInt32			unk04;
		TESObjectWEAP*	pWeapon;

		ExtraDataList* GetExtraData() const {
			return xData ? *xData : nullptr;
		}
	};

	struct CachedValues {
		enum
		{
			kCached_Radius = 0x1,
			kCached_WidthX = 0x2,
			kCached_WidthY = 0x4,
			kCached_DPS = 0x8,
			kCached_MedicineSkillMult = 0x10,
			kCached_Paralysis = 0x20,
			kCached_HealRate = 0x40,
			kCached_FatigueReturnRate = 0x80,
			kCached_PerceptionCondition = 0x100,
			kCached_EyeHeight = 0x200,
			kCached_SomethingShouldAttack = 0x400,
			kCached_WalkSpeed = 0x1000,
			kCached_RunSpeed = 0x2000,
			kCached_NoCrippledLegs = 0x4000,
			kCached_Height = 0x8000,
			kCached_IsGhost = 0x10000000,
			kCached_Health = 0x20000000,
			kCached_Fatigue = 0x40000000,
			kCached_SurvivalSkillMult = 0x80000000
		};

		float		fRadius;
		float		fWidthX;
		float		fWidthY;
		float		fHeight;
		float		fDPS;
		float		fMedicineSkillMult;
		float		fSurvivalSkillMult;
		float		fParalysis;
		float		fHealRate;
		float		fFatigueReturnRate;
		float		fPerceptionCondition;
		float		fEyeHeight;
		SInt32		unk30;
		SInt32		unk34;
		float		fWalkSpeed;
		float		fRunSpeedMult;
		bool		bHasNoCrippledLegs;
		Bitfield32	uiFlags;
	};

	enum ProcessLevel {
		INVALID			= -1,
		HIGH			= 0,
		MIDDLE_HIGH		= 1,
		MIDDLE_LOW		= 2,
		LOW				= 3,
		COUNT			= 4
	};

	virtual									~BaseProcess();
	virtual void							Copy(BaseProcess* apSource);
	virtual void							CopyPackage(TESPackageData* apPackageData);
	virtual void							UpdateAlt(MobileObject* apObject, float afDelta);
	virtual void							Update(MobileObject* apObject);
	virtual void							UpdateMissedPackages(MobileObject* apObject, bool);
	virtual NiPoint3*						GetCurrentPackageLocation(NiPoint3&, MobileObject*);
	virtual void							GetDetectionLocation();
	virtual double							GetDetectionTimeStamp(int, int);
	virtual void							CheckForNewPackage();
	virtual void							ComputeLastTimeProcessed();
	virtual bool							IsWandering() const;
	virtual UInt32							GetLastProcessedDay() const;
	virtual UInt32							GetLastProcessedMonth() const;
	virtual UInt32							GetLastProcessedYear() const;
	virtual void							SetHourPackageEvaluated(SInt32 aiVal);
	virtual SInt32							GetHourPackageEvaluated() const;
	virtual bool							SetupSpecialIdle(Actor* apActor, TESIdleForm* apIdle, UInt32 aeDefaultAction, bool abCrippled, bool abForce, bool abTestConditions);
	virtual void							UpdateLastSeenTargetPosition();
	virtual UInt32							GetMagicItem164();
	virtual void							SetMagicItem164(UInt32);
	virtual void							Unk_15();
	virtual void							ClearFaceAnimationData();
	virtual void							Unk_17();
	virtual bool							GetIdleDoneOnce() const;
	virtual void							SetIdleDoneOnce(bool abVal);
	virtual void							ProcessDetection(Actor* apActor);
	virtual void							Unk_1B();
	virtual bool							Unk_1C(Actor* apActor);
	virtual void							Unk_1D();
	virtual bool							GetIsDoingSayTo();
	virtual void							ProcessSandman();
	virtual void							ProcessCannibal();
	virtual void							Unk_21();
	virtual void							SetDoingSayTo();
	virtual void							SetTargetForPackage();
	virtual void							SetLocationForPackage();
	virtual TESObjectREFR*					GetCurrentDestinationReference(Actor* apActor, bool abInterrupt);
	virtual NiPoint3*						GetCurrentDestinationCoordinate(NiPoint3& arPos, Actor* apActor, bool abInterrupt);
	virtual TESObjectCELL*					GetCurrentDestinationCell(Actor* apActor);
	virtual TESWorldSpace*					GetCurrentDestinationWorldSpace(Actor* apActor);
	virtual float							GetCurrentDestinationRadius(Actor* apActor);
	virtual void							Unk_2A();
	virtual void							ClearDialogTarget();
	virtual void							Unk_2C(Actor* apActor, TESPackage* apPackage, bool);
	virtual bool							Unk_2D(Actor* apActor, TESPackage** appPackage);
	virtual double							GetSocialTimerForConversations();
	virtual void							SetSocialTimerForConversations(float afValue);
	virtual void							RemoveActorFromList264(Actor* apActor);
	virtual void							Unk_31(MobileObject* apObject, bool);
	virtual void							CreateFollowforEscort(Actor* apActor, TESPackage* apPackage, bool);
	virtual void							Unk_33();
	virtual void							SetWeaponAlertDrawn();
	virtual void							Unk_35();
	virtual void							Unk_36();
	virtual void							Unk_37();
	virtual void							Unk_38();
	virtual void							GetEssentialDownTimer();
	virtual void							SetEssentialDownTimer();
	virtual void							Unk_3B();
	virtual void*							InsertIntoDetectionList(Actor* apActor, UInt32 aeDetectionLevel, bool, SInt32, bool, bool, bool);
	virtual void							GetDetectingActors();
	virtual void							GetActorsGeneratedDetectionEvent();
	virtual void							CreateDetectionEvent();
	virtual void							FreeGeneratedDetectionEvent();
	virtual void							Unk_41();
	virtual void							Unk_42();
	virtual void							Unk_43();
	virtual bool							HasCaughtPlayerPickpocketting();
	virtual void							SetHasCaughtPlayerPickpocketting();
	virtual void							SetCurrentActionComplete();
	virtual void							Unk_47();
	virtual void							Unk_48();
	virtual void							Unk_49();
	virtual TESForm*						GetTarget();
	virtual void							SetTarget(TESForm*);
	virtual void							Unk_4C();
	virtual void							Unk_4D();
	virtual void							Unk_4E();
	virtual void							Unk_4F();
	virtual void							Unk_50();
	virtual BSFaceGenAnimationData*			GetFaceAnimationData();
	virtual WeaponInfo*						GetWeaponInfo() const;
	virtual AmmoInfo*						GetAmmoInfo() const;
	virtual void							Unk_54();
	virtual void							Unk_55();
	virtual void							Unk_56();
	virtual void							Unk_57();
	virtual void							Unk_58();
	virtual void							Unk_59();
	virtual void							UpdateAmmoInfo(AmmoInfo*);
	virtual void							Unk_5B();
	virtual void							HandleQueuedEquipItems();
	virtual void							Unk_5D();	// Called by 5E with count itemExtraList item
	virtual void							AppendQueuedEquipItem();	// EquipItem and UnEquipItem doEquip item count itemExtraList bytes = [equipArgC lockUnequip unk unEquipArcC lockEquip arg14 ] (arg as from Actor::(Un)EquipItem)
	virtual void							RemoveQueuedItem();
	virtual void							RemoveQueuedEquipItems();
	virtual NiNode*							GetProjectileNode() const;
	virtual void							SetProjectileNode(NiNode* apNode);
	virtual void							SetupWeaponNode();
	virtual NiNode*							GetWeaponBone(BipedAnim* apAnim);
	virtual void							Unk_65();
	virtual void							Unk_66();
	virtual void							Unk_67();
	virtual NiNode*							GetWeaponNode(bool abUnk);
	virtual NiNode*							GetBowNode();
	virtual bool							IsUsingOneHandGrenade();
	virtual bool							IsUsingOneHandMine();
	virtual bool							IsUsingThrownWeapon();
	virtual bool							IsUsingOneHandThrown();
	virtual Animation*						GetAnimData();
	virtual void							Unk_6F();
	virtual void							Unk_70();
	virtual void							Unk_71();
	virtual void							Unk_72(bool abVal);
	virtual void							Unk_73();
	virtual void							Unk_74();
	virtual void							Unk_75();
	virtual void							Unk_76();
	virtual void							CreateFollownoEscort(Actor* apActor, bool abFlag);
	virtual float							GetAwarePlayerTimer() const;
	virtual void							ModAwarePlayerTimer(float afVal);
	virtual void							Unk_7A();
	virtual bool							Unk_7B();
	virtual void							Unk_7C();
	virtual void							Unk_7D();
	virtual void							Unk_7E();
	virtual void							Unk_7F();
	virtual void							CheckIfThereSomeoneTalkWith();
	virtual void							Unk_81();
	virtual void							Unk_82();
	virtual TESPackage*						GetRunOncePackage();
	virtual void							SetInterruptPackage(TESPackage* package, Actor* onActor);
	virtual void							StopInterruptPackage();
	virtual void							Unk_86();	// 086 - SetInterruptPackageTargetRef
	virtual void							Unk_87();	// 087 - SetInterruptPackageTargetRef
	virtual void							Unk_88();	// 088 - IncreaseInterruptPackageUnk00C
	virtual void							ClearGreetingInfoData();
	virtual bool							IsCurrentProcedureDone() const;
	virtual TESPackage*						GetStablePackage();
	virtual void							SetStablePackage(TESPackage* package, Actor* onActor);
	virtual void							ClearCurrentPackage();
	virtual void							SetCurrentProcedureIndex();
	virtual UInt32							GetCurrentProcedureIndex() const;
	virtual void							AddToCurrentProcedureIndex(SInt32 aiOffset);
	virtual void							SetupNewPackage();
	virtual void*							Unk_92() const;	// Only HighProcess, get Unk0454
	virtual void							Unk_93(void*);
	virtual void							Unk_94();
	virtual void							GetAvoidNodes();
	virtual bool							IsAvoidAreaInAvoidPathingList(TESObjectREFR* apRef);
	virtual void							SetAvoidWaitTimer(float afVal);
	virtual void							RemoveAvoidPathingNode(TESObjectREFR* apRef);
	virtual float							GetHoldHeadTrackTimer() const;
	virtual void							SetHoldHeadTrackTimer(float afVal);
	virtual void							ResetPackageTarget();
	virtual TESPackageData*					GetRunOncePackageData();
	virtual TESPackageData*					GetPackageData();
	virtual TESPackage*						GetActorPackageThatIsRunning() const;
	virtual TESPackage*						GetCurrentPackage() const;
	virtual UInt32							GetProcedureIndexRunning();
	virtual void							SetProcedureIndexRunning(UInt32);
	virtual void							AddToProcedureIndexRunning();
	virtual bhkCharacterController*			GetCharacterController() const;
	virtual void							SetCharacterController(NiPointer<bhkCharacterController>& arController);
	virtual void							StopMoving(Actor* apActor);
	virtual void							ProcessFollow(Actor* apActor, bool abFlag, UInt32 aeMovemode, bool abPursuit);
	virtual void							ProcessPursue();
	virtual void							ProcessFlee(Actor* apActor);
	virtual void							ProcessGreet(Actor* apActor, TESTopic* apTopic, bool abForceSub, bool abStop, bool abQueue, bool abSayCallBack);
	virtual void							ProcessObserveCombat(Actor* apActor);
	virtual void							StopInteractingQuick();
	virtual void							Unk_AC();
	virtual bool							Unk_AD(TESObjectREFR* apFurnitureRef, UInt32 auiFurnitureType);
	virtual void							Unk_AE();
	virtual void							ProcessTravel();
	virtual void							Unk_B0();
	virtual void							Unk_B1();
	virtual void							SetDetectionTimer();
	virtual void							Unk_B3(int, Actor* apTarget, int, bool);
	virtual bool							GetResetCombatLOSBuffer(int, Actor* apTarget, int);
	virtual UInt32							GetDetectionValue(Actor* apTarget, int);
	virtual bool							GetIsInDialogMenu() const;
	virtual void							SetInDialogMenu(bool abInDialog);
	virtual void							Unk_B8();
	virtual int								Unk_B9(Actor*, Actor*);
	virtual int								Unk_BA() const;
	virtual void							Unk_BB(int);
	virtual void							HandleRunDetection();
	virtual double							GetDetectionTimer();
	virtual void							DecreaseDetectionTimer();
	virtual void							SetDiveBreath(float afVal);
	virtual float							GetDiveBreath() const;
	virtual void							Unk_C1();
	virtual void							Unk_C2();
	virtual bool							GetGreetingFlag() const;
	virtual void							SetGreetingFlag(bool);
	virtual float							Unk_C5() const;
	virtual void							Unk_C6(float);
	virtual bool							GetAlerted() const;
	virtual void							SetAlerted(bool abAlert);
	virtual bool							GetIgnoringCombat();
	virtual void							SetIgnoringCombat(bool);
	virtual ItemChange*						CreateObjectAquireList(Actor* apActor, UInt32 aePackageObjectType);
	virtual void							ClearObjectList();
	virtual float							GetIdleTimer() const;
	virtual void							SetIdleTimer(float afValue);
	virtual bool							EnterCombat(Actor* apActor, Actor* apTarget, bool, bool, int, bool, bool, bool, bool, bool, bool, bool, int);
	virtual void							FindDoorsToSendYellAlarm(Actor* apActor, Actor* apTarget);
	virtual void							CallForHelp(Actor* apActor, Actor* apTarget, Crime* apCrime);
	virtual void							SetNumberGuardsArresting(SInt32 aiVal);
	virtual void							CalculateMoveMode(Actor* apActor, float, float, float, bool, bool);
	virtual void							SetActorsAnimation(Actor* apActor, SInt32, bool);
	virtual void							FinishDying(Actor* apActor);
	virtual void							Unk_D6();
	virtual bool							IsRunningRunOnce();
	virtual bool							IsAFollower() const;
	virtual void							SetFollower(bool abVal);
	virtual void							GetEquippedWeaponHealthBracket();
	virtual bool							GetByte3D0() const;
	virtual void							SetByte3D0(bool);
	virtual void							SetByte3D1(bool);
	virtual bool							GetByte3D1() const;
	virtual void*							GetLipSyncAnim() const;
	virtual void							SetLipSyncAnim(void*);
	virtual void							SetHasLiveGrenade(bool abVal);
	virtual bool							GetHasLiveGrenade() const;
	virtual TESIdleForm*					GetLastIdlePlayed();
	virtual void							SetForcedIdle(TESIdleForm* idleForm);
	virtual void							StopIdle();
	virtual SInt32							GetActorValueI(TESActorBase* apBase, ActorValue::Index aeIndex, Actor* apActor); // Names could be wrong
	virtual float							GetActorValueF(TESActorBase* apBase, ActorValue::Index aeIndex, Actor* apActor);
	virtual void							GameModActorValueF(Actor* apActor, ActorValue::Index aeIndex, float afVal);
	virtual void							GameModActorValueI(Actor* apActor, ActorValue::Index aeIndex, SInt32 aiVal);
	virtual void							SetActorValueF(Actor* apActor, ActorValue::Index aeIndex, float afVal);
	virtual void							SetActorValueI(Actor* apActor, ActorValue::Index aeIndex, SInt32 aiVal);
	virtual void							Unk_EC(UInt32 avCode);
	virtual void							Unk_ED();
	virtual BSSimpleList<ActiveEffect*>*	GetActiveEffectList() const;
	virtual void							Unk_EF();
	virtual void							CastAbility(MagicCaster* apCaster, SpellItem* apAbility, bool abLoadCast);
	virtual void							TransferDisease(MagicCaster* apCaster, SpellItem* apDisease, MagicTarget* apTarget, bool abLoadCast);
	virtual MagicItem*						GetCurrentSpell() const;
	virtual void							SetCurrentSpell(MagicItem* apSpell);
	virtual MagicTarget*					GetDesiredTarget() const;
	virtual void							SetDesiredTarget(MagicTarget* apTarget);
	virtual void							GetByte168();
	virtual void							Unk_F7();
	virtual void							Unk_F8();
	virtual SInt16							GetCurrentAction() const;
	virtual BSAnimGroupSequence*			GetCurrentSequence() const;
	virtual void							SetAnimAction(UInt32 aeAnimationAction, BSAnimGroupSequence* apSequence);
	virtual void							GetForceFireWeapon();
	virtual void							SetForceFireWeapon(char);
	virtual bool							IsReadyForAnim();
	virtual void							Unk_FF();
	virtual void							SetIsAiming(bool isAiming);
	virtual bool							IsAiming() const;
	virtual void							Unk_102();
	virtual SInt32							GetKnockedState();
	virtual void							SetKnockedState(char state);
	virtual void							Unk_105();
	virtual void							PushActorAway(Actor* apActor, NiPoint3 akPos, float afForce);
	virtual void							UpdateKnockState();
	virtual void							Unk_108();
	virtual void							SetWeaponSequence(UInt32 auiIndex, BSAnimGroupSequence* apSequence);
	virtual void							Unk_10A();
	virtual float							GetAnimChaseLookAngle();
	virtual void							ModifyAnimChaseLookAngle(float afValue);
	virtual void							Unk_10D();
	virtual void							Unk_10E();
	virtual void							SetByte1D8();
	virtual bool							GetByte1D8();
	virtual void							Unk_111();
	virtual void							Unk_112();
	virtual bool							WantsWeaponOut();
	virtual void							SetWantsWeaponOut(bool);
	virtual bool							IsWeaponOut() const;
	virtual void							SetWeaponOut(Actor* actor, bool weaponOut);
	virtual void							Unk_117();
	virtual void							Unk_118();
	virtual void							Unk_119(Actor* actor);
	virtual void							Set3DUpdateFlag(UInt32 unk);
	virtual void							Unk_11B();
	virtual void							Unk_11C();
	virtual bool							GetFlag18C(UInt32 arg);
	virtual void							Unk_11E();
	virtual void							Unk_11F();
	virtual void							ClearObjectFromAcquireList(UInt32 auiFormID);
	virtual TESObjectREFR*					Get_UnkRef30C() const;
	virtual void							Set_UnkRef30C(TESObjectREFR* apRef);
	virtual BSSoundHandle*					GetSoundHandle(BSSoundHandle& arHandle, UInt32 aeType);
	virtual void							SetSoundHandle(UInt32 aeType, BSSoundHandle& arHandle);
	virtual void							StopSoundHandle(UInt32 aeType);
	virtual void							StartTorchSound();
	virtual void							Unk_127();
	virtual void							Unk_128();
	virtual void							SetDetectionModifier();
	virtual void							SetDetectionModifierTimer();
	virtual double							GetDetectionModifier();
	virtual void							GetDyingTimer();
	virtual void							SetDyingTimer();
	virtual void							IsTalking();
	virtual int								GetSitSleepState();
	virtual void							SetFurnitureRef();
	virtual void							GetFurnitureMarkerID();
	virtual void							GetCurrentFurnitureRef();
	virtual void							RemoveFurnitureRef(TESObjectREFR*);
	virtual UInt32							GetMarkerIndex();
	virtual FurnitureMark*					GetFurnitureMark();
	virtual bool							LoadIdleFurnitureAnimation(Actor* apActor);
	virtual void							Unk_137(Actor* actor, Animation* apAnimation);
	virtual bool							GetIsContinuingPackagePCNear() const;
	virtual void							SetIsContinuingPackagePCNear(bool abVal);
	virtual bool							GetTargetActivated() const;
	virtual void							SetTargetActivated();
	virtual bool							HasBeenAttacked() const;
	virtual void							SetBeenAttacked(bool abVal);
	virtual BSShaderPPLightingProperty*		GetLightingShaderProperty() const;
	virtual void							SetLightingShaderProperty(BSShaderPPLightingProperty* apProp);
	virtual void							HandleLightUpdateTimer(Actor* apActor, bool abIgnoreTorch);
	virtual DetectionData*					GetDetectionData(Actor* target, UInt32 detecting);
	virtual void							Unk_142();
	virtual double							GetLightUpdateTimer() const;
	virtual void							SetLightUpdateTimer(float afVal);
	virtual TESObjectREFR*					GetGenericLocation() const;
	virtual void							SetGenericLocation(TESObjectREFR* apLocation);
	virtual TESObjectREFR*					GetSecondLocation() const;
	virtual void							SetSecondLocation(TESObjectREFR* apLocation);
	virtual void							ClearCurrentDataforProcess();
	virtual void							SetCommandingActor(Actor* apActor);
	virtual Actor*							GetCommandingActor() const;
	virtual void							SetUnk15C(void*);
	virtual void*							GetUnk15C() const;
	virtual void							SetScriptPackageEndTime(float afVal);
	virtual float							GetScriptPackageEndTime() const;
	virtual void							SetFurniture(TESObjectREFR* apFurnitureRef, UInt8 aucMarkerIndex, FurnitureMark* apMark);
	virtual void							RemoveAndAttachItems(MobileObject* apObject);
	virtual UInt16							GetSaveSize(UInt32, MobileObject* apObject);
	virtual void							SaveGameBGS(BGSSaveGameBuffer* apBuffer);
	virtual void							SaveGameTES(UInt32, MobileObject*);
	virtual void							LoadGameBGS(BGSLoadFormBuffer* apBuffer);
	virtual void							LoadGameTES(UInt32, UInt32, MobileObject* apObject);
	virtual void							InitLoadGameBGS(BGSLoadFormBuffer* apBuffer);
	virtual void							InitLoadGameTES(UInt32, UInt32, MobileObject* apObject);
	virtual void							FinishInitLoadGame(UInt32, UInt32, MobileObject* apObject);
	virtual void							RevertBGS(BGSLoadFormBuffer* apBuffer);
	virtual void							RevertTES(UInt32, MobileObject* apObject);
	virtual void							FinishLoadGame();
	virtual TESEffectShader					GetWeaponEnchantmentVisuals() const;
	virtual void							SetWeaponEnchantmentVisuals(TESEffectShader* apShader);
	virtual void							GetByte17C();
	virtual void							Unk_160();
	virtual void							Unk_161(Actor* apActor);
	virtual void							SetByte19C(bool abVal);
	virtual bool							GetByte19C() const;
	virtual void							SetByte19D(UInt8 aucVal);
	virtual UInt8							GetByte19D() const;
	virtual void							OnPackageStart(Actor *apActor, TESPackage *apPackage);
	virtual TESPackage*						OnPackageChange(Actor *apActor, TESPackage *apPackage);
	virtual void							OnPackageEnd(Actor *apActor, TESPackage *apPackage);
	virtual bool							Unk_169() const;
	virtual void							Unk_16A(bool);
	virtual float							GetActorAlpha() const;
	virtual void							SetActorAlpha(float afVal);
	virtual void							GetUnk174();
	virtual void							SetUnk174();
	virtual void							Unk_16F();
	virtual SInt32							GetNumberGuardsArresting() const;
	virtual void							ModNumberGuardsArresting(SInt32 aiVal);
	virtual bool							ShouldCheckFlare(BSSimpleList<ActiveEffect*>* apEffects, UInt32 aeFlareType);
	virtual void							SetRefreshFlareFlags();
	virtual void							Unk_174();
	virtual void							SetUnk3AC(void*);
	virtual void*							GetUnk3AC() const;
	virtual void							SetUnk3B0(void*);
	virtual void*							GetUnk3B0() const;
	virtual void							Unk_179();
	virtual void							Unk_17A();
	virtual void							Unk_17B();
	virtual BSBound*						GetBound() const;
	virtual void							SetBound(BSBound* apBound);
	virtual void							SetForceNextUpdate(bool abVal);
	virtual bool							GetForceNextUpdate() const;
	virtual void							SetForceSpeakToPlayerAfterGettingUp(bool abVal);
	virtual bool							IsForceSpeakToPlayerAfterGettingUp() const;
	virtual void							Unk_182();
	virtual void							Unk_183();
	virtual UInt32							GetFadeState() const;
	virtual void							SetQueuedIdleFlags(UInt32 auiFlag);
	virtual UInt32							GetQueuedIdleFlags() const;
	virtual void							ResetQueuedIdleFlags();
	virtual void							UnsetQueuedIdleFlag();
	virtual void							SetDefaultHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetActionHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetScriptHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetCombatHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetDialogHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetProcedureHeadTrackTarget(TESObjectREFR* apTarget);
	virtual void							SetHeadTrackTarget(UInt32 aeType, TESObjectREFR* apTarget);
	virtual void							ClearDefaultHeadTrackTarget();
	virtual void							ClearActionHeadTrackTarget(bool abHoldDefault);
	virtual void							ClearScriptHeadTrackTarget(bool abHoldDefault);
	virtual void							ClearCombatHeadTrackTarget();
	virtual void							ClearDialogHeadTrackTarget(bool abHoldDefault);
	virtual void							ClearProcedureHeadTrackTarget(bool abHoldDefault);
	virtual void							UpdateDetectionModifierTimer();
	virtual float							GetDetectionModifierTimer();
	virtual void							ResetHeadTrackingTarget();
	virtual void							RemoveHeadTrackTarget(TESObjectREFR* apTarget);
	virtual bool							CanSetDefaultHeadTrackTarget();
	virtual bool							CanSetActionHeadTrackTarget();
	virtual bool							CanSetDialogHeadTrackTarget();
	virtual TESObjectREFR*					GetCurrentHeadTrackTargetByType(UInt32 aeType);
	virtual TESObjectREFR*					GetCurrentHeadTrackTarget();
	virtual UInt32							GetCurrentHeadTrackType();
	virtual const char*						GetCurrentHeadTrackTypeString();
	virtual void							SetByte420(bool);
	virtual bool							BetByte420();
	virtual TESObjectREFR*					GetPackageTarget();
	virtual void							Unk_1A4();
	virtual void							Unk_1A5();
	virtual void							Unk_1A6();
	virtual float							GetGameDayDied();
	virtual void							Unk_1A8();
	virtual void							AddArrowProjectile();
	virtual void							Unk_1AA();
	virtual void							Unk_1AB();
	virtual void							Unk_1AC();
	virtual void							CreateMuzzleFlash(Projectile* apProjectile, Actor* apShooter);
	virtual MuzzleFlash*					GetMuzzleFlash();
	virtual void							Unk_1AF();
	virtual void							Unk_1B0();
	virtual void							DestroyMuzzleFlash();
	virtual void							Unk_1B2();
	virtual void							SetNthLimbNode(NiNode* apNode);
	virtual NiNode*							GetNthLimbNode() const;
	virtual NiNode*							GetHairBodyPartNode() const;
	virtual NiNode*							GetHeadBodyPartNode() const;
	virtual bool							IsFiringAutomaticWeapon();
	virtual void							SetIsFiringAutomaticWeapon(bool);
	virtual void							UpdateIsWearingHeavyArmor();
	virtual void							IsWearingHeavyArmor();
	virtual bool							IsWearingPowerArmorTorso();
	virtual bool							IsWearingPowerArmorHelmet();
	virtual bool							IsWearingBackpack();
	virtual void							SetByte3B9(bool);
	virtual bool							GetByte3B9();
	virtual void							UpdateRadiation(Actor*, float);
	virtual void							Unk_1C1();
	virtual void							Unk_1C2();
	virtual void							PlayCrippledLimbAnim();
	virtual void							SavePackageToExtraData(TESObjectREFR* apRef);
	virtual void							LoadPackageFromExtraData(TESObjectREFR* apRef);
	virtual TESIdleForm*					GetForcedIdleForm() const;
	virtual void							SetForcedIdleForm(TESIdleForm* idleForm);
	virtual void							SetKFModelAndObject(NiRefObject* apObject, KFModel* apModel); // What are those models exactly?
	virtual NiRefObject*					GetUnkObject() const;
	virtual KFModel*						GetUnkKFModel() const;
	virtual void							AddPlayerDamageDealt();
	virtual void							GetPlayerHealthDamageDealt();
	virtual float							GetLightAmount();
	virtual void							SetLightAmount(float lightAmount);
	virtual void							AddDefferedHideLimb(UInt32 auiLimbID, NiNode* apLimbNode, NiNode* apReplacementNode, bool abExplosion);
	virtual void							UpdateDeferredHideDismemberedLimbs(Actor* apActor, float afDeltaTime);
	virtual void							SetLipSyncAnim1A0(void*);
	virtual void*							GetLipSyncAnim1A0() const;
	virtual void							Unk_1D3(void*);
	virtual void*							Unk_1D4() const;
	virtual void							ModEffectRads(float afVal);
	virtual void							SubEffectRads(float afVal);
	virtual float							GetEffectRads() const;
	virtual void							SetWaterRadsSec(float afVal);
	virtual float							GetWaterRadsSec() const;
	virtual void							SetUnk234();
	virtual float							GetRadsSec();
	virtual ActorHitData*					GetHitData();
	virtual void							CopyHitData(ActorHitData* hitData);
	virtual void							ResetHitData();
	virtual ActorHitData*					GetLastTargetHitData();
	virtual void							CopyLastTargetHitData(ActorHitData* hitData);
	virtual void							ResetLastTargetHitData();
	virtual void							GetUnk244();
	virtual void							SetUnk0244();
	virtual BSFaceGenNiNode*				GetFaceGenNiNodeBiped();
	virtual void							SetFaceGenNiNodeBiped(BSFaceGenNiNode* apNode);
	virtual BSFaceGenNiNode*				GetFaceGenNiNodeSkinned();
	virtual void							SetFaceGenNiNodeSkinned(BSFaceGenNiNode* apNode);
	virtual NiTriShape*						GetHeadAnims();
	virtual void							SetHeadAnims(NiTriShape* apShape);
	virtual void							Unk_1EA();
	virtual void							SetPathLookAtTarget(TESObjectREFR* apTarget);
	virtual void							Unk_1EC();


	ActorPackage	kCurrentPackage;
	float			unk1C_maybeUnused;
	float			fHourLastProcessed;
	SInt32			iDateLastProcessed;	
	UInt32			uiProcessLevel;
	CachedValues*	pCachedValues;

	// 0x8ACED0
	__forceinline bool GetCachedFlag(UInt32 auiFlag) const {
		return pCachedValues && pCachedValues->uiFlags.IsSet(auiFlag);
	}

	__forceinline void SetCachedFlag(UInt32 auiFlag, bool abSet) {
		if (pCachedValues)
			pCachedValues->uiFlags.SetBit(auiFlag, abSet);
	}
};

ASSERT_SIZE(BaseProcess, 0x30);