#pragma once
#include <string>
#include <Windows.h>

extern UINT32 g_uiEncoding;       // UI encoding identifier: 0=Windows-1252, 1=GBK, 2=Big5, 3=SJIS, 4=UHC
extern UINT32 g_usingWinEncoding; // Configured Windows code page: 1252/936/950/932/949
extern bool g_bEnableUTF8;
extern bool g_bEnableMultibyteFontHook;
extern bool g_bEnableFreeTypeFontRendering;
extern bool g_bEnableFreeTypeFontRenderingLog;
extern float g_fFreeTypeFontResolutionScale;
extern UINT32 g_uiFreeTypeFontMemoryCacheMB;
extern bool g_bDeleteUnusedFreeTypeFontCache;
extern bool g_bEnableFreeTypeDefaultPoolAtlas;
extern bool g_bEnableFreeTypeA8Atlas;
extern bool g_bEnableFreeTypeFontAggressivePerformanceMode;
extern bool g_bEnableFreeTypeFontStructuralFastPaths;
extern bool g_bEnableFreeTypeFontPreflightClipCull;
extern bool g_bEnableFreeTypeFontCommandBuffer;
extern UINT32 g_uiFreeTypeFontDistanceFieldMode;
extern UINT32 g_uiFreeTypeFontGpuAtlasCacheMB;
extern bool g_bEnableFreeTypeFontCompositePass;
extern UINT32 g_uiFreeTypeFontCompositeCacheMB;
extern bool g_bMultibyteInput;
extern bool g_bMultibyteInputLog;
extern bool g_bMultibyteInputCompositionPreview;
extern bool g_bMultibyteInputUseTSFCandidates;
extern bool g_bMultibyteInputStewieTweaks;
extern bool g_bMultibyteInputMCMExtender;
extern bool g_bMultibyteInputDialogueHistory;
extern bool g_bSuppressJIPKeyEventsDuringMultibyteInput;
extern bool g_bChangeJIPBigGunDesc;
extern std::string g_sNewBigGunsDesc;
extern UINT32 g_uiReorderDoorPrompt;  // 0=off, 1=CHS order, 2=KOR order
extern std::string g_sOptionalStructuralParticle;
extern bool g_bRemovePlural;
extern bool g_bSaveDisplayNameMap;
extern bool g_bEnableDictionaryTranslation;
extern bool g_bEnableDictionaryTranslationLog;
extern bool g_bEnableMuxQuestPromptTranslation;
extern bool g_bEnableDictionaryPerkDescriptionTranslation;
extern bool g_bEnableDictionaryItemEffectTranslation;
extern bool g_bEnableDictionaryMultiplierTextTranslation;
extern bool g_bEnableDictionaryWildcardTranslation;
extern bool g_bEnableDictionaryRegexTranslation;
extern bool g_bEnableDictionaryBeforeLinebreakTranslation;
extern bool g_bEnableDictionaryShrinkFuzzyTranslation;
extern bool g_bEnableDictionaryTrimBypassFuzzyTranslation;

void LoadConfig();
