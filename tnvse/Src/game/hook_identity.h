#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fonthook::hook_identity
{
	enum class Rel32Opcode : UInt8
	{
		Call = 0xE8,
		Jump = 0xE9,
	};

	struct Rel32Site
	{
		SIZE_T address = 0;
		SIZE_T stockTarget = 0;
		const char* name = nullptr;
	};

	struct Rel32InstructionImage
	{
		std::array<UInt8, 5> bytes = {};
		bool valid = false;
	};

	inline Rel32InstructionImage MakeRel32InstructionImage(
		SIZE_T source, Rel32Opcode opcode, SIZE_T expectedTarget)
	{
		Rel32InstructionImage image;
		if (!source || !expectedTarget)
			return image;
		const std::intptr_t displacementWide =
			static_cast<std::intptr_t>(expectedTarget)
			- static_cast<std::intptr_t>(source + image.bytes.size());
		if (displacementWide < std::numeric_limits<SInt32>::min()
			|| displacementWide > std::numeric_limits<SInt32>::max())
		{
			return image;
		}
		image.bytes[0] = static_cast<UInt8>(opcode);
		const SInt32 displacement = static_cast<SInt32>(displacementWide);
		std::memcpy(image.bytes.data() + 1u,
			&displacement, sizeof(displacement));
		image.valid = true;
		return image;
	}

	inline bool IsAccessibleRegion(
		SIZE_T address, SIZE_T size, bool requireExecutable)
	{
		if (!address || !size
			|| address > std::numeric_limits<SIZE_T>::max() - size)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION region = {};
		if (VirtualQuery(reinterpret_cast<const void*>(address),
			&region, sizeof(region)) != sizeof(region)
			|| region.State != MEM_COMMIT
			|| (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return false;
		}

		const SIZE_T regionStart =
			reinterpret_cast<SIZE_T>(region.BaseAddress);
		const SIZE_T regionEnd = regionStart + region.RegionSize;
		if (address < regionStart || address + size > regionEnd)
			return false;

		if (!requireExecutable)
			return true;

		const DWORD protection = region.Protect & 0xFFu;
		return protection == PAGE_EXECUTE
			|| protection == PAGE_EXECUTE_READ
			|| protection == PAGE_EXECUTE_READWRITE
			|| protection == PAGE_EXECUTE_WRITECOPY;
	}

	inline bool IsExecutableTarget(SIZE_T address)
	{
		return IsAccessibleRegion(address, 1, true);
	}

	inline bool ReadRel32Target(
		SIZE_T source, Rel32Opcode opcode, SIZE_T& target)
	{
		target = 0;
		if (!IsAccessibleRegion(source, 5, true)
			|| *reinterpret_cast<const UInt8*>(source)
				!= static_cast<UInt8>(opcode))
		{
			return false;
		}

		SInt32 displacement = 0;
		std::memcpy(&displacement,
			reinterpret_cast<const void*>(source + 1),
			sizeof(displacement));
		target = static_cast<SIZE_T>(
			static_cast<std::intptr_t>(source + 5)
			+ static_cast<std::intptr_t>(displacement));
		return true;
	}

	inline bool MatchesRel32Target(
		SIZE_T source, Rel32Opcode opcode, SIZE_T expectedTarget)
	{
		SIZE_T target = 0;
		return ReadRel32Target(source, opcode, target)
			&& target == expectedTarget;
	}

	// Use only after a complete IsAccessibleRegion audit has certified the
	// executable page for the current process lifetime.  This compares the full
	// instruction image instead of decoding through VirtualQuery on every hot
	// registration/readback.
	inline bool MatchesRel32TargetUnchecked(
		SIZE_T source, Rel32Opcode opcode, SIZE_T expectedTarget)
	{
		const Rel32InstructionImage expected = MakeRel32InstructionImage(
			source, opcode, expectedTarget);
		return expected.valid && std::memcmp(
			reinterpret_cast<const void*>(source), expected.bytes.data(),
			expected.bytes.size()) == 0;
	}

	inline bool MatchesRel32InstructionImageUnchecked(
		SIZE_T source, const Rel32InstructionImage& expected)
	{
		return source && expected.valid
			&& std::memcmp(reinterpret_cast<const void*>(source),
				expected.bytes.data(), expected.bytes.size()) == 0;
	}

	inline bool MatchesBytesUnchecked(
		SIZE_T source, const void* expected, SIZE_T size)
	{
		return source && expected && size
			&& std::memcmp(reinterpret_cast<const void*>(source), expected,
				size) == 0;
	}
}
