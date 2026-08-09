#include "SafeWrite.h"

#include <array>
#include <cstring>
#include <limits>
#include <memoryapi.h>

// From Better Transitions.  tNVSE routes all process-level hook publication
// through this checked backend; per-site identity and ownership policy remain
// with the hook that owns the address.

#pragma optimize("y", on)

namespace
{
	static_assert(sizeof(SIZE_T) == sizeof(UInt32),
		"Fallout New Vegas hook patches require an x86 build");

	class MemoryUnlock
	{
	public:
		MemoryUnlock(SIZE_T address, SIZE_T size)
			: m_address(address), m_size(size)
		{
			if (!m_address || !m_size
				|| m_address > std::numeric_limits<SIZE_T>::max()
					- (m_size - 1))
			{
				return;
			}
			m_active = VirtualProtect(reinterpret_cast<void*>(m_address),
				m_size, PAGE_EXECUTE_READWRITE, &m_oldProtect) != FALSE;
		}

		~MemoryUnlock()
		{
			if (m_active)
			{
				DWORD ignored = 0;
				VirtualProtect(reinterpret_cast<void*>(m_address), m_size,
					m_oldProtect, &ignored);
			}
		}

		bool IsActive() const
		{
			return m_active;
		}

		bool Restore()
		{
			if (!m_active)
				return false;
			DWORD ignored = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(m_address), m_size,
				m_oldProtect, &ignored))
			{
				return false;
			}
			m_active = false;
			return true;
		}

	private:
		const SIZE_T m_address;
		const SIZE_T m_size;
		DWORD m_oldProtect = 0;
		bool m_active = false;
	};

	bool CompleteExecutableWrite(
		MemoryUnlock& unlock, SIZE_T address, SIZE_T size)
	{
		const bool restored = unlock.Restore();
		const bool flushed = FlushInstructionCache(GetCurrentProcess(),
			reinterpret_cast<const void*>(address), size) != FALSE;
		return restored && flushed;
	}

	bool EncodeRel32(SIZE_T source, SIZE_T target, SIZE_T instructionSize,
		UInt32& displacement)
	{
		if (!source || !target || !instructionSize
			|| source > std::numeric_limits<SIZE_T>::max() - instructionSize)
		{
			return false;
		}

		// In 32-bit mode rel32 addition wraps in the 32-bit address space, so
		// every valid x86 target has an exact modular displacement.
		displacement = static_cast<UInt32>(
			target - (source + instructionSize));
		return true;
	}
}

bool __fastcall SafeWrite8(SIZE_T addr, SIZE_T data)
{
	const UInt8 value = static_cast<UInt8>(data);
	return SafeWriteBuf(addr, &value, sizeof(value));
}

bool __fastcall SafeWrite16(SIZE_T addr, SIZE_T data)
{
	const UInt16 value = static_cast<UInt16>(data);
	return SafeWriteBuf(addr, &value, sizeof(value));
}

bool __fastcall SafeWrite32(SIZE_T addr, SIZE_T data)
{
	const UInt32 value = static_cast<UInt32>(data);
	return SafeWriteBuf(addr, &value, sizeof(value));
}

bool __fastcall SafeWriteBuf(SIZE_T addr, const void* data, SIZE_T len)
{
	if (!addr || !data || !len)
		return false;
	MemoryUnlock unlock(addr, len);
	if (!unlock.IsActive())
		return false;
	std::memcpy(reinterpret_cast<void*>(addr), data, len);
	return CompleteExecutableWrite(unlock, addr, len);
}

bool __fastcall SafeWrite32IfEqual(
	SIZE_T addr, SIZE_T data, SIZE_T expected, SIZE_T* observed)
{
	if (!addr || (addr % alignof(LONG)) != 0)
		return false;
	MemoryUnlock unlock(addr, sizeof(LONG));
	if (!unlock.IsActive())
		return false;

	const LONG previous = InterlockedCompareExchange(
		reinterpret_cast<volatile LONG*>(addr),
		static_cast<LONG>(static_cast<UInt32>(data)),
		static_cast<LONG>(static_cast<UInt32>(expected)));
	if (observed)
		*observed = static_cast<UInt32>(previous);

	const bool matched = static_cast<UInt32>(previous)
		== static_cast<UInt32>(expected);
	if (!matched)
	{
		unlock.Restore();
		return false;
	}
	return CompleteExecutableWrite(unlock, addr, sizeof(LONG));
}

