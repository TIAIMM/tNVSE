#pragma once

#include "TESForm.hpp"
#include "TESPackageData.hpp"

class Script;
class BGSIdleCollection;
class TESCombatStyle;
class TESObjectREFR;
class TESIdleForm;
class TESTopic;
class Actor;
class NiPoint3;

union PackageObject {
	TESForm*		pForm;
	TESObjectREFR*	pReference;
	UInt32			uiObjectCode;
};

struct PackageSchedule {
	enum
	{
		kDay_Any = 0,
		kTime_Any = 0xFF,
	};

	enum
	{
		kMonth_January = 0,
		kMonth_February,
		kMonth_March,
		kMonth_April,
		kMonth_May,
		kMonth_June,
		kMonth_July,
		kMonth_August,
		kMonth_September,
		kMonth_October,
		kMonth_November,
		kMonth_December,
		kMonth_Spring,	// march, april, may
		kMonth_Summer,	// june, july, august
		kMonth_Autumn,	// september, august, november (in Geck)
		kMonth_Winter,	// december, january, february

		kMonth_Any = 0xFF,
	};

	enum
	{
		kWeekday_Sundays = 0,
		kWeekday_Morndays,
		kWeekday_Tuesdays,
		kWeekday_Wednesdays,
		kWeekday_Thursdays,
		kWeekday_Frydays,
		kWeekday_Saturdays,
		kWeekday_Weekdays,
		kWeekday_Weekends,
		kWeekday_MWF,
		kWeekday_TT,

		kWeekday_Any = 0xFF
	};

	SInt8	eMonth;
	SInt8	eDayOfWeek;
	SInt8	cDate;
	SInt8	cHour;
	SInt32	iDuration;

	static bool IsValidMonth(UInt8 m) { return (m + 1) <= kMonth_Winter; }
	static bool IsValidTime(UInt8 t) { return (t + 1) <= 24; }
	static bool IsValidDay(UInt8 d) { return (d + 1) <= kWeekday_TT; }
	static bool IsValidDate(UInt8 d) { return d <= 31; }
};

ASSERT_SIZE(PackageSchedule, 0x8);

struct PackageLocation {
	enum {
		kPackLocation_NearReference = 0,
		kPackLocation_InCell = 1,
		kPackLocation_CurrentLocation = 2,
		kPackLocation_EditorLocation = 3,
		kPackLocation_ObjectID = 4,
		kPackLocation_ObjectType = 5,
		kPackLocation_LinkedReference = 6,

		kPackLocation_Max,
	};

	UInt8			ucLocationType;
	UInt32			uiRadius;
	PackageObject	uObject;
};

ASSERT_SIZE(PackageLocation, 0xC);

struct PackageTarget {
	UInt8			ucTargetType;
	PackageObject	uObject;
	SInt32			iValue;
	float			unk0C;
};

ASSERT_SIZE(PackageTarget, 0x10);

struct PackageEventAction {
	TESIdleForm*	pIdle;
	Script*			pScript;
	TESTopic*		pTopic;
	UInt32			eType;
};

ASSERT_SIZE(PackageEventAction, 0x10);

class TESPackage : public TESForm {
public:
	TESPackage();
	~TESPackage();

	virtual bool	Unk_4E(void*);
	virtual bool	IsActorAtLocation(Actor* apActor, bool abIgnoreDistance, float afExtraRadius, bool abInFurniture);
	virtual bool	IsActorAtSecondLocation(Actor* apMobileObject, Actor* apPackageowner, bool abIgnoreDistance, float afExtraRadius, bool abInFurniture);
	virtual bool	IsActorAtRefTarget(Actor* apActor, SInt32 aiExtraRadius);
	virtual bool	IsTargetAtLocation(Actor* apActor, SInt32 aiExtraRadius);
	virtual UInt16	GetSaveSize();
	virtual UInt16	SaveGame();
	virtual UInt16	LoadGame();
	virtual void	Unk_56();
	virtual bool	IsPackageOwner(Actor* apActor = nullptr);

