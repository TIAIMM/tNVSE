#pragma once
#include "ITypes.h"

namespace fonthook
{
	bool __fastcall CharInList(const char* list, char c);
	void __fastcall SavePathProcess(UInt32, UInt32, char* szFileName);
	void InitSaveNameHook();
}