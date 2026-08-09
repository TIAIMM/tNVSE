#pragma once

#include "hook_identity.h"
#include "SafeWrite.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

// Identity and rollback metadata for tNVSE's process-wide hooks.
//
// Installation deliberately stays explicit in each feature source as
// WriteRelCall/WriteRelCallEx/WriteRelJumpEx/SafeWrite32, matching the xNVSE
// plugin style. These records validate the complete site and restore only
// while the top-level bytes/slot are still tNVSE's; they are not another
// publication abstraction.

namespace fonthook::hook_site
{
	template <class Target>
	SIZE_T FunctionAddress(Target functionPointer)
	{
		static_assert(std::is_pointer_v<Target>
			|| std::is_member_function_pointer_v<Target>,
			"a hook target must be a function or member-function pointer");
		static_assert(sizeof(functionPointer) == sizeof(SIZE_T),
			"Fallout New Vegas hooks require an x86 single-address target");
		SIZE_T functionAddress = 0;
		std::memcpy(&functionAddress, &functionPointer,
			sizeof(functionAddress));
		return functionAddress;
	}

	struct RelCallSite
	{
		const char* description = nullptr;
		SIZE_T callAddress = 0;
		SIZE_T expectedTarget = 0;
		SIZE_T replacementTarget = 0;
		SIZE_T predecessorTarget = 0;

		RelCallSite() = default;
		RelCallSite(const char* siteName, SIZE_T addressOfCall,
			SIZE_T vanillaTarget, SIZE_T replacement)
			: description(siteName), callAddress(addressOfCall),
			expectedTarget(vanillaTarget), replacementTarget(replacement)
		{}

		template <class Target>
		RelCallSite(const char* siteName, SIZE_T addressOfCall,
			SIZE_T vanillaTarget, Target replacement)
			: description(siteName), callAddress(addressOfCall),
			expectedTarget(vanillaTarget),
			replacementTarget(FunctionAddress(replacement))
		{}

		bool ReadTarget(SIZE_T& outTarget) const
		{
			return hook_identity::ReadRel32Target(callAddress,
				hook_identity::Rel32Opcode::Call, outTarget);
		}

		bool IsExpected() const
		{
			SIZE_T observedTarget = 0;
			return expectedTarget && ReadTarget(observedTarget)
				&& observedTarget == expectedTarget;
		}

		bool IsInstalled() const
		{
			SIZE_T observedTarget = 0;
			return replacementTarget && ReadTarget(observedTarget)
				&& observedTarget == replacementTarget;
		}

		bool IsInstalledUnchecked() const
		{
			return hook_identity::MatchesRel32TargetUnchecked(callAddress,
				hook_identity::Rel32Opcode::Call, replacementTarget);
		}

		// itr-nvse-style compare-before-restore: never overwrite a successor
		// which may already retain this hook as its predecessor.
		bool RollbackOwned(SIZE_T restoreTarget = 0,
			SIZE_T* observedTarget = nullptr)
		{
			SIZE_T currentTarget = 0;
			if (!ReadTarget(currentTarget))
				return false;
			if (observedTarget)
				*observedTarget = currentTarget;

			const SIZE_T restorationTarget = restoreTarget ? restoreTarget
				: (predecessorTarget ? predecessorTarget : expectedTarget);
			if (!restorationTarget || restorationTarget == replacementTarget)
				return false;
			if (currentTarget == replacementTarget)
			{
				if (!::ReplaceCall(callAddress, restorationTarget)
					|| !ReadTarget(currentTarget))
					return false;
				if (observedTarget)
					*observedTarget = currentTarget;
			}
			return currentTarget == restorationTarget;
		}
	};

	struct VTableSlotSite
	{
		const char* description = nullptr;
		SIZE_T slotAddress = 0;
		SIZE_T expectedTarget = 0;
		SIZE_T replacementTarget = 0;
		SIZE_T predecessorTarget = 0;

		VTableSlotSite() = default;
		VTableSlotSite(const char* siteName, SIZE_T addressOfSlot,
			SIZE_T vanillaTarget, SIZE_T replacement)
			: description(siteName), slotAddress(addressOfSlot),
			expectedTarget(vanillaTarget), replacementTarget(replacement)
		{}