	enum
	{
		kPackageFlag_OffersServices = 1 << 0,
		kPackageFlag_MustReachLocation = 1 << 1,
		kPackageFlag_MustComplete = 1 << 2,
		kPackageFlag_LockDoorsAtStart = 1 << 3,
		kPackageFlag_LockDoorsAtEnd = 1 << 4, 	// set by CHANGE_PACKAGE_WAITING ?
		kPackageFlag_LockDoorsAtLocation = 1 << 5,
		kPackageFlag_UnlockDoorsAtStart = 1 << 6,
		kPackageFlag_UnlockDoorsAtEnd = 1 << 7,
		kPackageFlag_UnlockDoorsAtLocation = 1 << 8,
		kPackageFlag_ContinueIfPCNear = 1 << 9,
		kPackageFlag_OncePerDay = 1 << 10,
		kPackageFlag_Unk11 = 1 << 11,
		kPackageFlag_SkipFalloutBehavior = 1 << 12,
		kPackageFlag_AlwaysRun = 1 << 13,
		kPackageFlag_Unk14 = 1 << 14,
		kPackageFlag_NeverRun = 1 << 15,	// Save only ?
		kPackageFlag_Unk16 = 1 << 16,
		kPackageFlag_AlwaysSneak = 1 << 17,
		kPackageFlag_AllowSwimming = 1 << 18,
		kPackageFlag_AllowFalls = 1 << 19,
		kPackageFlag_ArmorUnequipped = 1 << 20,
		kPackageFlag_WeaponsUnequipped = 1 << 21,
		kPackageFlag_DefensiveCombat = 1 << 22,
		kPackageFlag_WeaponsDrawn = 1 << 23,
		kPackageFlag_NoIdleAnims = 1 << 24,
		kPackageFlag_PretendInCombat = 1 << 25,
		kPackageFlag_ContinueDuringCombat = 1 << 26,
		kPackageFlag_NoCombatAlert = 1 << 27,
		kPackageFlag_NoWarnAttackBehavior = 1 << 28,
		kPackageFlag_Unk29 = 1 << 29,
		kPackageFlag_Unk30 = 1 << 30,
		kPackageFlag_Unk31 = 1 << 31
	};

	enum Type {
		EXPLORE				= 0,
		FOLLOW				= 1,
		ESCORT				= 2,
		EAT					= 3,
		SLEEP				= 4,
		WANDER				= 5,
		TRAVEL				= 6,
		ACCOMPANY			= 7,
		USE_ITEM_AT			= 8,
		AMBUSH				= 9,
		FLEE_NON_COMBAT		= 10,
		CAST_MAGIC			= 11,
		SANDBOX				= 12,
		PATROL				= 13,
		GUARD				= 14,
		DIALOGUE			= 15,
		USE_WEAPON			= 16,
		FIND				= 17,
		COMBAT				= 18,
		ALARM				= 21,
		FLEE				= 22,
		PTYPE_TRESPASS		= 23,
		SPECTATOR			= 24,
		IN_GAME_DIALOGUE	= 28,
		BACKUP				= 40,

		COUNT,
	};

	enum	// From OBSE and FNVEdit
	{
		kObjectType_None = 0,
		kObjectType_Activators,
		kObjectType_Armor,
		kObjectType_Books,
		kObjectType_Clothing,
		kObjectType_Containers,
		kObjectType_Doors,
		kObjectType_Ingredients,
		kObjectType_Lights,
		kObjectType_Misc,
		kObjectType_Flora,
		kObjectType_Furniture,
		kObjectType_WeaponsAny,
		kObjectType_Ammo,
		kObjectType_NPCs,
		kObjectType_Creatures,
		kObjectType_Keys,				//	10
		kObjectType_Alchemy,
		kObjectType_Food,
		kObjectType_AllCombatWearable,
		kObjectType_AllWearable,
		kObjectType_WeaponsRanged,
		kObjectType_WeaponsMelee,
		kObjectType_WeaponsNone,
		kObjectType_ActorEffectAny,
		kObjectType_ActorEffectRangeTarget,
		kObjectType_ActorEffectRangeTouch,
		kObjectType_ActorEffectRangeSelf,
		kObjectType_ActorsAny,

		kObjectType_Max,						//	1E
	};

	enum {
		kTargetType_Refr = 0,
		kTargetType_BaseObject = 1,
		kTargetType_TypeCode = 2,

		kTargetType_Max = 3,
	};

