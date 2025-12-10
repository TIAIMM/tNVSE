#pragma once

#include "Character.hpp"
#include "BSSimpleList.hpp"
#include "TESRegion.hpp"
#include "TESSound.hpp"
#include "TeleportPath.hpp"
#include "BSSoundHandle.hpp"
#include "TESMain.hpp"
#include "TESRegionSound.hpp"
#include "TESRegionList.hpp"

class AlchemyItem;
class BGSNote;
class BGSQuestObjective;
class bhkMouseSpringAction;
class CameraCaster;
class CombatGroup;
class DialoguePackage;
class ImageSpaceModifierInstanceDOF;
class ImageSpaceModifierInstanceDRB;
class MagicItem;
class MagicShaderHitEffect;
class MagicTarget;
class MusicMarker;
class QuestObjectiveTargets;
class TESCaravanCard;
class TESClass;
class TESEnchantableForm;
class TESObjectCELL;
class TESObjectREFR;
class TESObjectWEAP;
class TESQuest;
class TESReputation;
class TESTopic;
class TESWorldspace;
class Animation;
class BipedAnim;

class PlayerCharacter : public Character {
public:
	PlayerCharacter();
	virtual ~PlayerCharacter();

	struct WobbleNodes
	{
		NiNode* pWobbleAnimNodes[12];
		NiNode* pWobbleAnimNodes2[12];
	};

	struct LevelupInfo {
		bool	bShouldLevelUp;
		UInt32	uiXPForNextLevel;
		bool	byte08;
	};

	struct CompassTarget {
		CompassTarget() {};
		~CompassTarget() {};

		Actor*	pTarget			= nullptr;
		bool	bIsHostile		= false;
		bool	bIsUndetected	= false;
	};

	struct PositionRequest {
		TESWorldspace*	pWorldspace;
		TESObjectCELL*	pCell;
		NiPoint3		kPos;
		NiPoint3		kRot;
		bool			bResetWeather;
		void(__cdecl*	pfnCallbackFunc)(void*);
		void*			pCallbackFuncArg;
		TESObjectREFR*	pDestRef;
		TESObjectREFR*	pFastTravelRef;
	};

	struct FlyCamData {
		float		fRotZ;
		float		fRotX;
		NiPoint3	kPosition;
	};

	enum GrabMode {
		kGrabMode_ZKey = 0x1,
		kGrabMode_Telekenesis = 0x3,
	};

	// used to flag controls as disabled in disabledControlFlags
	enum {
		kControlFlag_Movement = 1 << 0,
		kControlFlag_Look = 1 << 1,
		kControlFlag_Pipboy = 1 << 2,
		kControlFlag_Fight = 1 << 3,
		kControlFlag_POVSwitch = 1 << 4,
		kControlFlag_RolloverText = 1 << 5,
		kControlFlag_Sneak = 1 << 6,
	};

	enum KillcamMode {
		kKillcamMode_None = 0x0,
		kKillcamMode_PlayerView = 0x1,
		kKillcamMode_Cinematic = 0x2,
	};

	virtual void		Unk_139();
	virtual void		Unk_13A();

	// lotsa data

