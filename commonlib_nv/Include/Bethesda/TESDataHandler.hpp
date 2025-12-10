#pragma once

#include "BSSimpleList.hpp"
#include "BSSimpleArray.hpp"
#include "NiTArray.hpp"
#include "NiTPointerList.hpp"

class BGSAcousticSpace;
class BGSAddonNode;
class BGSBodyPartData;
class BGSCameraShot;
class BGSDebris;
class BGSDehydrationStage;
class BGSEncounterZone;
class BGSExplosion;
class BGSHeadPart;
class BGSHungerStage;
class BGSImpactData;
class BGSImpactDataSet;
class BGSLightingTemplate;
class BGSListForm;
class BGSMenuIcon;
class BGSMessage;
class BGSMusicType;
class BGSNote;
class BGSPerk;
class BGSProjectile;
class BGSRadiationStage;
class BGSRagdoll;
class BGSSleepDeprevationStage;
class BGSVoiceType;
class EnchantmentItem;
class InventoryChanges;
class MediaLocationController;
class MediaSet;
class Script;
class SpellItem;
class TESAmmoEffect;
class TESBoundObject;
class TESCaravanDeck;
class TESCasino;
class TESChallenge;
class TESClass;
class TESClimate;
class TESCombatStyle;
class TESEffectShader;
class TESEyes;
class TESFaction;
class TESFile;
class TESGlobal;
class TESHair;
class TESImageSpace;
class TESImageSpaceModifier;
class TESLandTexture;
class TESLoadScreen;
class TESLoadScreenType;
class TESObjectACTI;
class TESObjectANIO;
class TESObjectCELL;
class TESObjectDOOR;
class TESObjectList;
class TESObjectMISC;
class TESObjectSTAT;
class TESObjectWEAP;
class TESPackage;
class TESQuest;
class TESRace;
class TESRecipe;
class TESRecipeCategory;
class TESRegionDataManager;
class TESRegionList;
class TESRegions;
class TESReputation;
class TESSound;
class TESTopic;
class TESTopicInfo;
class TESWaterForm;
class TESWeather;
class TESWorldSpace;

class CompiledFiles {
protected:
	friend class TESDataHandler;

	BSSimpleList<TESFile*>	kFiles;
#if ESL_SUPPORT
	BSSimpleArray<TESFile*> kNormalFiles;
	BSSimpleArray<TESFile*> kSmallFiles;
#if OVERLAY_SUPPORT
	BSSimpleArray<TESFile*> kOverlayFiles;
	UInt32					padding[0xF4];
#else
	UInt32					padding[0xF8];
#endif
#elif OVERLAY_SUPPORT
	BSSimpleArray<TESFile*> kNormalFiles;
	BSSimpleArray<TESFile*> kOverlayFiles;
	UInt32					padding[0xF8];
#else
	UInt32					uiCompiledFileCount;
	TESFile*				pFileArray[0xFF];
#endif

public:
	UInt32 GetFileCount() const;

	TESFile* GetFile(UInt32 auiIndex) const;

#if ESL_SUPPORT
	UInt32 GetSmallFileCount() const;

	TESFile* GetSmallFile(UInt32 auiIndex) const;
#endif

#if ESL_SUPPORT || OVERLAY_SUPPORT
	void Initialize();
#endif

	BSSimpleList<TESFile*>* GetFiles();
};

ASSERT_SIZE(CompiledFiles, 0x408);

class TESDataHandler {
public:
	TESDataHandler();
	~TESDataHandler();

