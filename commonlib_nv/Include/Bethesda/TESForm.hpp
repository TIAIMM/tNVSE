#pragma once

#include "BaseFormComponent.hpp"
#include "TESFullName.hpp"
#include "BSString.hpp"
#include "BSSimpleList.hpp"
#include "TESFile.hpp"
#include "BSTCaseInsensitiveStringMap.hpp"
#include "NiTLargeArray.hpp"

#define IS_ID(form, type) (form->ucTypeID == kFormType_##type)
#define NOT_ID(form, type) (form->ucTypeID != kFormType_##type)

class NiColor;
class BGSSaveGameBuffer;
class BGSLoadFormBuffer;
class TESObjectREFR;
class TESBoundObject;
class PlayerCharacter;

struct FORM_ENUM_STRING {
	UInt8			ucFormID;
	const char*		pFormString;
	UInt32			uiFormString;
};

struct FORM {
	UInt32 uiRecordType;
	UInt32 uiDataSize;
	UInt32 uiFormID;
	UInt32 uiFormFlags;
	UInt32 uiVersionControl;
	UInt16 usFormVersion;
	UInt16 usVCVersion;
};

struct FORM_GROUP {
	FORM	kGroupData;
	UInt32	uiGroupOffset;
};


enum FormType {
	kFormType_None = 0,					// 00
	kFormType_TES4,
	kFormType_Group,
	kFormType_GMST,
	kFormType_BGSTextureSet,
	kFormType_BGSMenuIcon,
	kFormType_TESGlobal,
	kFormType_TESClass,
	kFormType_TESFaction,					// 08
	kFormType_BGSHeadPart,
	kFormType_TESHair,
	kFormType_TESEyes,
	kFormType_TESRace,
	kFormType_TESSound,
	kFormType_BGSAcousticSpace,
	kFormType_TESSkill,
	kFormType_EffectSetting,					// 10
	kFormType_Script,
	kFormType_TESLandTexture,
	kFormType_EnchantmentItem,
	kFormType_SpellItem,
	kFormType_TESObjectACTI,
	kFormType_BGSTalkingActivator,
	kFormType_BGSTerminal,
	kFormType_TESObjectARMO,					// 18	inv object
	kFormType_TESObjectBOOK,						// 19	inv object
	kFormType_TESObjectCLOT,					// 1A	inv object
	kFormType_TESObjectCONT,
	kFormType_TESObjectDOOR,
	kFormType_IngredientItem,				// 1D	inv object
	kFormType_TESObjectLIGH,					// 1E	inv object
	kFormType_TESObjectMISC,						// 1F	inv object
	kFormType_TESObjectSTAT,					// 20
	kFormType_BGSStaticCollection,
	kFormType_BGSMovableStatic,
	kFormType_BGSPlaceableWater,
	kFormType_TESGrass,
	kFormType_TESObjectTREE,
	kFormType_TESFlora,
	kFormType_TESFurniture,
	kFormType_TESObjectWEAP,					// 28	inv object
	kFormType_TESAmmo,						// 29	inv object
	kFormType_TESNPC,						// 2A
	kFormType_TESCreature,					// 2B
	kFormType_TESLevCreature,			// 2C
	kFormType_TESLevCharacter,			// 2D
	kFormType_TESKey,						// 2E	inv object
	kFormType_AlchemyItem,				// 2F	inv object
	kFormType_BGSIdleMarker,				// 30
	kFormType_BGSNote,						// 31	inv object
	kFormType_BGSConstructibleObject,		// 32	inv object
	kFormType_BGSProjectile,
	kFormType_TESLevItem,				// 34	inv object
	kFormType_TESWeather,
	kFormType_TESClimate,
	kFormType_TESRegion,
	kFormType_NavMeshInfoMap,						// 38
	kFormType_TESObjectCELL,
	kFormType_TESObjectREFR,				// 3A
	kFormType_Character,						// 3B
	kFormType_Creature,						// 3C
	kFormType_MissileProjectile,						// 3D
	kFormType_GrenadeProjectile,						// 3E
	kFormType_BeamProjectile,						// 3F
	kFormType_FlameProjectile,						// 40
	kFormType_TESWorldSpace,
	kFormType_TESObjectLAND,
	kFormType_NavMesh,
	kFormType_TLOD,
	kFormType_TESTopic,
	kFormType_TESTopicInfo,
	kFormType_TESQuest,
	kFormType_TESIdleForm,						// 48
	kFormType_TESPackage,
	kFormType_TESCombatStyle,
	kFormType_TESLoadScreen,
	kFormType_TESLevSpell,
	kFormType_TESObjectANIO,
	kFormType_TESWaterForm,
	kFormType_TESEffectShader,
	kFormType_TOFT,						// 50	table of Offset (see OffsetData in Worldspace)
	kFormType_BGSExplosion,
	kFormType_BGSDebris,
	kFormType_TESImageSpace,
	kFormType_TESImageSpaceModifier,
	kFormType_BGSListForm,					// 55
	kFormType_BGSPerk,
	kFormType_BGSBodyPartData,
	kFormType_BGSAddonNode,				// 58
	kFormType_ActorValueInfo,
	kFormType_BGSRadiationStage,
	kFormType_BGSCameraShot,
	kFormType_BGSCameraPath,
	kFormType_BGSVoiceType,
	kFormType_BGSImpactData,
	kFormType_BGSImpactDataSet,
	kFormType_TESObjectARMA,						// 60
	kFormType_BGSEncounterZone,
	kFormType_BGSMessage,
	kFormType_BGSRagdoll,
	kFormType_DOBJ,
	kFormType_BGSLightingTemplate,
	kFormType_BGSMusicType,
	kFormType_TESObjectIMOD,					// 67	inv object
	kFormType_TESReputation,				// 68
	kFormType_ContinuousBeamProjectile,						// 69 Continuous Beam
	kFormType_TESRecipe,
	kFormType_TESRecipeCategory,
	kFormType_TESCasinoChips,				// 6C	inv object
	kFormType_TESCasino,
	kFormType_TESLoadScreenType,
	kFormType_MediaSet,
	kFormType_MediaLocationController,	// 70
	kFormType_TESChallenge,
	kFormType_TESAmmoEffect,
	kFormType_TESCaravanCard,				// 73	inv object
	kFormType_TESCaravanMoney,				// 74	inv object
	kFormType_TESCaravanDeck,
	kFormType_BGSDehydrationStage,
	kFormType_BGSHungerStage,
	kFormType_BGSSleepDeprevationStage,	// 78
	kFormType_Count
};

