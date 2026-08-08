#include "MemoryManager.hpp"
#include "NiPixelFormat.hpp"
#include "native_calls.h"

namespace fonthook
{
	void __cdecl ConvertToAsciiQuotes(UInt8* currentChar)
	{
		CdeclCall(0xA122B0, currentChar);
	}

	bool __cdecl Interface_FindTextReplacementString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape)
	{
		return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
	}

	bool __cdecl Interface_TestConstantForGameSettings(const char* p_varNameBuffer, void* p_parsedTextBuffer)
	{
		return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
	}

	// 0xEC62C0
	UInt32 SafeDoubleToUInt32(double value)
	{
		if (value >= 0.0)
			return static_cast<UInt32>(value);
		if (value <= -4294967296.0)
			return 0;
		if (value < -4294967295.0)
			return 1;
		return static_cast<UInt32>(-value);
	}

	// 0xEC62C0
	UInt32 ConditionalFloatToUInt(double value)
	{
		if (*(volatile UInt32*)0x01270A6C)
			return static_cast<UInt32>(value);
		else
			return SafeDoubleToUInt32(value);
	}

	BSFile* FileFinder_GetFile(
		const char* apName,
		NiFile::OpenMode aeMode,
		UInt32 aiSize,
		UInt32 aiArchiveType
	)
	{
		return CdeclCall<BSFile*>(0xAFDF20, apName, aeMode, aiSize, aiArchiveType);
	}

	// ---- Global singletons ----
	MemoryManager* MemoryManager_s_Instance = reinterpret_cast<MemoryManager*>(0x11F6238);
	const NiPoint3& NiPoint3_ZERO =
		*reinterpret_cast<const NiPoint3*>(0x11F426C);
	const NiPixelFormat& NiPixelFormat_RGBA32 =
		*reinterpret_cast<const NiPixelFormat*>(0x11AA2A0);

} // namespace fonthook
