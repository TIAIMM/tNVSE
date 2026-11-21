// From the repository root in a Win32 MSVC developer prompt:
// cl /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX /Icommonlib_nv/Include/Utils /Itnvse/Src /Itnvse/Src/font tests/korean_inventory_encoding.cpp tnvse/Src/font/encoding.cpp
// korean_inventory_encoding.exe
#include "encoding.h"

#include <iostream>
#include <string>

UINT32 g_uiEncoding = 4;
UINT32 g_usingWinEncoding = 949;
bool g_bEnableUTF8 = true;
bool g_bEnableMultibyteFontHook = true;

namespace
{
    int failures = 0;

    void Check(bool condition, const char* label)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << label << '\n';
            ++failures;
        }
    }

    void CheckConversion(const std::string& input, const std::string& expected, const char* label)
    {
        const char* text = input.c_str();
        std::string output;
        Check(fonthook::ConvertToMultiByte(text, output, true) &&
            text == output.c_str() && output == expected, label);
    }

    void CheckUnchanged(const std::string& input, const char* label, bool hasExtraGlyphs = true)
    {
        const char* text = input.c_str();
        std::string output = "sentinel";
        Check(!fonthook::ConvertToMultiByte(text, output, hasExtraGlyphs) &&
            text == input.c_str() && output == "sentinel", label);
    }
}

int main()
{
    const std::string itemUtf8 = "11 .357 매그넘 탄약 [50]";
    const std::string itemCp949 = "11 .357 \xB8\xC5\xB1\xD7\xB3\xD1 \xC5\xBA\xBE\xE0 [50]";
    const std::string added = " \xC3\xDF\xB0\xA1\xB5\xCA";
    const std::string removed = " \xC1\xA6\xB0\xC5\xB5\xCA";
    const std::string equipped = "\xC0\xE5\xC2\xF8\xB5\xC7\xBE\xFA\xBD\xC0\xB4\xCF\xB4\xD9. : ";
    const std::string unequipped = "\xC0\xE5\xC2\xF8\xC7\xD8\xC1\xA6 \xB5\xC7\xBE\xFA\xBD\xC0\xB4\xCF\xB4\xD9. : ";

    Check(!fonthook::IsValidUTF8With3ByteMin((unequipped + itemUtf8).c_str()),
        "mixed notification is rejected by whole-string UTF-8 detection");
    CheckConversion(itemUtf8 + added, itemCp949 + added, "added");
    CheckConversion(itemUtf8 + removed, itemCp949 + removed, "removed");
    CheckConversion(equipped + itemUtf8, equipped + itemCp949, "equipped");
    CheckConversion(unequipped + itemUtf8, unequipped + itemCp949, "unequipped");
    CheckConversion(itemUtf8 + " 추가됨", itemCp949 + added, "all UTF-8");

    CheckUnchanged(itemCp949 + added, "all CP949 suffix");
    CheckUnchanged(unequipped + itemCp949, "all CP949 prefix");
    CheckUnchanged("11 .357" + removed, "ASCII item");
    CheckUnchanged(unequipped, "missing item");
    CheckUnchanged(itemUtf8 + added + " trailing text", "suffix must be at the end");
    CheckUnchanged(itemUtf8 + " \xC1\xA6\xB0\xC5", "unrecognized CP949 suffix");
    CheckUnchanged(unequipped + itemUtf8 + "\xF0\x9F\x98\x80", "unrepresentable character");
    CheckUnchanged(equipped + itemUtf8 + "\xED\xA0\x80", "invalid UTF-8 surrogate");
    CheckUnchanged(itemUtf8 + "\xC2\xA0" + added, "no best-fit conversion");

    CheckUnchanged(unequipped + itemUtf8, "requires extra glyphs", false);
    g_bEnableUTF8 = false;
    CheckUnchanged(unequipped + itemUtf8, "UTF-8 disabled");
    g_bEnableUTF8 = true;
    g_uiEncoding = 1;
    g_usingWinEncoding = 936;
    CheckUnchanged(unequipped + itemUtf8, "different code page");

    if (failures == 0)
        std::cout << "18 Korean inventory encoding checks passed.\n";
    return failures != 0;
}