class TESForm : public BaseFormComponent {
public:
	static inline auto bs_rtti = RTTI_TESForm;

	TESForm();
	
	virtual					~TESForm();
	virtual void			InitializeData();
	virtual void			ClearData();
	virtual bool			Unk_07(); // NavMesh returns false, rest 1, NavMeshInfoMap does shit
	virtual bool			Load(TESFile* apFile);
	virtual bool			LoadPartial(TESFile* apFile);
	virtual bool			Save(TESFile* apFile);
	virtual void			SaveAlt();					// Writes data
	virtual bool			LoadEdit(TESFile* apFile);
	virtual bool			SaveEdit(TESFile* apFile);
	virtual bool			SavesBefore(FORM* apForm);
	virtual bool			SavesBeforeAlt(TESForm* apForm);
	virtual TESForm*		CreateDuplicateForm(bool, NiTPointerMap<TESForm*, TESForm*>*);
	virtual void			PostDuplicateProcess(NiTPointerMap<TESForm*, TESForm*>*);
	virtual void			AddChange(UInt32 auiChangedFlags);
	virtual void			RemoveChange(UInt32 auiChangedFlags);
	virtual UInt32			GetSaveSize(UInt32 auiChangedFlags) const;
	virtual void			SaveGameBGS(BGSSaveGameBuffer* apBuffer);
	virtual void			SaveGameTES(UInt32 auiChangedFlags);
	virtual void			LoadGameBGS(BGSSaveGameBuffer* apBuffer);
	virtual void			LoadGameTES(UInt32 auiChangedFlags, UInt32);
	virtual void			InitLoadGameBGS(BGSSaveGameBuffer* apBuffer);
	virtual void			InitLoadGameTES(UInt32, UInt32);
	virtual void			FinishInitLoadGameTES(UInt32, UInt32);
	virtual void			RevertBGS(BGSLoadFormBuffer* apBuffer);
	virtual void			RevertTES(UInt32 auiChangedFlags);
	virtual void			Unk_1E(void* arg);
	virtual bool			FindInFileFast(TESFile* apFile);
	virtual void			Unk_20(BGSLoadFormBuffer* apBuffer);
	virtual void			FinishLoadGameBGS(BGSLoadFormBuffer* apBuffer);
	virtual void			InitItem();
	virtual UInt32			GetSavedFormType() const;
	virtual void			GetFormDetailedString(BSString& arDest);
	virtual bool			IsPermanentOrIsQuestItem() const;
	virtual bool			HasTalkedToPC() const;
	virtual bool			GetHavokDeath();
	virtual bool			Unk_28();
	virtual bool			IsNeedToChangeProcess() const;
	virtual bool			Unk_2A();
	virtual bool			HasSpecificTextures() const; // Has Platform/Language Specific Textures
	virtual bool			GetFlag2000000() const;
	virtual bool			GetFlag40000000() const;
	virtual bool			IsShowInLocalMap() const;
	virtual void			SetCastsShadows(bool abShadowCaster);
	virtual NiColor*		GetEmittanceColor();
	virtual void			SetDelete(bool abDeleted);
	virtual void			SetAltered(bool abAltered);
	virtual void			SetQuestItem(bool abQuestIem);
	virtual void			SetTalkedToPC(bool abTalkedTo);
	virtual void			SetHavokDeath(bool abDied);
	virtual void			SetNeedToChangeProcess(bool abChange);
	virtual void			SaveObjectBound();
	virtual void			LoadObjectBound(TESFile* apFile);
	virtual bool			IsBoundObject() const;
	virtual bool			IsObject() const;
	virtual bool			IsMagicItem() const;
	virtual bool			IsReference() const;
	virtual bool			IsArmorAddon() const;
	virtual bool			IsActorBase() const;
	virtual bool			IsMobileObject() const;
	virtual bool			IsActor() const;
	virtual UInt32			Unk_41();
	virtual void			Copy(TESForm* apCopy);
	virtual bool			Compare(TESForm* apForm);
	virtual bool			BelongsInGroup(FORM* apGroupFORM, bool abAllowParentGroups, bool abCurrentOnly);
	virtual void			CreateGroupData(FORM* apForm, FORM* apOutGroupFORM);
	virtual bool			IsParentForm() const;
	virtual bool			IsParentFormTree() const;
	virtual bool			IsFormTypeChild(UInt8 ucFormType) const;
	virtual bool			Activate(TESObjectREFR*, TESObjectREFR*, bool, TESBoundObject*, int);
	virtual void			SetFormID(UInt32 aiID, bool abUpdateFile);
	virtual const char*		GetObjectTypeName();
	virtual const char*		GetEditorID() const;
	virtual bool			SetEditorID(const char* edid);

