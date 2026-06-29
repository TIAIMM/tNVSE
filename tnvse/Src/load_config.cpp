#include "load_config.h"

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

void LoadConfig()
{
	char filename[MAX_PATH];
	GetModuleFileNameA(NULL, filename, MAX_PATH);
	char* lastSlash = strrchr(filename, '\\') + 1;
	strcpy_s(lastSlash, MAX_PATH - (lastSlash - filename), "Data\\nvse\\plugins\\tnvse.ini");

	g_uiEncoding = GetPrivateProfileInt("MAIN", "uiEncoding", 1, filename);
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

	g_bEnableUTF8 = GetPrivateProfileInt("MAIN", "bUTF8", 1, filename);
	gLog.FormattedMessage("EnableUTF8: %d", g_bEnableUTF8);

	g_bChangeJIPBigGunDesc = GetPrivateProfileInt("MAIN", "bChangeJIPBigGunDesc", 1, filename);

	char sTempBigGunsDesc[512] = { 0 };
	GetPrivateProfileString(
		"MAIN",
		"sNewBigGunsDesc",
		"The Big Guns skill determines your combat effectiveness with all oversized weapons such as the Fat Man, Missile Launcher, Flamer, Minigun, Gatling Laser, etc.",
		sTempBigGunsDesc,
		512,
		filename
	);
	g_sNewBigGunsDesc = sTempBigGunsDesc;

	g_uiReorderDoorPrompt = GetPrivateProfileInt("MAIN", "uiReorderDoorPrompt", 1, filename);
	gLog.FormattedMessage("g_uiReorderDoorPrompt: %d", g_uiReorderDoorPrompt);

	char sTempStructuralParticle[512] = { 0 };
	GetPrivateProfileString(
		"MAIN",
		"sOptionalStructuralParticle",
		"",
		sTempStructuralParticle,
		512,
		filename
	);
	g_sOptionalStructuralParticle = sTempStructuralParticle;

	g_bRemovePlural = GetPrivateProfileInt("MAIN", "bRemovePlural", 1, filename);
	gLog.FormattedMessage("g_bRemovePlural: %d", g_bRemovePlural);

	g_bEnableDictionaryTranslation = GetPrivateProfileInt("MAIN", "bEnableDictionaryTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslation: %d", g_bEnableDictionaryTranslation);

	g_bEnableDictionaryTranslationLog = GetPrivateProfileInt("MAIN", "bEnableDictionaryTranslationLog", 0, filename);
	gLog.FormattedMessage("g_bEnableDictionaryTranslationLog: %d", g_bEnableDictionaryTranslationLog);

	g_bEnableMuxQuestPromptTranslation = GetPrivateProfileInt("MAIN", "bEnableMuxQuestPromptTranslation", 1, filename);
	gLog.FormattedMessage("g_bEnableMuxQuestPromptTranslation: %d", g_bEnableMuxQuestPromptTranslation);
}
