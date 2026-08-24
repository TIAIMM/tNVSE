#include "MemoryManager.hpp"
#include "NiPixelFormat.hpp"
#include "native_calls.h"
#include "text_safety.h"

namespace fonthook
{
	void __cdecl ConvertToAsciiQuotes(UInt8* currentChar)
	{
		CdeclCall<void>(0xA122B0, currentChar);
	}

	bool __cdecl Interface_FindTextReplacementString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape)
	{
		text_safety::ClearBuffer(outBuffer, bufferSize);
		if (!text_safety::IsRetailReplacementArgumentSafe(varName)
			|| !outBuffer
			|| bufferSize < text_safety::kRetailReplacementOutputCapacity)
		{
			return false;
		}
		return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
	}

	bool __cdecl Interface_TestConstantForGameSettings(const char* p_varNameBuffer, void* p_parsedTextBuffer)
	{
		char* output = static_cast<char*>(p_parsedTextBuffer);
		text_safety::ClearBuffer(output, 1);
		if (!text_safety::IsRetailReplacementArgumentSafe(p_varNameBuffer)
			|| !output)
		{
			return false;
		}
		return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
	}

	// Retail's compiler helper consumes the double from x87 ST(0), not from a
	// normal stack argument. Keep that nonstandard ABI inside this wrapper.
	UInt32 ConditionalFloatToUInt(double value)
	{
		UInt32 result = 0;
		__asm
		{
			fld qword ptr[value]
			mov eax, 0EC62C0h
			call eax
			mov result, eax
		}
		return result;
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
