#include "load_config.h"

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
bool g_bEnableFreeTypeDevicePixelScale;
float g_fFreeTypeFontResolutionScale;
UINT32 g_uiFreeTypeFontMemoryCacheMB;
bool g_bDeleteUnusedFreeTypeFontCache;
bool g_bEnableFreeTypeDefaultPoolAtlas;
bool g_bEnableFreeTypeA8Atlas;
UINT32 g_uiFreeTypeFontGpuAtlasCacheMB;
bool g_bMultibyteInput;
bool g_bMultibyteInputDebug;
bool g_bMultibyteInputCompositionPreview;
bool g_bMultibyteInputHideSystemCandidateWindow;
bool g_bMultibyteInputUseTSFCandidates;
bool g_bMultibyteInputStewieTweaks;
std::wstring g_sMultibyteInputOverlayFontPath;
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
bool g_bEnableDictionaryBeforeLinebreakTranslation;
bool g_bEnableDictionaryShrinkFuzzyTranslation;
bool g_bEnableDictionaryTrimBypassFuzzyTranslation;

namespace
{
	constexpr const char* kMainSection = "Main";
	constexpr const char* kDictionarySection = "Dictionary";
	constexpr const char* kMissingValue = "__tnvse_missing__";

	bool HasConfigKey(const char* section, const char* key, const char* filename)
	{
		char value[64] = { 0 };
		GetPrivateProfileString(section, key, kMissingValue, value, sizeof(value), filename);
		return std::strcmp(value, kMissingValue) != 0;
	}

	UINT32 ReadConfigInt(const char* section, const char* legacySection, const char* key, int defaultValue, const char* filename)
	{
		if (HasConfigKey(section, key, filename))
			return GetPrivateProfileInt(section, key, defaultValue, filename);
		if (legacySection && HasConfigKey(legacySection, key, filename))
			return GetPrivateProfileInt(legacySection, key, defaultValue, filename);
		return static_cast<UINT32>(defaultValue);
	}

	void ReadConfigString(
		const char* section,
		const char* legacySection,
		const char* key,
		const char* defaultValue,
		char* buffer,
		DWORD bufferSize,
		const char* filename)
	{
		if (HasConfigKey(section, key, filename))
		{
			GetPrivateProfileString(section, key, defaultValue, buffer, bufferSize, filename);
			return;
		}
		if (legacySection && HasConfigKey(legacySection, key, filename))
		{
			GetPrivateProfileString(legacySection, key, defaultValue, buffer, bufferSize, filename);
			return;
		}
		GetPrivateProfileString(section, key, defaultValue, buffer, bufferSize, filename);
	}