		template <class Target>
		VTableSlotSite(const char* siteName, SIZE_T addressOfSlot,
			SIZE_T vanillaTarget, Target replacement)
			: description(siteName), slotAddress(addressOfSlot),
			expectedTarget(vanillaTarget),
			replacementTarget(FunctionAddress(replacement))
		{}

		bool ReadTarget(SIZE_T& outTarget) const
		{
			outTarget = 0;
			if (!hook_identity::IsAccessibleRegion(
					slotAddress, sizeof(SIZE_T), false))
			{
				return false;
			}
			outTarget = *reinterpret_cast<const SIZE_T*>(slotAddress);
			return true;
		}

		bool IsExpected() const
		{
			SIZE_T observedTarget = 0;
			return expectedTarget && ReadTarget(observedTarget)
				&& observedTarget == expectedTarget;
		}

		bool IsInstalled() const
		{
			SIZE_T observedTarget = 0;
			return replacementTarget && ReadTarget(observedTarget)
				&& observedTarget == replacementTarget;
		}

		bool RollbackOwned(SIZE_T restoreTarget = 0,
			SIZE_T* observedTarget = nullptr)
		{
			SIZE_T currentTarget = 0;
			if (!ReadTarget(currentTarget))
				return false;
			if (observedTarget)
				*observedTarget = currentTarget;
			const SIZE_T restorationTarget = restoreTarget ? restoreTarget
				: (predecessorTarget ? predecessorTarget : expectedTarget);
			if (!restorationTarget || restorationTarget == replacementTarget)
				return false;
			if (currentTarget == replacementTarget)
			{
				if (!::SafeWrite32(slotAddress, restorationTarget)
					|| !ReadTarget(currentTarget))
					return false;
				if (observedTarget)
					*observedTarget = currentTarget;
			}
			return currentTarget == restorationTarget;
		}
	};

	struct EntryJumpSite
	{
		const char* description = nullptr;
		SIZE_T entryAddress = 0;
		const UInt8* originalBytes = nullptr;
		SIZE_T originalLength = 0;
		SIZE_T patchLength = 0;
		SIZE_T replacementTarget = 0;

		EntryJumpSite() = default;

		template <class Target, size_t N>
		EntryJumpSite(const char* siteName, SIZE_T addressOfEntry,
			const std::array<UInt8, N>& expectedBytes, Target replacement)
			: description(siteName), entryAddress(addressOfEntry),
			originalBytes(expectedBytes.data()), originalLength(N), patchLength(N),
			replacementTarget(FunctionAddress(replacement))
		{}

		template <class Target, size_t N>
		EntryJumpSite(const char* siteName, SIZE_T addressOfEntry,
			const std::array<UInt8, N>& expectedBytes,
			SIZE_T bytesToReplace, Target replacement)
			: description(siteName), entryAddress(addressOfEntry),
			originalBytes(expectedBytes.data()), originalLength(N),
			patchLength(bytesToReplace),
			replacementTarget(FunctionAddress(replacement))
		{}

		bool MatchesOriginalBytes() const
		{
			return hook_identity::IsAccessibleRegion(
					entryAddress, originalLength, true)
				&& originalBytes && originalLength >= 5 && patchLength >= 5
				&& patchLength <= originalLength
				&& std::memcmp(reinterpret_cast<const void*>(entryAddress),
					originalBytes, originalLength) == 0;
		}

		bool OwnsJumpHead() const
		{
			return hook_identity::MatchesRel32Target(entryAddress,
				hook_identity::Rel32Opcode::Jump, replacementTarget);
		}

		bool IsInstalled() const
		{
			if (!OwnsJumpHead())
				return false;
			for (SIZE_T offset = 5; offset < patchLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(entryAddress + offset) != 0x90)
					return false;
			}
			return true;
		}

