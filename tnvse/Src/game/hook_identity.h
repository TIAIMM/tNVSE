#pragma once

#include <Windows.h>

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
}