	const char* GetEditorIDAlt() const;

	struct EditorData {
		BSString	strEditorID;
		UInt32		vcMasterFormID;
		UInt32		vcRevision;
	};

	enum Flags : UInt32 {
		IS_MASTER				= 1u << 0,
		IS_ALTERED				= 1u << 1,
		INITIALIZED				= 1u << 3,
		UNK_4					= 1u << 4,
		DELETED					= 1u << 5,
		KNOWN					= 1u << 6,
		IN_PLACEABLE_WATER		= 1u << 6,
		UNK_7					= 1u << 7,
		DROPPED					= 1u << 8,
		CASTS_SHADOWS			= 1u << 9,
		QUEST_ITEM				= 1u << 10,
		PERSISTENT				= 1u << 10,
		DISABLED				= 1u << 11,
		UNK_12					= 1u << 12,
		UNK_13					= 1u << 13,
		DONT_SAVE				= 1u << 14,
		TEMPORARY				= 1u << 14,
		VISIBLE_WHEN_DISTANT	= 1u << 15,
		HAVOK_DEATH				= 1u << 16,
		NEED_TO_CHANGE_PROCESS	= 1u << 17,
		COMPRESSED				= 1u << 18,
		SPECIFIC_TEXTURES		= 1u << 19,
		CENTER_ON_CREATION		= 1u << 20,
		STILL_LOADING			= 1u << 21,
		BEING_DROPPED			= 1u << 22,
		UNK_23					= 1u << 23,
		UNK_24					= 1u << 24,
		UNK_25					= 1u << 25,
		IS_VATS_TARGETTABLE		= 1u << 26,
		DISABLE_FADE			= 1u << 27,
		CHANGED_INVENTORY		= 1u << 27,
		UNK_28					= 1u << 28,
		UNK_29					= 1u << 29,
		TALKING_ACTIVATOR		= 1u << 30,
		CONTINUOUS_BROADCAST	= 1u << 30,
		UNK_31					= 1u << 31,

