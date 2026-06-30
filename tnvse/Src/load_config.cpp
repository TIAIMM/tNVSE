#include "load_config.h"

#include <cstring>

UINT32 g_uiEncoding;
UINT32 g_usingWinEncoding;
bool g_bEnableUTF8;
bool g_bChangeJIPBigGunDesc;
std::string g_sNewBigGunsDesc;
UINT32 g_uiReorderDoorPrompt;
std::string g_sOptionalStructuralParticle;
bool g_bRemovePlural;
bool g_bEnableDictionaryTranslation;
bool g_bEnableDictionaryTranslationLog;
bool g_bEnableMuxQuestPromptTranslation;

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

	g_bEnableDictionaryTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslation: %d", g_bEnableDictionaryTranslation);

	g_bEnableDictionaryTranslationLog = ReadConfigInt(kDictionarySection, kMainSection, "bEnableDictionaryTranslationLog", 0, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslationLog: %d", g_bEnableDictionaryTranslationLog);

	g_bEnableMuxQuestPromptTranslation = ReadConfigInt(kDictionarySection, kMainSection, "bEnableMuxQuestPromptTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableMuxQuestPromptTranslation: %d", g_bEnableMuxQuestPromptTranslation);
}