	enum PROCEDURE {
		PROCEDURE_TRAVEL = 0x0,
		PROCEDURE_WANDER = 0x1,
		PROCEDURE_ACTIVATE = 0x2,
		PROCEDURE_ACQUIRE = 0x3,
		PROCEDURE_SLEEP = 0x4,
		PROCEDURE_EAT = 0x5,
		PROCEDURE_FOLLOW = 0x6,
		PROCEDURE_ESCORT = 0x7,
		PROCEDURE_ALARM = 0x8,
		PROCEDURE_COMBAT = 0x9,
		PROCEDURE_FLEE = 0xA,
		PROCEDURE_YIELD = 0xB,
		PROCEDURE_DIALOGUE = 0xC,
		PROCEDURE_WAIT = 0xD,
		PROCEDURE_TRAVEL_TARGET = 0xE,
		PROCEDURE_PURSUE = 0xF,
		PROCEDURE_GREET = 0x10,
		PROCEDURE_CREATE_FOLLOW = 0x11,
		PROCEDURE_OBSERVE_COMBAT = 0x12,
		PROCEDURE_OBSERVE_DIALOGUE = 0x13,
		PROCEDURE_GREET_DEAD = 0x14,
		PROCEDURE_WARN = 0x15,
		PROCEDURE_GET_UP = 0x16,
		PROCEDURE_MOUNT_HORSE = 0x17,
		PROCEDURE_DISMOUNT_HORSE = 0x18,
		PROCEDURE_DO_NOTHING = 0x19,
		PROCEDURE_NOTIFY = 0x1A,
		PROCEDURE_ACCOMPANY = 0x1B,
		PROCEDURE_USE_ITEM_AT = 0x1C,
		PROCEDURE_FEED = 0x1D,
		PROCEDURE_AMBUSH_WAIT = 0x1E,
		PROCEDURE_SURFACE = 0x1F,
		PROCEDURE_WAIT_FOR_SPELL = 0x20,
		PROCEDURE_CHOOSE_CAST = 0x21,
		PROCEDURE_FLEE_NON_COMBAT = 0x22,
		PROCEDURE_REMOVE_WORN_ITEMS = 0x23,
		PROCEDURE_SEARCH = 0x24,
		PROCEDURE_CLEAR_MOUNT_POSITION = 0x25,
		PROCEDURE_SUMMON_CREATURE_DEFEND = 0x26,
		PROCEDURE_AVOID_RADIATION = 0x27,
		PROCEDURE_UNEQUIP_ARMOR = 0x28,
		PROCEDURE_TAKE_BACK_ITEM = 0x29,
		PROCEDURE_SANDBOX = 0x2A,
		PROCEDURE_USE_IDLE_MARKER = 0x2B,
		PROCEDURE_PATROL = 0x2C,
		PROCEDURE_EXPLOSION_REACTION = 0x2D,
		PROCEDURE_GRENADE_MINE_PICKUP_THROW = 0x2E,
		PROCEDURE_GUARD = 0x2F,
		PROCEDURE_ALERT_SEARCH = 0x30,
		PROCEDURE_DIALOGUE_ACTIVATE = 0x31,
		PROCEDURE_USE_WEAPON = 0x32,
		PROCEDURE_MOVEMENT_BLOCKED = 0x33,
		PROCEDURE_CANNIBAL_FEED = 0x34,
		PROCEDURE_BACK_UP = 0x35,
		PROCEDURE_DONE = 0x36,
		PROCEDURE_COUNT = 0x36,
	};


	struct Data {
		UInt32	eProcedureType;
		UInt32	uiPackageFlags;
		UInt8	ucType;
		UInt16	usBehaviorFlags;
		UInt16	usSpecificFlags;
	};

	Data				kData;
	TESPackageData*		pPackageData;
	PackageLocation*	pPackageLocation;
	PackageTarget*		pPackageTarget;
	BGSIdleCollection*	pIdleCollection;
	PackageSchedule		kPackSched;
	BSSimpleList<void*>	unk040;
	TESCombatStyle*		pCombatStyle;
	PackageEventAction	kOnBegin;
	PackageEventAction	kOnEnd;
	PackageEventAction	kOnChange;
	UInt32				unk07C;

	static bool IsValidObjectCode(UInt8 o) { return o < kObjectType_Max; }
	NiPoint3* GetLocationCoord(NiPoint3& arOut, Actor* apActor);

	static PROCEDURE GetProcedureAction(UInt8 aucType, UInt8 aucIndex);
	static const char* GetProcedureName(UInt8 aucProcedure);

	const char* GetName(UInt8 aucIndex) const;
};

ASSERT_SIZE(TESPackage, 0x80);