	UInt32								unk1C8;
	UInt32								unk1CC;
	UInt32								unk1D0;
	UInt32								unk1D4;
	UInt32								unk1D8;
	UInt32								unk1DC;
	UInt32								unk1E0;
	UInt32								unk1E4;
	UInt32								unk1E8;
	PositionRequest*					pPositionRequest;
	TESObjectWEAP*						pWeap1F0;
	float								fTime1F4;
	UInt8								byte1F8;
	UInt32								sandmanDetectionValue;
	UInt32								unk200;
	UInt8								byte204;
	UInt8								byte205;
	bool								bQuestTargetsNeedRecalculated;
	UInt8								byte207;
	DialoguePackage*					pClosestConversation;
	bool								bIsPlayerMovingIntoNewSpace;
	void*								pActiveEffects;
	MagicItem*							pMagicItem214;
	MagicTarget*						pMagicTarget218;
	CameraCaster*						pCameraCaster;
	UInt32								unk220;
	UInt32								unk224;
	UInt32								unk228;
	float								fTime22C;
	UInt32								unk230;
	float								time234;
	BSSimpleList<UInt32>*				list238;
	BSSimpleList<TESEnchantableForm*>*	pEnchantmentList23C;
	bool								bShowQuestItems;
	UInt8								byte241;
	UInt8								byte242;
	UInt8								byte243;
	float								fMagicModifiers[77];
	float								fPermAVMods[77];
	float								fHealthAV_4AC;
	float								fActorValues4B0[77];
	BSSimpleList<UInt32>				kNotes;
	ImageSpaceModifierInstanceDOF*		pIronSightsDOFInstance;
	ImageSpaceModifierInstanceDOF*		pVATSDOFInstance;
	ImageSpaceModifierInstanceDRB*		pVATSDRBInstance;
	bool								bIsDetected;
	bool								bPreventRegionSoundUpdates;
	bool								byte5FA;
	bool								byte5FB;
	BSSimpleList<UInt32>				kTeammates;
	TESObjectREFR*						pLastExteriorDoor;
	bool								bIsPlayingCombatAttackSound;
	void*								pActionList;
	BSSimpleList<UInt32>*				pCasinoDataList;
	BSSimpleList<TESCaravanCard*>*		pCaravanCards1;
	BSSimpleList<TESCaravanCard*>*		pCaravanCards2;
	UInt32								uiCaravanCapsEarned;
	UInt32								uiCaravanCapsLost;
	UInt32								uiNumCaravanWins;
	UInt32								uiNumCaravanLosses;
	UInt32								uiLargestCaravanWin;
	UInt32								unk630;
	bhkMouseSpringAction*				pGrabSpringAction;
	TESObjectREFR*						pGrabbedRef;
	GrabMode							eGrabType;
	float								fCurrentGrabbedItemWeight;
	float								fGrabDistance;
	UInt8								byte648;
	UInt8								byte649;
	bool								bIs3rdPersonVisible;
	bool								bIs3rdPerson;
	bool								bThirdPerson;
	bool								bShouldRestoreFirstPersonAfterVanityMode;
	bool								bWasFirstPersonBeforeVanityCam;
	bool								bIsForceFirstPerson;
	bool								byte650;
	bool								bAlwaysRun;
	bool								bAutoMove;
	bool								byte653;
	UInt32								uiSleepHours;
	bool								bIsResting;
	bool								byte659;
	bool								byte65A;
	bool								byte65B;
	float								unk65C;
	float								fTime660;
	float								fTime664;
	float								fUnused_time668;
	bool								bIsActorWithin_iSneakSkillUseDistance;
	UInt8								flag66D;
	bool								bCanSleepWait;
	UInt8								byte66F;
	float								fWorldFOV;
	float								f1stPersonFOV;
	float								fOverShoulderFOV;
	UInt32								unk67C;
	Bitfield8							cControlFlags;
	bool								bIsWaitingForOpenContainerAnim;
	bool								byte682;
	bool								byte683;
	float								fWaitingForContainerOpenAnimPreventActivateTimer;
	TESObjectREFR*						pActivatedDoor;
	BipedAnim*							pBipedAnims1st;
	Animation*							pFirstPersonArms;
	NiNodePtr							spPlayerNode;
	float								fEyeHeight;
	NiNode*								pInventoryMenu;
	Animation*							pAnimData6A0;
	MagicShaderHitEffect*				pMagicShaderHitEffect;
	BSSimpleList<UInt32>				kTopicList;
	BSSimpleList<UInt32>				kList6B0;
	TESQuest*							pActiveQuest;
	BSSimpleList<UInt32>				kQuestObjectiveList;
	BSSimpleList<UInt32>				kQuestTargetList;
	bool								bPlayerGreetFlag;
	float								fPlayerGreetTimer;
	UInt32								unk6D4;
	bool								bIsAMurderer;
	UInt32								uiAmountSoldStolen;
	float								fSortActorDistanceListTimer;
	float								fSeatedRotation;
	UInt8								byte6E8;
	UInt8								byte6E9;
	UInt8								byte6EA;
	UInt8								byte6EB;
	MagicItem*							pSelectedSpell;
	TESObjectBOOK*						pSelectedScroll;
	TESObjectREFR*						pPlacedMarker;
	TeleportPath						kPlacedMarkerTarget;
	float								fTimeGrenadeHeld;
	UInt32								unk734;
	UInt32								unk738;
	TESClass*							pClass73C_unset;
	UInt32								unk740;
	UInt32								uiCrimeCounts[5];
	AlchemyItem*						pPoisonToApply;
	bool								bInCharGen;
	UInt8								byte75D;
	UInt8								bCanUseTelekinesis;
	UInt8								byte75F;
	TESRegion*							pCurrentRegion;
	TESRegionList						kRegionsList;
	BSSimpleList<TESRegionSound*>		kRegionSounds;
	BSSoundHandle						kHeartBeatSound;
	UInt32								unk788;
	UInt32								iLastPlayingTimeUpdate;
	UInt32								iTotalPlayingTime;
	UInt32								unk794;
	bool								bIsInSleepPackage;
	bool								bIsTransitioningSeatedState;
	bool								bIsDrinkingPlacedWater;
	bool								bIsPackagePlaying;
	UInt8								byte79C;
	UInt8								byte79D;
	NiPoint3							kLastBorderRegionPosition;
	void*								pWorldOrCell;
	NiTPrimitiveArray<TESRegion>*		pRegionArray;
	void*								pMusicType;
	UInt32								uiGameDifficulty;
	bool								bIsHardcore;
	KillcamMode							eKillCamMode;
	bool								bInCombatWithGuard;
	bool								bIsYoung;
	bool								bIsToddler;
	bool								bCanUsePA;
	BSSimpleList<UInt32>				kMapMarkers;
	void*								pWorldspaceForMapMarkerUpdates;
	BSSimpleList<MusicMarker*>			kMusicMarkers;
	MusicMarker*						pCurrMusicMarker;
	FlyCamData							kFlycamPos;
	UInt32								unk7F4;
	BSSoundHandle						kMagicCastSound;
	BSSoundHandle						unk808[6];
	BSSimpleList<TESObjectREFR*>		kDroppedRefList;
	NiTMap<UInt32, UInt8>				unk858;
	UInt32								unk868;
	UInt32								unk86C;
	bool								bHasShownNoChargeWarning;
	float								fDropAngleMod;
	float								fLastDropAngleMod;
	LevelupInfo*						pLevelUpInfo;
	BSSimpleList<UInt32>				kPerkRanksPC;
	BSSimpleList<UInt32>				kPerkEntriesPC[74];
	BSSimpleList<UInt32>				kPerkRanksTM;
	BSSimpleList<UInt32>				kPerkEntriesTM[74];
	Actor*								pAutoAimActor;
	NiPoint3							kBulletAutoAim;
	NiNodePtr							spAutoAimNode;
	bool								bIsActorWithinInterfaceManagerPickDistance;
	Actor*								pReticleActor;
	BSSimpleList<CompassTarget*>*		pCompassTargets;
	float								fPipboyLightHeldTime;
	float								fAmmoSwapTimer;
	bool								bShouldOpenPipboy;
	char								byteD55;
	char								byteD56;
	char								byteD57;
	NiPoint3							kPtD58;
	CombatGroup*						pCombatGroup;
	UInt32								uiTeammateCount;
	float								fCombatYieldTimer;
	float								fCombatYieldRetryTimer;
	WobbleNodes							kWobbleAnims;
	NiPoint3							kVectorDD4;
	NiPoint3							kCameraPos;
	bhkRigidBody*						pRigidBody;
	bool								bInCombat;
	bool								bUnseen;
	bool								bIsLODApocalypse;
	bool								byteDF3;
	BSSimpleArray<UInt32>				kRockitLauncherContainer;
	float								fRockItLauncherWeight;
	bool								bHasNightVisionApplied;
	char								byteE09;
	char								byteE0A;
	char								byteE0B;
	TESReputation*						pModifiedReputation;
	int									unkE10;
	int									unkE14;
	float								fKillCamTimer;
	float								fKillCamCooldown;
	char								byteE20;
	char								bIsUsingTurbo;
	char								byteE22;
	char								byteE23;
	float								fLastHelloTime;
	float								fCounterAttackTimer;
	char								byteE2C;
	bool								bIsCateyeEnabled;
	bool								bIsSpottingImprovedActive;
	char								byteE2F;
	float								fItemDetectionTimer;
	NiNode*								pIronSightNode;
	bool								bNoHardcoreTracking;
	bool								bSkipHCNeedsUpdate;
	char								byteE3A;
	char								byteE3B;
	BSSimpleArray<UInt32>				kHotkeyedWeaponAmmos;
	TESQuest*							pPatch04DebugQuest;

