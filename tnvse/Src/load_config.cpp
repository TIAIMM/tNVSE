#include "load_config.h"
#include "text_safety.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

UINT32 g_uiEncoding;
UINT32 g_usingWinEncoding;
bool g_bEnableUTF8;
bool g_bEnableMultibyteFontHook;
bool g_bEnableFreeTypeFontRendering;
bool g_bEnableFreeTypeFontRenderingLog;
float g_fFreeTypeFontResolutionScale = 1.0f;
UINT32 g_uiFreeTypeFontMemoryCacheMB;
bool g_bDeleteUnusedFreeTypeFontCache;
bool g_bEnableFreeTypeDefaultPoolAtlas;
bool g_bEnableFreeTypeNativeAtlas;
bool g_bEnableFreeTypeFontVanillaLayout = true;
bool g_bEnableFreeTypeFontPreflightClipCull = true;
bool g_bEnableFreeTypeFontCommandBuffer = false;
UINT32 g_uiFreeTypeFontDistanceFieldMode = kFreeTypeFontMtsdfMode;
UINT32 g_uiFreeTypeFontGpuAtlasCacheMB;
bool g_bEnableFreeTypeFontCompositePass = true;
bool g_bDisableFreeTypeRichTextEffects = false;
bool g_bFixPipBoySearchIcon = true;
bool g_bMultibyteInput;
bool g_bMultibyteInputLog;
bool g_bMultibyteInputCompositionPreview;
bool g_bMultibyteInputUseTSFCandidates;
bool g_bMultibyteInputStewieTweaks;
bool g_bMultibyteInputMCMExtender;
bool g_bMultibyteInputDialogueHistory;
bool g_bMultibyteInputModernHelpMenu;
bool g_bSuppressJIPKeyEventsDuringMultibyteInput;
bool g_bChangeJIPBigGunDesc;
std::string g_sNewBigGunsDesc;
UINT32 g_uiReorderDoorPrompt;
std::string g_sOptionalStructuralParticle;
bool g_bRemovePlural;
bool g_bSaveDisplayNameMap;
bool g_bEnableDictionaryTranslation;
bool g_bEnableDictionaryTranslationLog;
bool g_bEnableMuxQuestPromptTranslation;
bool g_bEnableDictionaryPerkDescriptionTranslation;
bool g_bEnableDictionaryItemEffectTranslation;
bool g_bEnableDictionaryMultiplierTextTranslation;
bool g_bEnableDictionaryWildcardTranslation;
bool g_bEnableDictionaryRegexTranslation;
bool g_bEnableDictionaryMixedSourceTranslation;
bool g_bEnableDictionaryBeforeLinebreakTranslation;
bool g_bEnableDictionaryShrinkFuzzyTranslation;
bool g_bEnableDictionaryTrimBypassFuzzyTranslation;

namespace
{
	constexpr const char* kMultibyteSection = "Multibyte";
	constexpr const char* kFreeTypeFontSection = "FreeTypeFont";
	constexpr const char* kMultibyteInputSection = "MultibyteInput";
	constexpr const char* kDictionarySection = "Dictionary";

	UINT32 ReadConfigInt(const char* section, const char* key, int defaultValue,
		const char* filename)
	{
		return GetPrivateProfileInt(section, key, defaultValue, filename);
	}

	void ReadConfigString(
		const char* section,
		const char* key,
		const char* defaultValue,
		char* buffer,
		DWORD bufferSize,
		const char* filename)
	{
		GetPrivateProfileString(section, key, defaultValue, buffer, bufferSize, filename);
	}

	float ReadConfigFloat(const char* section, const char* key,
		float defaultValue, const char* filename)
	{
		char defaultBuffer[32] = {};
		_snprintf_s(defaultBuffer, _countof(defaultBuffer), _TRUNCATE,
			"%.6g", defaultValue);
		char value[64] = {};
		ReadConfigString(section, key, defaultBuffer,
			value, static_cast<DWORD>(sizeof(value)), filename);
		char* end = nullptr;
		const float parsed = std::strtof(value, &end);
		while (end && *end == ' ')
			++end;
		return end != value && end && !*end && std::isfinite(parsed)
			? parsed : defaultValue;
	}
}

