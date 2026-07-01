#pragma once
#include "BSFile.hpp"
#include "globals.h"
#include "ui_decode.h"

namespace fonthook
{
	// ---- Native game function wrappers ----
	void __cdecl ConvertToAsciiQuotes(UInt8* currentChar);
	bool __cdecl Interface_FindTextReplacementString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape);
	bool __cdecl Interface_TestConstantForGameSettings(const char* p_varNameBuffer, void* p_parsedTextBuffer);

	// ---- Float/int conversion helpers ----
	UInt32 SafeDoubleToUInt32(double value);
	UInt32 ConditionalFloatToUInt(double value);

	// ---- Font/FontManager helpers ----
	BSFile* FileFinder_GetFile(const char* apName, NiFile::OpenMode aeMode, UInt32 aiSize, UInt32 aiArchiveType);

	// ---- Inline helpers ----
	inline int MaxInt(int a, int b)
	{
		return a >= b ? a : b;
	}
	inline float MaxFloat(float a, float b)
	{
		return a >= b ? a : b;
	}
	inline float MinFloat(float a, float b)
	{
		return a <= b ? a : b;
	}
	inline int MinInt(int a, int b)
	{
		return a <= b ? a : b;
	}

} // namespace fonthook