	bool IsThirdPerson() const { return bThirdPerson ? true : false; }
	UInt32 GetMovementFlags() const { return pActorMover->GetMovementFlags(); }	// 11: IsSwimming, 9: IsSneaking, 8: IsRunning, 7: IsWalking, 0: keep moving
	bool IsPlayerSwimming() const { return (GetMovementFlags() >> 11) & 1; }
	bool IsSleepingOrResting() const { return uiSleepHours > 0; }

	static PlayerCharacter* GetSingleton() {
		return *reinterpret_cast<PlayerCharacter**>(0x011DEA3C);
	}

	QuestObjectiveTargets* GetCurrentQuestObjectiveTargets();

	bool HasPipBoyOpen() const;

	static NiCamera* GetCamera1st();
	static NiCamera* GetCamera3rd();

	static NiCamera* GetVanityCamera();

	bool IsVanityMode() const;

	bool IsSleepingorResting() const;

	NiNode* GetNode(const bool abFirstPerson) const;

	void UpdatePlayerControlsMask(bool abEnable, UInt32 aMask);

	void UpdateImageSpaceIronsights();

	bool CenterOnCell(const char* apName, TESObjectCELL* apCell = nullptr);

	CompassTarget* GetCompassTargetForActor(Actor* apActor) const;
	void AddCompassTarget(Actor* apActor, bool abIsHostile, bool abIsUndetected);
	void RemoveCompassTarget(Actor* apActor);

