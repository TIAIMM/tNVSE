#include "loadconfig.h"

UINT32 g_uiEncoding;
UINT32 g_usingWinEncoding;
bool bEnableUTF8;

void LoadConfig() {
	// From JGNVSE
	char filename[MAX_PATH];
	GetModuleFileNameA(NULL, filename, MAX_PATH);
	char g_workingDir[MAX_PATH];
	strncpy_s(g_workingDir, filename, (strlen(filename) - 13));
	char* lastSlash = (char*)(strrchr(filename, '\\') + 1);
	uint32_t length = filename - lastSlash;
	strcpy_s(lastSlash, length, "Data\\nvse\\plugins\\tnvse.ini");

	g_uiEncoding = static_cast<UINT32>(GetPrivateProfileInt("MAIN", "uiEncoding", 1, filename));
	bEnableUTF8 = static_cast<bool>(GetPrivateProfileInt("MAIN", "uiUTF8", 1, filename));
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

	gLog.FormattedMessage("EnableUTF8: %d", (unsigned int)bEnableUTF8);
	gLog.FormattedMessage("Encoding: %u", (unsigned int)g_usingWinEncoding);
}