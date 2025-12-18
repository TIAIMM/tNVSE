#pragma once

//enum class UIEncoding : UInt32 {
//    ENG = 0,
//    CHS = 1,
//    CHT = 2,
//    JP = 3,
//    KOR = 4
//};
//
//enum class WinEncoding : UInt32 {
//    ENG = CP_ACP,
//    CHS = 936,
//    CHT = 950,
//    JP = 932,
//    KOR = 949
//};

extern UINT32 g_uiEncoding;
extern UINT32 g_usingWinEncoding;
extern bool bEnableUTF8;

void LoadConfig();