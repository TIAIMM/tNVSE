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
	SIZE_T FunctionAddress(Target target)
	{
		static_assert(std::is_pointer_v<Target>
			|| std::is_member_function_pointer_v<Target>,
			"a hook target must be a function or member-function pointer");
		static_assert(sizeof(target) == sizeof(SIZE_T),
			"Fallout New Vegas hooks require an x86 single-address target");
		SIZE_T address = 0;
		std::memcpy(&address, &target, sizeof(address));
		return address;
	}

	struct RelCallHook
	{
		const char* name = nullptr;
		SIZE_T address = 0;
		SIZE_T expectedTarget = 0;
		SIZE_T hookTarget = 0;
		SIZE_T predecessor = 0;

		RelCallHook() = default;
		RelCallHook(const char* siteName, SIZE_T callAddress,
			SIZE_T vanillaTarget, SIZE_T target)
			: name(siteName), address(callAddress),
			expectedTarget(vanillaTarget), hookTarget(target)
		{}

		template <class Target>
		RelCallHook(const char* siteName, SIZE_T callAddress,
			SIZE_T vanillaTarget, Target target)
			: name(siteName), address(callAddress),
			expectedTarget(vanillaTarget), hookTarget(FunctionAddress(target))
		{}

		bool ReadTarget(SIZE_T& target) const
		{
			return hook_identity::ReadRel32Target(address,
				hook_identity::Rel32Opcode::Call, target);
		}

		bool IsExpected() const
		{
			SIZE_T target = 0;
			return expectedTarget && ReadTarget(target)
				&& target == expectedTarget;
		}

		bool IsInstalled() const
		{
			SIZE_T target = 0;
			return hookTarget && ReadTarget(target) && target == hookTarget;
		}

		bool IsInstalledUnchecked() const
		{
			return hook_identity::MatchesRel32TargetUnchecked(address,
				hook_identity::Rel32Opcode::Call, hookTarget);
		}

		// itr-nvse-style compare-before-restore: never overwrite a successor
		// which may already retain this hook as its predecessor.
		bool RollbackOwned(SIZE_T restoreTarget = 0, SIZE_T* observed = nullptr)
		{
			SIZE_T current = 0;
			if (!ReadTarget(current))
				return false;
			if (observed)
				*observed = current;

			const SIZE_T restore = restoreTarget ? restoreTarget
				: (predecessor ? predecessor : expectedTarget);
			if (!restore || restore == hookTarget)
				return false;
			if (current == hookTarget)
			{
				if (!::ReplaceCall(address, restore) || !ReadTarget(current))
					return false;
				if (observed)
					*observed = current;
			}
			return current == restore;
		}
	};

	struct VTableHook
	{
		const char* name = nullptr;
		SIZE_T entry = 0;
		SIZE_T expectedTarget = 0;
		SIZE_T hookTarget = 0;
		SIZE_T predecessor = 0;

		VTableHook() = default;
		VTableHook(const char* siteName, SIZE_T slotAddress,
			SIZE_T vanillaTarget, SIZE_T target)
			: name(siteName), entry(slotAddress),
			expectedTarget(vanillaTarget), hookTarget(target)
		{}

		template <class Target>
		VTableHook(const char* siteName, SIZE_T slotAddress,
			SIZE_T vanillaTarget, Target target)
			: name(siteName), entry(slotAddress),
			expectedTarget(vanillaTarget), hookTarget(FunctionAddress(target))
		{}

		bool ReadTarget(SIZE_T& target) const
		{
			target = 0;
			if (!hook_identity::IsAccessibleRegion(
					entry, sizeof(SIZE_T), false))
			{
				return false;
			}
			target = *reinterpret_cast<const SIZE_T*>(entry);
			return true;
		}

		bool IsExpected() const
		{
			SIZE_T target = 0;
			return expectedTarget && ReadTarget(target)
				&& target == expectedTarget;
		}

		bool IsInstalled() const
		{
			SIZE_T target = 0;
			return hookTarget && ReadTarget(target) && target == hookTarget;
		}

		bool RollbackOwned(SIZE_T restoreTarget = 0, SIZE_T* observed = nullptr)
		{
			SIZE_T current = 0;
			if (!ReadTarget(current))
				return false;
			if (observed)
				*observed = current;
			const SIZE_T restore = restoreTarget ? restoreTarget
				: (predecessor ? predecessor : expectedTarget);
			if (!restore || restore == hookTarget)
				return false;
			if (current == hookTarget)
			{
				if (!::SafeWrite32(entry, restore) || !ReadTarget(current))
					return false;
				if (observed)
					*observed = current;
			}
			return current == restore;
		}
	};

	struct EntryJumpHook
	{
		const char* name = nullptr;
		SIZE_T address = 0;
		const UInt8* original = nullptr;
		SIZE_T originalLength = 0;
		SIZE_T patchLength = 0;
		SIZE_T hookTarget = 0;

		EntryJumpHook() = default;

		template <class Target, size_t N>
		EntryJumpHook(const char* siteName, SIZE_T entryAddress,
			const std::array<UInt8, N>& originalBytes, Target target)
			: name(siteName), address(entryAddress), original(originalBytes.data()),
			originalLength(N), patchLength(N), hookTarget(FunctionAddress(target))
		{}

		template <class Target, size_t N>
		EntryJumpHook(const char* siteName, SIZE_T entryAddress,
			const std::array<UInt8, N>& originalBytes,
			SIZE_T bytesToReplace, Target target)
			: name(siteName), address(entryAddress), original(originalBytes.data()),
			originalLength(N), patchLength(bytesToReplace),
			hookTarget(FunctionAddress(target))
		{}

		bool HasOriginal() const
		{
			return hook_identity::IsAccessibleRegion(
					address, originalLength, true)
				&& original && originalLength >= 5 && patchLength >= 5
				&& patchLength <= originalLength
				&& std::memcmp(reinterpret_cast<const void*>(address),
					original, originalLength) == 0;
		}

		bool OwnsHead() const
		{
			return hook_identity::MatchesRel32Target(address,
				hook_identity::Rel32Opcode::Jump, hookTarget);
		}

		bool IsInstalled() const
		{
			if (!OwnsHead())
				return false;
			for (SIZE_T offset = 5; offset < patchLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(address + offset) != 0x90)
					return false;
			}
			return true;
		}

		bool IsInstalledUnchecked() const
		{
			if (!hook_identity::MatchesRel32TargetUnchecked(address,
					hook_identity::Rel32Opcode::Jump, hookTarget))
			{
				return false;
			}
			for (SIZE_T offset = 5; offset < patchLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(address + offset) != 0x90)
					return false;
			}
			return true;
		}

		bool RollbackOwned()
		{
			if (OwnsHead()
				&& !::SafeWriteBuf(address, original, originalLength))
				return false;
			return HasOriginal();
		}
	};

	struct InstructionCallHook
	{
		const char* name = nullptr;
		SIZE_T address = 0;
		const UInt8* original = nullptr;
		SIZE_T originalLength = 0;
		SIZE_T hookTarget = 0;

		InstructionCallHook() = default;

		template <class Target, size_t N>
		InstructionCallHook(const char* siteName, SIZE_T instructionAddress,
			const std::array<UInt8, N>& originalBytes, Target target)
			: name(siteName), address(instructionAddress),
			original(originalBytes.data()), originalLength(N),
			hookTarget(FunctionAddress(target))
		{}

		bool HasOriginal() const
		{
			return original && originalLength >= 5
				&& hook_identity::IsAccessibleRegion(address, originalLength, true)
				&& std::memcmp(reinterpret_cast<const void*>(address),
					original, originalLength) == 0;
		}

		bool IsInstalled() const
		{
			return hook_identity::MatchesRel32Target(address,
				hook_identity::Rel32Opcode::Call, hookTarget);
		}

		bool IsInstalledUnchecked() const
		{
			return hook_identity::MatchesRel32TargetUnchecked(address,
				hook_identity::Rel32Opcode::Call, hookTarget);
		}

		bool RollbackOwned()
		{
			if (IsInstalled()
				&& !::SafeWriteBuf(address, original, originalLength))
			{
				return false;
			}
			return HasOriginal();
		}
	};

	struct BytePatch
	{
		const char* name = nullptr;
		SIZE_T address = 0;
		const UInt8* original = nullptr;
		const UInt8* replacement = nullptr;
		SIZE_T length = 0;

		template <size_t N>
		BytePatch(const char* siteName, SIZE_T patchAddress,
			const std::array<UInt8, N>& originalBytes,
			const std::array<UInt8, N>& replacementBytes)
			: name(siteName), address(patchAddress),
			original(originalBytes.data()), replacement(replacementBytes.data()),
			length(N)
		{}

		bool HasOriginal() const
		{
			return hook_identity::IsAccessibleRegion(address, length, true)
				&& std::memcmp(reinterpret_cast<const void*>(address),
					original, length) == 0;
		}

		bool IsInstalled() const
		{
			return hook_identity::IsAccessibleRegion(address, length, true)
				&& std::memcmp(reinterpret_cast<const void*>(address),
					replacement, length) == 0;
		}

		bool RollbackOwned()
		{
			if (IsInstalled() && !::SafeWriteBuf(address, original, length))
				return false;
			return HasOriginal();
		}
	};

	struct WindowProcHook
	{
		const char* name = nullptr;
		WNDPROC hookTarget = nullptr;
		HWND window = nullptr;
		WNDPROC predecessor = nullptr;

		WindowProcHook(const char* siteName, WNDPROC target)
			: name(siteName), hookTarget(target)
		{}

		bool RollbackOwned(WNDPROC* observed = nullptr)
		{
			if (!window || !predecessor)
				return false;
			WNDPROC current = reinterpret_cast<WNDPROC>(
				GetWindowLongPtrA(window, GWLP_WNDPROC));
			if (observed)
				*observed = current;
			if (current != hookTarget)
				return current == predecessor;

			LONG_PTR displaced = 0;
			if (!::SafeSetWindowLongPtrA(window, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(predecessor), &displaced))
			{
				return false;
			}
			current = reinterpret_cast<WNDPROC>(
				GetWindowLongPtrA(window, GWLP_WNDPROC));
			if (observed)
				*observed = current;
			return current == predecessor;
		}
	};
}