void LoadConfig()
{
	constexpr char kRelativeConfigPath[] = "Data\\nvse\\plugins\\tnvse.ini";
	char filename[MAX_PATH] = {};
	const DWORD modulePathLength = GetModuleFileNameA(nullptr, filename, _countof(filename));
	char* const lastSlash = modulePathLength > 0 && modulePathLength < _countof(filename)
		? std::strrchr(filename, '\\')
		: nullptr;
	const bool absoluteConfigPathReady = lastSlash
		&& fonthook::text_safety::CopyCStringIfFits(
			lastSlash + 1,
			_countof(filename) - static_cast<size_t>((lastSlash + 1) - filename),
			kRelativeConfigPath) == fonthook::text_safety::CopyStatus::Copied;
	if (!absoluteConfigPathReady)
	{
		fonthook::text_safety::CopyCStringIfFits(
			filename, _countof(filename), kRelativeConfigPath);
		gLog.FormattedMessage(
			"Config path: executable path unavailable or malformed; using relative path %s",
			filename);
	}

	g_bEnableMultibyteFontHook = ReadConfigInt(
		kMultibyteSection, "bEnableMultibyteFontHook", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableMultibyteFontHook: %d",
		g_bEnableMultibyteFontHook);

	const UINT32 configuredUiEncoding = ReadConfigInt(
		kMultibyteSection, "uiEncoding", 1, filename);
	g_uiEncoding = g_bEnableMultibyteFontHook ? configuredUiEncoding : 0;
	switch (g_uiEncoding)
	{
	case 0: g_usingWinEncoding = 1252; break; // Windows-1252
	case 1: g_usingWinEncoding = 936; break; //GBK
	case 2: g_usingWinEncoding = 950; break; //Big5
	case 3:  g_usingWinEncoding = 932;  break; //Shift-JIS
	case 4: g_usingWinEncoding = 949; break; //UHC
	default:
		g_uiEncoding = 1;
		g_usingWinEncoding = 936;
		break;
	}
	gLog.FormattedMessage("Encoding: uiEncoding=%u codePage=%u",
		g_uiEncoding, g_usingWinEncoding);

	g_bEnableUTF8 = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteSection, "bUTF8", 1, filename) != 0;
	gLog.FormattedMessage("EnableUTF8: %d", g_bEnableUTF8);
	if (!g_bEnableMultibyteFontHook)
	{
		gLog.FormattedMessage(
			"Multibyte master disabled: forcing uiEncoding=0 and disabling Multibyte, MultibyteInput, and Dictionary features");
	}

	g_bEnableFreeTypeFontRendering = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeFontRendering", 0, filename);
	gLog.FormattedMessage("g_bEnableFreeTypeFontRendering: %d", g_bEnableFreeTypeFontRendering);

	g_bEnableFreeTypeFontRenderingLog = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeFontRenderingLog", 0, filename);
	gLog.FormattedMessage("g_bEnableFreeTypeFontRenderingLog: %d", g_bEnableFreeTypeFontRenderingLog);

	const float configuredFreeTypeFontResolutionScale = std::clamp(
		ReadConfigFloat(kFreeTypeFontSection, "fFreeTypeFontResolutionScale",
			1.0f, filename), 0.1f, 10.0f);
	g_fFreeTypeFontResolutionScale = std::round(
		configuredFreeTypeFontResolutionScale * 1000.0f) / 1000.0f;
	gLog.FormattedMessage(
		"g_fFreeTypeFontResolutionScale: %.3f",
		g_fFreeTypeFontResolutionScale);
	g_uiFreeTypeFontMemoryCacheMB = std::clamp<UINT32>(
		ReadConfigInt(kFreeTypeFontSection, "uiFreeTypeFontMemoryCacheMB", 192,
			filename), 64, 384);
	gLog.FormattedMessage("g_uiFreeTypeFontMemoryCacheMB: %u", g_uiFreeTypeFontMemoryCacheMB);

	g_bDeleteUnusedFreeTypeFontCache = ReadConfigInt(
		kFreeTypeFontSection, "bDeleteUnusedFreeTypeFontCache", 0, filename) != 0;
	gLog.FormattedMessage("g_bDeleteUnusedFreeTypeFontCache: %d",
		g_bDeleteUnusedFreeTypeFontCache);

	g_bEnableFreeTypeDefaultPoolAtlas = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeDefaultPoolAtlas", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableFreeTypeDefaultPoolAtlas: %d",
		g_bEnableFreeTypeDefaultPoolAtlas);

	// The legacy A8 key remains the fallback for existing installations. The
	// accurate native-atlas key takes precedence when both are present.
	const UINT32 legacyNativeAtlas = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeA8Atlas", 1, filename);
	g_bEnableFreeTypeNativeAtlas = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeNativeAtlas",
		legacyNativeAtlas, filename) != 0;
	gLog.FormattedMessage("g_bEnableFreeTypeNativeAtlas: %d",
		g_bEnableFreeTypeNativeAtlas);

	g_bEnableFreeTypeFontVanillaLayout = ReadConfigInt(
		kFreeTypeFontSection,
		"bEnableFreeTypeFontVanillaLayout", 1, filename) != 0;
	gLog.FormattedMessage(
		"g_bEnableFreeTypeFontVanillaLayout: %d (%s)",
		g_bEnableFreeTypeFontVanillaLayout,
		g_bEnableFreeTypeFontVanillaLayout
			? "allowed" : "disabled");
	g_bEnableFreeTypeFontPreflightClipCull = ReadConfigInt(
		kFreeTypeFontSection,
		"bEnableFreeTypeFontPreflightClipCull", 1, filename) != 0;
	gLog.FormattedMessage(
		"g_bEnableFreeTypeFontPreflightClipCull: %d",
		g_bEnableFreeTypeFontPreflightClipCull);

	char commandBufferValue[32] = {};
	ReadConfigString(kFreeTypeFontSection,
		"bEnableFreeTypeFontCommandBuffer", "",
		commandBufferValue,
		static_cast<DWORD>(sizeof(commandBufferValue)), filename);
	if (commandBufferValue[0])
	{
		g_bEnableFreeTypeFontCommandBuffer = ReadConfigInt(
			kFreeTypeFontSection,
			"bEnableFreeTypeFontCommandBuffer", 1, filename) != 0;
	}
	else
	{
		const UINT32 legacyMode = ReadConfigInt(
			kFreeTypeFontSection,
			"uiFreeTypeFontCommandBufferMode", 0, filename);
		g_bEnableFreeTypeFontCommandBuffer = legacyMode != 0;
		if (legacyMode)
		{
			gLog.FormattedMessage(
				"uiFreeTypeFontCommandBufferMode=%u is deprecated; migrating this run to bEnableFreeTypeFontCommandBuffer=1",
				legacyMode);
		}
	}
	gLog.FormattedMessage(
		"g_bEnableFreeTypeFontCommandBuffer: %d (%s)",
		g_bEnableFreeTypeFontCommandBuffer,
		g_bEnableFreeTypeFontCommandBuffer
			? "retained-native-replay" : "current");

	g_uiFreeTypeFontDistanceFieldMode = ReadConfigInt(
		kFreeTypeFontSection, "uiFreeTypeFontDistanceFieldMode",
		kFreeTypeFontMtsdfMode,
		filename);
	if (g_uiFreeTypeFontDistanceFieldMode > kFreeTypeFontMtsdfMode)
	{
		gLog.FormattedMessage(
			"g_uiFreeTypeFontDistanceFieldMode: invalid value %u; using MTSDF",
			g_uiFreeTypeFontDistanceFieldMode);
		g_uiFreeTypeFontDistanceFieldMode = kFreeTypeFontMtsdfMode;
	}
	const char* distanceFieldModeName = "baked vanilla-like";
	if (g_uiFreeTypeFontDistanceFieldMode == kFreeTypeFontTsdfMode)
		distanceFieldModeName = "TSDF";
	else if (g_uiFreeTypeFontDistanceFieldMode == kFreeTypeFontMtsdfMode)
		distanceFieldModeName = "MTSDF";
	gLog.FormattedMessage(
		"g_uiFreeTypeFontDistanceFieldMode: %u (%s)",
		g_uiFreeTypeFontDistanceFieldMode,
		distanceFieldModeName);

	g_uiFreeTypeFontGpuAtlasCacheMB = ReadConfigInt(
		kFreeTypeFontSection, "uiFreeTypeFontGpuAtlasCacheMB", 0, filename);
	if (g_uiFreeTypeFontGpuAtlasCacheMB > 4095)
		g_uiFreeTypeFontGpuAtlasCacheMB = 4095;
	gLog.FormattedMessage("g_uiFreeTypeFontGpuAtlasCacheMB: %u",
		g_uiFreeTypeFontGpuAtlasCacheMB);

	g_bEnableFreeTypeFontCompositePass = ReadConfigInt(
		kFreeTypeFontSection, "bEnableFreeTypeFontCompositePass", 1,
		filename) != 0;
	gLog.FormattedMessage("g_bEnableFreeTypeFontCompositePass: %d",
		g_bEnableFreeTypeFontCompositePass);

	g_bDisableFreeTypeRichTextEffects = ReadConfigInt(
		kFreeTypeFontSection, "bDisableFreeTypeRichTextEffects", 0,
		filename) != 0;
	gLog.FormattedMessage("g_bDisableFreeTypeRichTextEffects: %d",
		g_bDisableFreeTypeRichTextEffects);

	g_bFixPipBoySearchIcon = ReadConfigInt(
		kFreeTypeFontSection, "bFixPipBoySearchIcon", 1, filename) != 0;
	gLog.FormattedMessage("g_bFixPipBoySearchIcon: %d",
		g_bFixPipBoySearchIcon);

	const UINT32 compositeCacheBudgetMB = std::clamp<UINT32>(
		ReadConfigInt(kFreeTypeFontSection, "uiFreeTypeFontCompositeCacheMB",
			32, filename), 0, 512);
	gLog.FormattedMessage(
		"g_uiFreeTypeFontCompositeCacheMB: %u (deprecated; whole-text RTT cache disabled)",
		compositeCacheBudgetMB);

	g_bMultibyteInput = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInput", 0, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInput: %d", g_bMultibyteInput);

	g_bMultibyteInputLog = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputLog", 0, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInputLog: %d", g_bMultibyteInputLog);

	g_bMultibyteInputCompositionPreview = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputCompositionPreview", 0, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInputCompositionPreview: %d", g_bMultibyteInputCompositionPreview);

	g_bMultibyteInputUseTSFCandidates = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputUseTSFCandidates", 1, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInputUseTSFCandidates: %d", g_bMultibyteInputUseTSFCandidates);

	g_bMultibyteInputStewieTweaks = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputStewieTweaks", 1, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInputStewieTweaks: %d", g_bMultibyteInputStewieTweaks);

	g_bMultibyteInputMCMExtender = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputMCMExtender", 1, filename) != 0;
	gLog.FormattedMessage("g_bMultibyteInputMCMExtender: %d", g_bMultibyteInputMCMExtender);

	g_bMultibyteInputDialogueHistory = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputDialogueHistory", 1, filename) != 0;
	gLog.FormattedMessage(
		"g_bMultibyteInputDialogueHistory: %d",
		g_bMultibyteInputDialogueHistory);

	g_bMultibyteInputModernHelpMenu = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bMultibyteInputModernHelpMenu", 1, filename) != 0;
	gLog.FormattedMessage(
		"g_bMultibyteInputModernHelpMenu: %d",
		g_bMultibyteInputModernHelpMenu);

	g_bSuppressJIPKeyEventsDuringMultibyteInput = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteInputSection,
			"bSuppressJIPKeyEventsDuringMultibyteInput", 1, filename) != 0;
	gLog.FormattedMessage(
		"g_bSuppressJIPKeyEventsDuringMultibyteInput: %d",
		g_bSuppressJIPKeyEventsDuringMultibyteInput);

	g_bChangeJIPBigGunDesc = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteSection,
			"bChangeJIPBigGunDesc", 1, filename) != 0;
	gLog.FormattedMessage("g_bChangeJIPBigGunDesc: %d",
		g_bChangeJIPBigGunDesc);

	char sTempBigGunsDesc[512] = { 0 };
	ReadConfigString(
		kMultibyteSection,
		"sNewBigGunsDesc",
		"The Big Guns skill determines your combat effectiveness with all oversized weapons such as the Fat Man, Missile Launcher, Flamer, Minigun, Gatling Laser, etc.",
		sTempBigGunsDesc,
		512,
		filename
	);
	g_sNewBigGunsDesc = sTempBigGunsDesc;

	g_uiReorderDoorPrompt = g_bEnableMultibyteFontHook
		? ReadConfigInt(kMultibyteSection, "uiReorderDoorPrompt", 1, filename)
		: 0;
	gLog.FormattedMessage("g_uiReorderDoorPrompt: %d", g_uiReorderDoorPrompt);

	char sTempStructuralParticle[512] = { 0 };
	ReadConfigString(
		kMultibyteSection,
		"sOptionalStructuralParticle",
		"",
		sTempStructuralParticle,
		512,
		filename
	);
	g_sOptionalStructuralParticle = sTempStructuralParticle;

	g_bRemovePlural = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteSection, "bRemovePlural", 1, filename) != 0;
	gLog.FormattedMessage("g_bRemovePlural: %d", g_bRemovePlural);

	g_bSaveDisplayNameMap = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kMultibyteSection, "bSaveDisplayNameMap", 1, filename) != 0;
	gLog.FormattedMessage("g_bSaveDisplayNameMap: %d", g_bSaveDisplayNameMap);

	g_bEnableDictionaryTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryTranslation: %d", g_bEnableDictionaryTranslation);

	g_bEnableDictionaryTranslationLog = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryTranslationLog", 0, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryTranslationLog: %d", g_bEnableDictionaryTranslationLog);

	g_bEnableMuxQuestPromptTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableMuxQuestPromptTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableMuxQuestPromptTranslation: %d", g_bEnableMuxQuestPromptTranslation);

	g_bEnableDictionaryPerkDescriptionTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryPerkDescriptionTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryPerkDescriptionTranslation: %d", g_bEnableDictionaryPerkDescriptionTranslation);

	g_bEnableDictionaryItemEffectTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryItemEffectTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryItemEffectTranslation: %d", g_bEnableDictionaryItemEffectTranslation);

	g_bEnableDictionaryMultiplierTextTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryMultiplierTextTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryMultiplierTextTranslation: %d", g_bEnableDictionaryMultiplierTextTranslation);

	g_bEnableDictionaryWildcardTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryWildcardTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryWildcardTranslation: %d", g_bEnableDictionaryWildcardTranslation);

	g_bEnableDictionaryRegexTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryRegexTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryRegexTranslation: %d", g_bEnableDictionaryRegexTranslation);

	g_bEnableDictionaryMixedSourceTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryMixedSourceTranslation", 0, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryMixedSourceTranslation: %d", g_bEnableDictionaryMixedSourceTranslation);

	g_bEnableDictionaryBeforeLinebreakTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryBeforeLinebreakTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryBeforeLinebreakTranslation: %d", g_bEnableDictionaryBeforeLinebreakTranslation);

	g_bEnableDictionaryShrinkFuzzyTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryShrinkFuzzyTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryShrinkFuzzyTranslation: %d", g_bEnableDictionaryShrinkFuzzyTranslation);

	g_bEnableDictionaryTrimBypassFuzzyTranslation = g_bEnableMultibyteFontHook
		&& ReadConfigInt(kDictionarySection,
			"bEnableDictionaryTrimBypassFuzzyTranslation", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableDictionaryTrimBypassFuzzyTranslation: %d", g_bEnableDictionaryTrimBypassFuzzyTranslation);
}
