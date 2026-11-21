"""Compile the tag-72 UI composition routines with stubbed game inputs.

Run python tests/korean_ui_composition.py in a Win32 MSVC developer prompt.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tnvse/Src"


def functions(file, signatures):
    source = (SOURCE / file).read_text(encoding="utf-8-sig")
    result = []
    for signature in signatures:
        start = source.index(signature)
        opening = source.index("{", start)
        depth = 0
        for end in range(opening, len(source)):
            if source[end] == "{":
                depth += 1
            elif source[end] == "}":
                depth -= 1
                if depth == 0:
                    result.append(source[start:end + 1])
                    break
        else:
            raise ValueError(signature)
    return "\n".join(result)


stubs = r'''
#include "encoding.h"
#include "save_level_tile_fix.h"

#include <cstring>
#include <iostream>

UINT32 g_uiEncoding = 4;
UINT32 g_usingWinEncoding = 949;
bool g_bEnableUTF8 = true;
bool g_bEnableMultibyteFontHook = true;
std::string g_sOptionalStructuralParticle;
using namespace fonthook;

const std::string korean = "한글";
const std::string korean949 = "\xC7\xD1\xB1\xDB";
const std::string door949 = "\xB9\xAE";
const std::string level949 = "\xB7\xB9\xBA\xA7 ";
const std::string lockMessage = "This lock cannot be picked. It requires a key to open.";
bool haveTranslation = true;
bool TranslateText(const char* source, std::string& out)
{
    if (std::strcmp(source, "cell")) return false;
    out = korean;
    return true;
}

bool TryTranslateExactText(const std::string& source, std::string& out, int)
{
    if (!haveTranslation || source != lockMessage) return false;
    out = korean;
    return true;
}

struct IconFont
{
    std::string path;
    void AddTextIcon(const char* input) { path = input; }
};

struct FontManager
{
    IconFont* pFont[1] = {};
    static FontManager* GetSingleton() { return nullptr; }
};

namespace text_safety
{
    constexpr size_t kRetailReplacementNameMaxBytes = 0x7f;
    constexpr size_t kRetailReplacementOutputCapacity = 0x400;
}

bool hasGlyphs = true;
bool HasRichTextExtraGlyphs() { return hasGlyphs; }
UInt32 GetRichTextCharType(UInt8) { return 0; }
std::string replacementValue;
void Interface_FindTextReplacementString(const char*, char* buffer, size_t size, bool)
{
    strcpy_s(buffer, size, replacementValue.c_str());
}
'''

checks = r'''
int main()
{
    int failures = 0;
    int count = 0;
    const auto check = [&](bool result, const char* label)
    {
        ++count;
        if (!result) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    check(BuildDoorPromptKOR(door949.c_str(), " ", korean.c_str(), false)
        == korean949 + " " + door949, "Korean door prompt");
    check(BuildDoorPromptCHS(door949.c_str(), " ", "cell", true)
        == " " + korean949 + door949, "translated destination in reordered prompt");
    check(BuildDoorPromptKOR(door949.c_str(), " ", korean949.c_str(), false)
        == korean949 + " " + door949, "already CP949 door prompt");
    g_bEnableUTF8 = false;
    check(BuildDoorPromptKOR(door949.c_str(), " ", korean.c_str(), false)
        == korean + " " + door949, "UTF-8 option disabled");
    g_bEnableUTF8 = true;

    std::string output;
    check(TryTranslateEngineLockPrefix((lockMessage + " " + korean).c_str(), output)
        && output == korean949 + " " + korean949, "UTF-8 key name after lock text");
    check(TryTranslateEngineLockPrefix((lockMessage + "\n" + korean949).c_str(), output)
        && output == korean949 + "\n" + korean949, "CP949 key name after lock text");
    check(!TryTranslateEngineLockPrefix((lockMessage + "suffix").c_str(), output),
        "lock prefix must end at a boundary");
    haveTranslation = false;
    check(!TryTranslateEngineLockPrefix(lockMessage.c_str(), output), "lock dictionary entry required");

    const std::string title = "볼트 거주자";
    const std::string title949 = "\xBA\xBC\xC6\xAE \xB0\xC5\xC1\xD6\xC0\xDA";
    check(save_level_tile_fix::TryNormalize((level949 + "1: " + title).c_str(), output)
        && output == level949 + "1: " + title949, "mixed save level row");
    check(save_level_tile_fix::TryNormalize(("레벨 1: " + title).c_str(), output)
        && output == level949 + "1: " + title949, "UTF-8 save level row");
    check(save_level_tile_fix::TryNormalize(("Level 1: " + title).c_str(), output)
        && output == "Level 1: " + title949, "ASCII label with UTF-8 title");
    output = "sentinel";
    check(!save_level_tile_fix::TryNormalize((level949 + "1: " + title949).c_str(), output)
        && output == "sentinel", "already CP949 save row");
    check(!save_level_tile_fix::TryNormalize((level949 + "1: " + title + "\xF0\x9F\x98\x80").c_str(), output)
        && output == "sentinel", "save title cannot be converted without loss");
    check(!save_level_tile_fix::TryNormalize((level949 + "12345: " + title).c_str(), output),
        "unexpected level row format");

    IconFont font;
    FontManager manager{{&font}};
    UInt32 index = 0;
    output = door949;
    replacementValue = korean;
    check(TryAppendRichTextReplacement(&manager, "&text;", index, output)
        && index == 6 && output == door949 + korean949, "UTF-8 replacement in CP949 rich text");
    index = 0;
    output.clear();
    hasGlyphs = false;
    check(TryAppendRichTextReplacement(&manager, "&text;", index, output)
        && output == korean, "rich text without extra glyphs");
    index = 0;
    output.clear();
    replacementValue = "\\icon.dds";
    check(TryAppendRichTextReplacement(&manager, "&text;", index, output)
        && output == "\x01" && font.path == "icon.dds", "replacement icon remains intact");

    std::cout << count << " Korean UI composition checks, " << failures << " failures.\n";
    return failures != 0;
}
'''

test_source = stubs + "\n" + "\n".join([
    functions("font/text_hooks.cpp", [
        "std::string GetDoorStructuralParticle(", "const char* SafeCString(",
        "std::string NormalizeDoorUiPiece(", "const char* TranslateDoorPiece(",
        "const char* TranslateDoorToPiece(", "std::string BuildDoorPromptCHS(",
        "std::string BuildDoorPromptKOR(",
    ]),
    functions("dictionary/dictionary_translate.cpp", ["static bool TryTranslateEngineLockPrefix("]),
    functions("font/font_manager.cpp", ["bool TryAppendRichTextReplacement("]),
]) + checks

with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / "composition.cpp").write_text(test_source, encoding="utf-8")
    subprocess.run([
        "cl", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/DNOMINMAX",
        f"/I{ROOT / 'commonlib_nv/Include/Utils'}", f"/I{SOURCE}", f"/I{SOURCE / 'font'}",
        f"/I{SOURCE / 'game'}", "composition.cpp", str(SOURCE / "font/encoding.cpp"),
        "/Fe:composition.exe",
    ], cwd=work, check=True)
    subprocess.run([str(work / "composition.exe")], cwd=work, check=True)