	float ReadConfigFloat(const char* section, const char* legacySection,
		const char* key, float defaultValue, const char* filename)
	{
		char defaultBuffer[32] = {};
		_snprintf_s(defaultBuffer, _countof(defaultBuffer), _TRUNCATE,
			"%.6g", defaultValue);
		char value[64] = {};
		ReadConfigString(section, legacySection, key, defaultBuffer,
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
	char filename[MAX_PATH];
	GetModuleFileNameA(NULL, filename, MAX_PATH);
	char* lastSlash = strrchr(filename, '\\') + 1;
	strcpy_s(lastSlash, MAX_PATH - (lastSlash - filename), "Data\\nvse\\plugins\\tnvse.ini");

	g_uiEncoding = ReadConfigInt(kMainSection, nullptr, "uiEncoding", 1, filename);
	switch (g_uiEncoding)
	{
	case 0: g_usingWinEncoding = 0; break;
	case 1: g_usingWinEncoding = 936; break; //GBK
	case 2: g_usingWinEncoding = 950; break; //Big5
	case 3:  g_usingWinEncoding = 932;  break; //Shift-JIS
	case 4: g_usingWinEncoding = 949; break; //UHC
	default:
		g_usingWinEncoding = 936;
		break;
	}
	//gLog.FormattedMessage("Encoding: %u", g_uiEncoding);
	gLog.FormattedMessage("Encoding: %u", g_usingWinEncoding);

	g_bEnableUTF8 = ReadConfigInt(kMainSection, nullptr, "bUTF8", 1, filename);
	gLog.FormattedMessage("EnableUTF8: %d", g_bEnableUTF8);

	g_bEnableMultibyteFontHook = ReadConfigInt(kMainSection, nullptr, "bEnableMultibyteFontHook", 1, filename);
	gLog.FormattedMessage("g_bEnableMultibyteFontHook: %d", g_bEnableMultibyteFontHook);

	g_bEnableFreeTypeFontRendering = ReadConfigInt(kMainSection, nullptr, "bEnableFreeTypeFontRendering", 0, filename);
	gLog.FormattedMessage("g_bEnableFreeTypeFontRendering: %d", g_bEnableFreeTypeFontRendering);

	g_bEnableFreeTypeFontRenderingLog = ReadConfigInt(kMainSection, nullptr, "bEnableFreeTypeFontRenderingLog", 0, filename);
	gLog.FormattedMessage("g_bEnableFreeTypeFontRenderingLog: %d", g_bEnableFreeTypeFontRenderingLog);

	g_bEnableFreeTypeDevicePixelScale = ReadConfigInt(kMainSection, nullptr, "bEnableFreeTypeDevicePixelScale", 1, filename);
	gLog.FormattedMessage("g_bEnableFreeTypeDevicePixelScale: %d", g_bEnableFreeTypeDevicePixelScale);

	g_fFreeTypeFontResolutionScale = std::clamp(
		ReadConfigFloat(kMainSection, nullptr, "fFreeTypeFontResolutionScale",
			1.0f, filename), 0.1f, 10.0f);
	gLog.FormattedMessage("g_fFreeTypeFontResolutionScale: %.3f%s",
		g_fFreeTypeFontResolutionScale,
		g_bEnableFreeTypeDevicePixelScale ? " (ignored: device pixel scale enabled)" : "");

	g_uiFreeTypeFontMemoryCacheMB = std::clamp<UINT32>(
		ReadConfigInt(kMainSection, nullptr, "uiFreeTypeFontMemoryCacheMB", 192, filename), 64, 384);
	gLog.FormattedMessage("g_uiFreeTypeFontMemoryCacheMB: %u", g_uiFreeTypeFontMemoryCacheMB);

	g_bDeleteUnusedFreeTypeFontCache = ReadConfigInt(
		kMainSection, nullptr, "bDeleteUnusedFreeTypeFontCache", 0, filename) != 0;
	gLog.FormattedMessage("g_bDeleteUnusedFreeTypeFontCache: %d",
		g_bDeleteUnusedFreeTypeFontCache);

	g_bEnableFreeTypeDefaultPoolAtlas = ReadConfigInt(
		kMainSection, nullptr, "bEnableFreeTypeDefaultPoolAtlas", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableFreeTypeDefaultPoolAtlas: %d",
		g_bEnableFreeTypeDefaultPoolAtlas);

	g_bEnableFreeTypeA8Atlas = ReadConfigInt(
		kMainSection, nullptr, "bEnableFreeTypeA8Atlas", 1, filename) != 0;
	gLog.FormattedMessage("g_bEnableFreeTypeA8Atlas: %d",
		g_bEnableFreeTypeA8Atlas);

	g_uiFreeTypeFontGpuAtlasCacheMB = ReadConfigInt(
		kMainSection, nullptr, "uiFreeTypeFontGpuAtlasCacheMB", 0, filename);
	if (g_uiFreeTypeFontGpuAtlasCacheMB > 4095)
		g_uiFreeTypeFontGpuAtlasCacheMB = 4095;
	gLog.FormattedMessage("g_uiFreeTypeFontGpuAtlasCacheMB: %u",
		g_uiFreeTypeFontGpuAtlasCacheMB);

	g_bMultibyteInput = ReadConfigInt(kMainSection, nullptr, "bMultibyteInput", 0, filename);
	gLog.FormattedMessage("g_bMultibyteInput: %d", g_bMultibyteInput);

	g_bMultibyteInputDebug = ReadConfigInt(kMainSection, nullptr, "bMultibyteInputDebug", 0, filename);
	gLog.FormattedMessage("g_bMultibyteInputDebug: %d", g_bMultibyteInputDebug);

	g_bMultibyteInputCompositionPreview = ReadConfigInt(kMainSection, nullptr, "bMultibyteInputCompositionPreview", 0, filename);
	gLog.FormattedMessage("g_bMultibyteInputCompositionPreview: %d", g_bMultibyteInputCompositionPreview);

	g_bMultibyteInputHideSystemCandidateWindow = ReadConfigInt(kMainSection, nullptr, "bMultibyteInputHideSystemCandidateWindow", 1, filename);
	gLog.FormattedMessage("g_bMultibyteInputHideSystemCandidateWindow: %d", g_bMultibyteInputHideSystemCandidateWindow);

	g_bMultibyteInputUseTSFCandidates = ReadConfigInt(kMainSection, nullptr, "bMultibyteInputUseTSFCandidates", 1, filename);
	gLog.FormattedMessage("g_bMultibyteInputUseTSFCandidates: %d", g_bMultibyteInputUseTSFCandidates);

	g_bMultibyteInputStewieTweaks = ReadConfigInt(kMainSection, nullptr, "bMultibyteInputStewieTweaks", 1, filename);
	gLog.FormattedMessage("g_bMultibyteInputStewieTweaks: %d", g_bMultibyteInputStewieTweaks);

	wchar_t wideFilename[MAX_PATH] = { 0 };
	wchar_t sTempOverlayFontPath[MAX_PATH] = { 0 };
	GetModuleFileNameW(nullptr, wideFilename, ARRAYSIZE(wideFilename));
	if (wchar_t* wideLastSlash = wcsrchr(wideFilename, L'\\'))
		wcscpy_s(wideLastSlash + 1, ARRAYSIZE(wideFilename) - (wideLastSlash + 1 - wideFilename), L"Data\\nvse\\plugins\\tnvse.ini");
	GetPrivateProfileStringW(
		L"Main",
		L"sMultibyteInputOverlayFontPath",
		L"",
		sTempOverlayFontPath,
		ARRAYSIZE(sTempOverlayFontPath),
		wideFilename);
	g_sMultibyteInputOverlayFontPath = sTempOverlayFontPath;
	gLog.FormattedMessage("g_sMultibyteInputOverlayFontPath: %ls", g_sMultibyteInputOverlayFontPath.c_str());

	g_bChangeJIPBigGunDesc = ReadConfigInt(kMainSection, nullptr, "bChangeJIPBigGunDesc", 1, filename);

	char sTempBigGunsDesc[512] = { 0 };
	ReadConfigString(
		kMainSection,
		nullptr,
		"sNewBigGunsDesc",
		"The Big Guns skill determines your combat effectiveness with all oversized weapons such as the Fat Man, Missile Launcher, Flamer, Minigun, Gatling Laser, etc.",
		sTempBigGunsDesc,
		512,
		filename
	);
	g_sNewBigGunsDesc = sTempBigGunsDesc;

	g_uiReorderDoorPrompt = ReadConfigInt(kMainSection, nullptr, "uiReorderDoorPrompt", 1, filename);
	gLog.FormattedMessage("g_uiReorderDoorPrompt: %d", g_uiReorderDoorPrompt);

	char sTempStructuralParticle[512] = { 0 };
	ReadConfigString(
		kMainSection,
		nullptr,
		"sOptionalStructuralParticle",
		"",
		sTempStructuralParticle,
		512,
		filename
	);
	g_sOptionalStructuralParticle = sTempStructuralParticle;

	g_bRemovePlural = ReadConfigInt(kMainSection, nullptr, "bRemovePlural", 1, filename);
	gLog.FormattedMessage("g_bRemovePlural: %d", g_bRemovePlural);

	g_bSaveDisplayNameMap = ReadConfigInt(kMainSection, nullptr, "bSaveDisplayNameMap", 1, filename);
	gLog.FormattedMessage("g_bSaveDisplayNameMap: %d", g_bSaveDisplayNameMap);

	g_bEnableDictionaryTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslation: %d", g_bEnableDictionaryTranslation);

	g_bEnableDictionaryTranslationLog = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryTranslationLog", 0, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslationLog: %d", g_bEnableDictionaryTranslationLog);

	g_bEnableMuxQuestPromptTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableMuxQuestPromptTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableMuxQuestPromptTranslation: %d", g_bEnableMuxQuestPromptTranslation);

	g_bEnableDictionaryPerkDescriptionTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryPerkDescriptionTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryPerkDescriptionTranslation: %d", g_bEnableDictionaryPerkDescriptionTranslation);

	g_bEnableDictionaryItemEffectTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryItemEffectTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryItemEffectTranslation: %d", g_bEnableDictionaryItemEffectTranslation);

	g_bEnableDictionaryMultiplierTextTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryMultiplierTextTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryMultiplierTextTranslation: %d", g_bEnableDictionaryMultiplierTextTranslation);

	g_bEnableDictionaryWildcardTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryWildcardTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryWildcardTranslation: %d", g_bEnableDictionaryWildcardTranslation);

	g_bEnableDictionaryRegexTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryRegexTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryRegexTranslation: %d", g_bEnableDictionaryRegexTranslation);

	g_bEnableDictionaryBeforeLinebreakTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryBeforeLinebreakTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryBeforeLinebreakTranslation: %d", g_bEnableDictionaryBeforeLinebreakTranslation);

	g_bEnableDictionaryShrinkFuzzyTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryShrinkFuzzyTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryShrinkFuzzyTranslation: %d", g_bEnableDictionaryShrinkFuzzyTranslation);

	g_bEnableDictionaryTrimBypassFuzzyTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryTrimBypassFuzzyTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTrimBypassFuzzyTranslation: %d", g_bEnableDictionaryTrimBypassFuzzyTranslation);
}
