#pragma once
#include "ITypes.h"

namespace fonthook
{
	bool __fastcall CharInList(const char* list, char c);
	void __fastcall SavePathProcess(void* ptThis, UInt32 EDX, char* szFileName);
	void InitSaveNameHook();
}