	Bitfield8								ucDLCFlags;				// 000
	TESObjectList*							pObjects;				// 004
	BSSimpleList<TESPackage*>				kPackages;				// 008
	BSSimpleList<TESWorldSpace*>			kWorldSpaces;			// 010
	BSSimpleList<TESClimate*>				kClimates;				// 019
	BSSimpleList<TESImageSpace*>			kImageSpaces;			// 020
	BSSimpleList<TESImageSpaceModifier*>	kImageSpaceMods;		// 028
	BSSimpleList<TESWeather*>				kWeathers;				// 030
	BSSimpleList<EnchantmentItem*>			kEnchantmentItems;		// 038
	BSSimpleList<SpellItem*>				kSpellItems;			// 040
	BSSimpleList<BGSHeadPart*>				kHeadParts;				// 048
	BSSimpleList<TESHair*>					kHairs;					// 050
	BSSimpleList<TESEyes*>					kEyes;					// 058
	BSSimpleList<TESRace*>					kRaces;					// 060
	BSSimpleList<BGSEncounterZone*>			kEncounterZones;		// 068
	BSSimpleList<TESLandTexture*>			kLandTextures;			// 070
	BSSimpleList<BGSCameraShot*>			kCameraShots;			// 078
	BSSimpleList<TESClass*>					kClasses;				// 080
	BSSimpleList<TESFaction*>				kFactions;				// 088
	BSSimpleList<TESReputation*>			kReputations;			// 090
	BSSimpleList<TESChallenge*>				kChallenges;			// 098
	BSSimpleList<TESRecipe*>				kRecipes;				// 0A0
	BSSimpleList<TESRecipeCategory*>		kRecipeCategories;		// 0A8
	BSSimpleList<TESAmmoEffect*>			kAmmoEffects;			// 0B0
	BSSimpleList<TESCasino*>				kCasinos;				// 0B8
	BSSimpleList<TESCaravanDeck*>			kCaravanDecks;			// 0C0
	BSSimpleList<Script*>					kScripts;				// 0C8
	BSSimpleList<TESSound*>					kSounds;				// 0D0
	BSSimpleList<BGSAcousticSpace*>			kAcousticSpaces;		// 0D8
	BSSimpleList<BGSRagdoll*>				kRagdolls;				// 0E0
	BSSimpleList<TESGlobal*>				kGlobals;				// 0E8
	BSSimpleList<BGSVoiceType*>				kVoiceTypes;			// 0F0
	BSSimpleList<BGSImpactData*>			kImpactData;			// 0F8
	BSSimpleList<BGSImpactDataSet*>			kImpactDataSets;		// 100
	BSSimpleList<TESTopic*>					kTopics;				// 108
	BSSimpleList<TESTopicInfo*>				kTopicInfos;			// 110
	BSSimpleList<TESQuest*>					kQuests;				// 118
	BSSimpleList<TESCombatStyle*>			kCombatStyles;			// 120
	BSSimpleList<TESLoadScreen*>			kLoadScreens;			// 128
	BSSimpleList<TESWaterForm*>				kWaterForms;			// 130
	BSSimpleList<TESEffectShader*>			kEffectShaders;			// 138
	BSSimpleList<BGSProjectile*>			kProjectiles;			// 140
	BSSimpleList<BGSExplosion*>				kExplosions;			// 148
	BSSimpleList<BGSRadiationStage*>		kRadiationStages;		// 150
	BSSimpleList<BGSDehydrationStage*>		kDehydrationStages;		// 158
	BSSimpleList<BGSHungerStage*>			kHungerStages;			// 160
	BSSimpleList<BGSSleepDeprevationStage*>	kSleepDepriveStages;	// 168
	BSSimpleList<BGSDebris*>				kDebris;				// 170
	BSSimpleList<BGSPerk*>					kPerks;					// 178
	BSSimpleList<BGSBodyPartData*>			kBodyPartData;			// 180
	BSSimpleList<BGSNote*>					kNotes;					// 188
	BSSimpleList<BGSListForm*>				kListForms;				// 190
	BSSimpleList<BGSMenuIcon*>				kMenuIcons;				// 198
	BSSimpleList<TESObjectANIO*>			kAnimatedObjects;		// 1A0
	BSSimpleList<BGSMessage*>				kMessages;				// 1A8
	BSSimpleList<BGSLightingTemplate*>		kLightningTemplates;	// 1B0
	BSSimpleList<BGSMusicType*>				kMusicTypes;			// 1B8
	BSSimpleList<TESLoadScreenType*>		kLoadScreenTypes;		// 1C0
	BSSimpleList<MediaSet*>					kMediaSets;				// 1C8
	BSSimpleList<MediaLocationController*>	kMediaLocControllers;	// 1D0
	TESRegionList*							pRegions;				// 1D8
	NiTPrimitiveArray<TESObjectCELL*>		kCellArray;				// 1DC
	NiTPrimitiveArray<BGSAddonNode*>		kAddonArray;			// 1EC
	NiTPointerList<TESForm*>				kBadForms;				// 1FC
	UInt32									uiNextCreatedRefID;		// 208
	TESFile*								pActiveFile;			// 20C
	CompiledFiles							kCompiledFiles;			// 210
	bool									bMasterSave;			// 618
	bool									bSaveLoadGame;			// 619
	bool									bCompilingFiles;		// 61A
	bool									unk61B;					// 61B
	bool									unk61C;					// 61C
	bool									bClearingData;			// 61D
	bool									bIsClosingFile;			// 61E
	bool									unk61F;					// 61F
	bool									bLoadingFiles;			// 620
	bool									bIsLoading;				// 621
	UInt8									ucGameSettingsLoadState;// 622
	bool									unk623;					// 623
	TESRegionDataManager*					pRegionManager;			// 624
	InventoryChanges*						pBarterContainer;		// 628
	UInt32									unk62C;					// 62C
	TESForm*								pSpotterEffect;			// 630
	TESForm*								pItemDetectedEffect;	// 634
	TESForm*								pCatEyeMobileEffect;	// 638

