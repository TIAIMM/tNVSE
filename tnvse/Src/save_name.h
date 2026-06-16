#pragma once

namespace fonthook
{
	bool __fastcall CharInList(const char* list, char c);
	void __fastcall SavePathProcess(DWORD, DWORD, char* szFileName);
	void InitSaveNameHook();
}