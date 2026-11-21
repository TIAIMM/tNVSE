// In a Win32 MSVC developer prompt at the repository root:
// cl /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX /Icommonlib_nv/Include/Utils /Itnvse/Src /Itnvse/Src/font tests/strict_utf8_conversion.cpp tnvse/Src/font/encoding.cpp
// strict_utf8_conversion.exe
#include "encoding.h"

#include <iostream>
#include <string>

UINT32 g_uiEncoding = 4;
UINT32 g_usingWinEncoding = 949;
bool g_bEnableUTF8 = true;
bool g_bEnableMultibyteFontHook = true;

int main()
{
	int failures = 0;
	const auto check = [&](bool pass, const char* label)
	{
		if (!pass)
		{
			std::cerr << "FAIL: " << label << '\n';
			++failures;
		}
	};

	for (const char* invalid : { "\xE0\x80\x80", "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80" })
		check(!fonthook::IsValidUTF8With3ByteMin(invalid), "reject invalid Unicode");

	const char* text = "한글";
	std::string output;
	check(fonthook::ConvertToMultiByte(text, output, true) && output == "\xC7\xD1\xB1\xDB",
		"valid Korean conversion");
	check(!fonthook::IsValidUTF8With3ByteMin("ASCII") && !fonthook::IsValidUTF8With3ByteMin("\xCE\xB1"),
		"preserve legacy detection for ASCII and two-byte text");
	for (const char* input : { "한글\xF0\x9F\x98\x80", "한글\xC2\xA0" })
	{
		text = input;
		output = "unchanged";
		check(!fonthook::ConvertToMultiByte(text, output, true) && text == input && output == "unchanged",
			"preserve text on default or best-fit mapping");
	}

	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	auto* pages = static_cast<char*>(VirtualAlloc(nullptr, info.dwPageSize * 2,
		MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	DWORD previousProtection;
	const bool protectedPage = pages && VirtualProtect(pages + info.dwPageSize,
		info.dwPageSize, PAGE_NOACCESS, &previousProtection);
	check(protectedPage, "guard page setup");
	if (protectedPage)
	{
		char* end = pages + info.dwPageSize - 1;
		*end = '\0';
		end[-1] = '\xE3';
		check(!fonthook::IsValidUTF8With3ByteMin(end - 1), "truncated three-byte sequence");
		end[-1] = '\xF0';
		check(!fonthook::IsValidUTF8With3ByteMin(end - 1), "truncated four-byte sequence");
	}
	if (pages)
		VirtualFree(pages, 0, MEM_RELEASE);

	if (!failures)
		std::cout << "11 strict UTF-8 checks passed.\n";
	return failures != 0;
}