	static TESDataHandler* GetSingleton() {
		return *reinterpret_cast<TESDataHandler**>(0x11C3F2C);
	}
	UInt32 GetCompiledFileCount() const;
	TESFile* GetFile(UInt32 auiIndex) const;
	TESFile* GetFileByName(const char* apFileName);

	void SetMasterFileLargeBuffer(SInt32 aiBufferSize);

	void RemoveIDFromDataHandler(UInt32 auiFormID);

	bool DoAddForm(TESForm* apForm);

	TESQuest* GetQuestByName(const char* questName);

	TESGlobal* GetGlobal(const char* apEditorID);

	BGSAddonNode* GetAddonNode(UInt32 auiIndex);

	static TESForm* CreateFormFromID(UInt8 aucID);

	TESObjectCELL* GetCellFromCellCoord(SInt32 aiX, SInt32 aiY, TESWorldSpace* apWorldSpace, bool abUnk);
	TESObjectCELL* NewCell(const char* apName, SInt32 x, SInt32 y, TESWorldSpace* apWorldSpace);
	void UnloadCell(TESObjectCELL* apCell);

	bool CompileFiles(bool abDownloadedContentOnly);
	void AddCompiledFile(TESFile* apFile);
	bool BuildFileList(const char* apDirectory = "DATA\\");
	void FixAllScriptIDs();
	void AutoCalcStats();
	void GenerateDefaultObjects();
	bool ConstructObjectList(TESFile* apFile, bool abNoExistingForms);
	void InitAllForms();

	void CloseAllTES();

	void LoadArchivesForFile(TESFile* apFile);

	UInt32 GetDLCIndex(const char* apFileName);
	void SetDLCFlags(UInt8 aucFlags, bool abSet);

	static void EnsureFactionFriendshipsAreTwoWay();

	void CreateThreadSafeFiles();

	void SortAllLists();
};

ASSERT_SIZE(TESDataHandler, 0x63C);

extern const TESObjectSTAT* const* const pDoorMarker;
extern const TESObjectSTAT* const* const pMapMarker;
extern const TESObjectSTAT* const* const pAudioMarker;
extern const TESObjectSTAT* const* const pAudioBuoyMarker;
extern const TESObjectSTAT* const* const pBoundMarker;
extern const TESObjectSTAT* const* const pPlaneMarker;
extern const TESObjectSTAT* const* const pRoomMarker;
extern const TESObjectSTAT* const* const pPortalMarker;
extern const TESObjectSTAT* const* const pCollisionMarker;
extern const TESObjectSTAT* const* const pXMarker;
extern const TESObjectSTAT* const* const pXMarkerHeading;
extern const TESObjectSTAT* const* const pCOCMarkerHeading;
extern const TESObjectSTAT* const* const pTravelMarker;
extern const TESObjectSTAT* const* const pNorthMarker;
extern const TESObjectDOOR* const* const pDefaultDoor;
extern const TESObjectSTAT* const* const pTempleMarker;
extern const TESObjectSTAT* const* const pHolyMarker;
extern const TESObjectSTAT* const* const pRadiationMarker;
extern const TESObjectMISC* const* const pLockpick;
extern const TESObjectSTAT* const* const pHorseMarker;
extern const TESObjectWEAP* const* const pDefaultUnarmedWeapon;
extern const TESObjectACTI* const* const pAshPile1;
extern const TESObjectACTI* const* const pAshPile2;
extern const BGSExplosion * const* const pWaterExplosion;
extern const TESObjectWEAP* const* const pGasTrapDummyWeap;