		bool IsInstalledUnchecked() const
		{
			if (!hook_identity::MatchesRel32TargetUnchecked(entryAddress,
					hook_identity::Rel32Opcode::Jump, replacementTarget))
			{
				return false;
			}
			for (SIZE_T offset = 5; offset < patchLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(entryAddress + offset) != 0x90)
					return false;
			}
			return true;
		}

		bool RollbackOwned()
		{
			if (OwnsJumpHead()
				&& !::SafeWriteBuf(
					entryAddress, originalBytes, originalLength))
				return false;
			return MatchesOriginalBytes();
		}
	};

	struct InstructionCallSite
	{
		const char* description = nullptr;
		SIZE_T instructionAddress = 0;
		const UInt8* originalBytes = nullptr;
		SIZE_T originalLength = 0;
		SIZE_T replacementTarget = 0;

		InstructionCallSite() = default;

		template <class Target, size_t N>
		InstructionCallSite(const char* siteName, SIZE_T addressOfInstruction,
			const std::array<UInt8, N>& expectedBytes, Target replacement)
			: description(siteName), instructionAddress(addressOfInstruction),
			originalBytes(expectedBytes.data()), originalLength(N),
			replacementTarget(FunctionAddress(replacement))
		{}

		bool MatchesOriginalBytes() const
		{
			return originalBytes && originalLength >= 5
				&& hook_identity::IsAccessibleRegion(
					instructionAddress, originalLength, true)
				&& std::memcmp(reinterpret_cast<const void*>(instructionAddress),
					originalBytes, originalLength) == 0;
		}

		bool IsInstalled() const
		{
			return hook_identity::MatchesRel32Target(instructionAddress,
				hook_identity::Rel32Opcode::Call, replacementTarget);
		}

		bool IsInstalledUnchecked() const
		{
			return hook_identity::MatchesRel32TargetUnchecked(
				instructionAddress, hook_identity::Rel32Opcode::Call,
				replacementTarget);
		}

		bool RollbackOwned()
		{
			if (IsInstalled()
				&& !::SafeWriteBuf(
					instructionAddress, originalBytes, originalLength))
			{
				return false;
			}
			return MatchesOriginalBytes();
		}
	};

	struct BytePatchSite
	{
		const char* description = nullptr;
		SIZE_T patchAddress = 0;
		const UInt8* originalBytes = nullptr;
		const UInt8* replacementBytes = nullptr;
		SIZE_T length = 0;

		template <size_t N>
		BytePatchSite(const char* siteName, SIZE_T addressOfPatch,
			const std::array<UInt8, N>& expectedBytes,
			const std::array<UInt8, N>& patchedBytes)
			: description(siteName), patchAddress(addressOfPatch),
			originalBytes(expectedBytes.data()),
			replacementBytes(patchedBytes.data()),
			length(N)
		{}

		bool MatchesOriginalBytes() const
		{
			return hook_identity::IsAccessibleRegion(patchAddress, length, true)
				&& std::memcmp(reinterpret_cast<const void*>(patchAddress),
					originalBytes, length) == 0;
		}

		bool IsInstalled() const
		{
			return hook_identity::IsAccessibleRegion(patchAddress, length, true)
				&& std::memcmp(reinterpret_cast<const void*>(patchAddress),
					replacementBytes, length) == 0;
		}

		bool RollbackOwned()
		{
			if (IsInstalled()
				&& !::SafeWriteBuf(patchAddress, originalBytes, length))
				return false;
			return MatchesOriginalBytes();
		}
	};

	struct WindowProcSite
	{
		const char* description = nullptr;
		WNDPROC replacementProc = nullptr;
		HWND windowHandle = nullptr;
		WNDPROC predecessorProc = nullptr;

		WindowProcSite(const char* siteName, WNDPROC replacement)
			: description(siteName), replacementProc(replacement)
		{}

		bool RollbackOwned(WNDPROC* observedProc = nullptr)
		{
			if (!windowHandle || !predecessorProc)
				return false;
			WNDPROC currentProc = reinterpret_cast<WNDPROC>(
				GetWindowLongPtrA(windowHandle, GWLP_WNDPROC));
			if (observedProc)
				*observedProc = currentProc;
			if (currentProc != replacementProc)
				return currentProc == predecessorProc;

			LONG_PTR displaced = 0;
			if (!::SafeSetWindowLongPtrA(windowHandle, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(predecessorProc), &displaced))
			{
				return false;
			}
			currentProc = reinterpret_cast<WNDPROC>(
				GetWindowLongPtrA(windowHandle, GWLP_WNDPROC));
			if (observedProc)
				*observedProc = currentProc;
			return currentProc == predecessorProc;
		}
	};
}
