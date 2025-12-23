#include "loadconfig.h"

UINT32 g_uiEncoding;
UINT32 g_usingWinEncoding;
bool g_bEnableUTF8;
bool g_bChangeJIPBigGunDesc;
std::string g_sNewBigGunsDesc;
UINT32 g_uiReorderDoorPrompt;
std::string g_sOptionalStructuralParticle;
bool g_bRemovePlural;

void LoadConfig() {
	// From JGNVSE
	char filename[MAX_PATH];
	GetModuleFileNameA(NULL, filename, MAX_PATH);
	char workingDir[MAX_PATH];
	strncpy_s(workingDir, filename, (strlen(filename) - 13));
	char* lastSlash = (char*)(strrchr(filename, '\\') + 1);
	uint32_t length = filename - lastSlash;
	strcpy_s(lastSlash, length, "Data\\nvse\\plugins\\tnvse.ini");

	g_uiEncoding = static_cast<UINT32>(GetPrivateProfileInt("MAIN", "uiEncoding", 1, filename));
	g_bEnableUTF8 = static_cast<bool>(GetPrivateProfileInt("MAIN", "bUTF8", 1, filename));
	//gLog.FormattedMessage("Encoding: %u", (unsigned int)g_uiEncoding);

	switch (g_uiEncoding) {
	case 0: g_usingWinEncoding = 0; break;
	case 1: g_usingWinEncoding = 936; break; //GBK
	case 2: g_usingWinEncoding = 950; break; //Big5
	case 3:  g_usingWinEncoding = 932;  break; //Shift-JIS
	case 4: g_usingWinEncoding = 949; break; //UHC

	default:
		g_usingWinEncoding = 936;
		break;
	}

	gLog.FormattedMessage("EnableUTF8: %d", (unsigned int)g_bEnableUTF8);
	gLog.FormattedMessage("Encoding: %u", (unsigned int)g_usingWinEncoding);

	g_bChangeJIPBigGunDesc = static_cast<bool>(GetPrivateProfileInt("MAIN", "bChangeJIPBigGunDesc", 1, filename));

	char sTempBigGunsDesc[512] = { 0 };
	GetPrivateProfileStringA(
		"MAIN",
		"sNewBigGunsDesc",
		"The Big Guns skill determines your combat effectiveness with all oversized weapons such as the Fat Man, Missile Launcher, Flamer, Minigun, Gatling Laser, etc.",
		sTempBigGunsDesc,
		512,
		filename
	);
	g_sNewBigGunsDesc = sTempBigGunsDesc;
	gLog.FormattedMessage("g_sNewBigGunsDesc: %s", g_sNewBigGunsDesc);

	g_uiReorderDoorPrompt = static_cast<UINT32>(GetPrivateProfileInt("MAIN", "uiReorderDoorPrompt", 1, filename));
	gLog.FormattedMessage("g_uiReorderDoorPrompt: %d", (unsigned int)g_uiReorderDoorPrompt);

	char sTempStructuralParticle[512] = { 0 };
	GetPrivateProfileStringA(
		"MAIN",
		"sOptionalStructuralParticle",
		"",
		sTempStructuralParticle,
		512,
		filename
	);
	g_sOptionalStructuralParticle = sTempStructuralParticle;
	gLog.FormattedMessage("g_sOptionalStructuralParticle: %s", g_sOptionalStructuralParticle);

	g_bRemovePlural = static_cast<bool>(GetPrivateProfileInt("MAIN", "bRemovePlural", 1, filename));
	gLog.FormattedMessage("g_bRemovePlural: %d", (unsigned int)g_bRemovePlural);
}