	void EnableFlyCam() {
		if (TESMain::GetSingleton()->bIsFlyCam) {
			return;
		}

		ThisStdCall(0x961E30, this, false);
	}

	void DisableFlyCam() {
		if (!TESMain::GetSingleton()->bIsFlyCam) {
			return;
		}

		ThisStdCall(0x961E30, this, false);
	}

	void SetFlyCamPos(const NiPoint3 &pos) {
		kFlycamPos.kPosition = pos;
	}

	void SetFlyCamRot(const NiPoint3 &angle) {
		kFlycamPos.fRotX = angle.x;
		kFlycamPos.fRotZ = angle.z;
	}

	NiPoint3 GetFlyCamPos() {
		return kFlycamPos.kPosition;
	}

	NiPoint3 GetFlyCamRot() {
		return { kFlycamPos.fRotX, 0.f, kFlycamPos.fRotZ };
	}

	NiPoint3 GetCameraRot() {
		const auto pitch = *reinterpret_cast<float*>(0x11E0764);
		const auto yaw = *reinterpret_cast<float*>(0x11E076C);

		return { pitch, 0.f, yaw };
	}

	void SetCameraRot(const NiPoint3 &rot) {
		*reinterpret_cast<float*>(0x11E0764) = rot.x;
		*reinterpret_cast<float*>(0x11E076C) = rot.z;
	}

	float GetAttacksPerSecond();
};

ASSERT_OFFSET(PlayerCharacter, kFlycamPos, 0x7E0);
ASSERT_OFFSET(PlayerCharacter, fLastDropAngleMod, 0x874);
ASSERT_OFFSET(PlayerCharacter, fKillCamTimer, 0xE18);
ASSERT_SIZE(PlayerCharacter, 0xE50);