bool __fastcall SafeSetWindowLongPtrA(
	HWND window, int index, LONG_PTR data, LONG_PTR* previous)
{
	if (!window || !previous)
		return false;
	*previous = 0;
	SetLastError(ERROR_SUCCESS);
	const LONG_PTR result = SetWindowLongPtrA(window, index, data);
	const DWORD error = GetLastError();
	if (!result && error != ERROR_SUCCESS)
		return false;
	*previous = result;
	return true;
}

bool __fastcall WriteRelJump(SIZE_T jumpSrc, SIZE_T jumpTgt)
{
	UInt32 displacement = 0;
	if (!EncodeRel32(jumpSrc, jumpTgt, 5, displacement))
		return false;
	std::array<UInt8, 5> instruction = { 0xE9, 0, 0, 0, 0 };
	std::memcpy(instruction.data() + 1, &displacement,
		sizeof(displacement));
	return SafeWriteBuf(jumpSrc, instruction.data(), instruction.size());
}

bool __fastcall WriteRelCall(SIZE_T jumpSrc, SIZE_T jumpTgt)
{
	UInt32 displacement = 0;
	if (!EncodeRel32(jumpSrc, jumpTgt, 5, displacement))
		return false;
	std::array<UInt8, 5> instruction = { 0xE8, 0, 0, 0, 0 };
	std::memcpy(instruction.data() + 1, &displacement,
		sizeof(displacement));
	return SafeWriteBuf(jumpSrc, instruction.data(), instruction.size());
}

bool __fastcall ReplaceCall(SIZE_T jumpSrc, SIZE_T jumpTgt)
{
	UInt32 displacement = 0;
	return EncodeRel32(jumpSrc, jumpTgt, 5, displacement)
		&& SafeWrite32(jumpSrc + 1, displacement);
}

bool __fastcall ReplaceVirtualFunc(SIZE_T jumpSrc, void* jumpTgt)
{
	return SafeWrite32(jumpSrc, reinterpret_cast<SIZE_T>(jumpTgt));
}

bool __fastcall WriteRelJnz(SIZE_T jumpSrc, SIZE_T jumpTgt)
{
	UInt32 displacement = 0;
	if (!EncodeRel32(jumpSrc, jumpTgt, 6, displacement))
		return false;
	std::array<UInt8, 6> instruction = { 0x0F, 0x85, 0, 0, 0, 0 };
	std::memcpy(instruction.data() + 2, &displacement,
		sizeof(displacement));
	return SafeWriteBuf(jumpSrc, instruction.data(), instruction.size());
}

bool __fastcall WriteRelJle(SIZE_T jumpSrc, SIZE_T jumpTgt)
{
	UInt32 displacement = 0;
	if (!EncodeRel32(jumpSrc, jumpTgt, 6, displacement))
		return false;
	std::array<UInt8, 6> instruction = { 0x0F, 0x8E, 0, 0, 0, 0 };
	std::memcpy(instruction.data() + 2, &displacement,
		sizeof(displacement));
	return SafeWriteBuf(jumpSrc, instruction.data(), instruction.size());
}

bool __fastcall PatchMemoryNop(ULONG_PTR address, SIZE_T size)
{
	if (!address || !size)
		return false;
	MemoryUnlock unlock(address, size);
	if (!unlock.IsActive())
		return false;
	std::memset(reinterpret_cast<void*>(address), 0x90, size);
	return CompleteExecutableWrite(unlock, address, size);
}

bool __fastcall PatchMemoryNopRange(
	ULONG_PTR startAddress, ULONG_PTR endAddress)
{
	return endAddress > startAddress
		&& PatchMemoryNop(startAddress, endAddress - startAddress);
}

#pragma optimize("y", off)