		TAKEN = DELETED | IS_ALTERED,
	};

	enum {
		kModified_Flags = 0x00000001
	};

	UInt32					ucTypeID;
	Bitfield32				uiFormFlags;
	UInt32					uiFormID;
	BSSimpleList<TESFile*>	kFiles;

	inline UInt32	GetFormID() const { return uiFormID; }
	TESForm*		TryGetREFRParent();
	UInt8			GetModIndex() const;
	TESFullName*	GetFullName() const;
	const char*		GetName() const;

	static const char* GetNameForFormType(UInt32 uiFormType);
	const char* GetFormName();

	static NiTPointerMap<UInt32, TESForm*>**		const pAllForms;
	static BSTCaseInsensitiveStringMap<TESForm*>**	const pAllFormsByEditorID;
	static NiTLargePrimitiveArray<TESForm*>*		const pAlteredForms;


	bool IsWeapon() const { return ucTypeID == kFormType_TESObjectWEAP; }
	bool IsArmor() const { return ucTypeID == kFormType_TESObjectARMO; }

	bool GetDeleted() const { return uiFormFlags.IsSet(DELETED); }
	void SetDeleted(bool abSet) { uiFormFlags.SetBit(DELETED, abSet); }

	bool GetTaken() const { return uiFormFlags.IsSet(TAKEN); }
	void SetTaken(bool abSet) { uiFormFlags.SetBit(TAKEN, abSet); }

	bool GetPersistent() const { return uiFormFlags.IsSet(PERSISTENT); }
	void SetPersistent(bool abSet) { uiFormFlags.SetBit(PERSISTENT, abSet); }

	bool GetDisabled() const { return uiFormFlags.IsSet(DISABLED); }
	void SetDisabled(bool abSet) { uiFormFlags.SetBit(DISABLED, abSet); }

	bool GetDontSave() const { return uiFormFlags.IsSet(DONT_SAVE); }
	void SetDontSave(bool abSet) { uiFormFlags.SetBit(DONT_SAVE, abSet); }

	bool GetTemporary() const { return uiFormFlags.IsSet(TEMPORARY); }
	void SetTemporary(bool abSet) { uiFormFlags.SetBit(TEMPORARY, abSet); }

	void SetBeingDropped(bool abSet) { uiFormFlags.SetBit(BEING_DROPPED, abSet); }

	bool IsStillLoading() const { return uiFormFlags.IsSet(STILL_LOADING); }

	void DoAddForm(TESForm* newForm, bool bPersist = true, bool record = true) const;

	TESForm* CloneForm(bool bPersist = true);
	bool     IsInventoryObject() const;

	bool FormMatches(TESForm* toMatch) const;

	void SetTemporary();

	TESFile* GetFile(UInt32 idx) const;
	UInt32 GetRelativeFormID() const;
	UInt32 GetLoadFormID() const;

	static TESForm* GetFormByNumericID(UInt32 auID);
	static TESForm* GetFormByEditorID(const char* apEDID);

	void MarkForDelete() {
		CdeclCall(0x5D8E20, 0, 0, this);
	}
};

ASSERT_SIZE(TESForm, 0x18);
