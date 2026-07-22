#include "multibyte_input_ime_internal.h"

#include "plugin_dependencies.h"

#include <array>

// JIP LN 57.30 polls DIHookControl::rawState directly in LN_ProcessEvents.
// Replace only that five-byte comparison, leaving the shared DirectInput state
// and every non-JIP input consumer untouched.

namespace fonthook::multibyte_input
{
	namespace
	{
		constexpr SIZE_T kJipRawKeyStateCompareRva = 0x13C59;
		constexpr SIZE_T kJipLastKeyStateRva = 0x772E0;
		constexpr UInt32 kKeyboardKeyCount = 256;
		constexpr SIZE_T kJipKeyInfoStride = 7;
		constexpr SIZE_T kJipRawStateOffset = 4;

		constexpr std::array<UInt8, 18> kJipRawKeyStateSignature = {
			0x80, 0x7C, 0x01, 0x04, 0x00,
			0x74, 0x04, 0xB3, 0x01, 0xEB, 0x02, 0x32, 0xDB,
			0x88, 0x5D, 0xFF,
			0x38, 0x9F,
		};

		bool s_hookInstalled = false;
		bool s_installAttempted = false;
		bool s_captureActive = false;
		bool s_captureEndedPendingSnapshot = false;
		std::array<bool, kKeyboardKeyCount> s_suppressUntilRelease = {};
		volatile UInt8 s_filteredRawState = 0;

		UInt8* JipLastKeyStates()
		{
			return hJIP
				? reinterpret_cast<UInt8*>(hJIP) + kJipLastKeyStateRva
				: nullptr;
		}

		bool RawKeyboardState(const UInt8* diHookControl, UInt32 key)
		{
			return diHookControl
				&& diHookControl[key * kJipKeyInfoStride + kJipRawStateOffset] != 0;
		}

		bool IsModuleRangeValid(HMODULE module, SIZE_T rva, SIZE_T length)
		{
			if (!module || !length)
				return false;

			const auto base = reinterpret_cast<const UInt8*>(module);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
				return false;

			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;

			const SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
			return rva < imageSize && length <= imageSize - rva;
		}

		bool MatchesJipRawKeyStateSignature()
		{
			constexpr SIZE_T kAbsoluteOperandSize = sizeof(UInt32);
			const SIZE_T inspectedLength = kJipRawKeyStateSignature.size()
				+ kAbsoluteOperandSize;
			if (!IsModuleRangeValid(
				hJIP, kJipRawKeyStateCompareRva, inspectedLength))
			{
				return false;
			}

			const auto code = reinterpret_cast<const UInt8*>(hJIP)
				+ kJipRawKeyStateCompareRva;
			if (!std::equal(
				kJipRawKeyStateSignature.begin(),
				kJipRawKeyStateSignature.end(), code))
			{
				return false;
			}

			UInt32 lastKeyStateOperand = 0;
			std::memcpy(
				&lastKeyStateOperand,
				code + kJipRawKeyStateSignature.size(),
				sizeof(lastKeyStateOperand));
			const UInt32 expectedLastKeyState =
				reinterpret_cast<UInt32>(hJIP)
				+ static_cast<UInt32>(kJipLastKeyStateRva);
			return lastKeyStateOperand == expectedLastKeyState;
		}

		bool __cdecl FilterJipRawKeyState(
			UInt32 key, bool rawState, const UInt8* diHookControl)
		{
			UInt8* lastKeyState = JipLastKeyStates();
			if (!lastKeyState)
				return rawState;

			// The key that closes a text field may make capture inactive before JIP
			// polls the same frame. Snapshot every physically held keyboard key on
			// the first subsequent observation and drain it through release.
			if (s_captureEndedPendingSnapshot)
			{
				for (UInt32 index = 0; index < kKeyboardKeyCount; ++index)
				{
					if (RawKeyboardState(diHookControl, index))
						s_suppressUntilRelease[index] = true;
				}
				s_captureEndedPendingSnapshot = false;
			}

			if (key >= kKeyboardKeyCount)
				return rawState;

			if (s_captureActive)
			{
				s_suppressUntilRelease[key] = rawState;
				lastKeyState[key] = rawState;
				return rawState;
			}

			if (s_suppressUntilRelease[key])
			{
				lastKeyState[key] = rawState;
				if (!rawState)
					s_suppressUntilRelease[key] = false;
			}

			return rawState;
		}

		__declspec(naked) void JipRawKeyStateCompareHook()
		{
			__asm
			{
				// Original state at this site:
				//   EDI = key, EAX = DIHookControl*, ECX = key * 7.
				// Preserve every register and the incoming flags. The original
				// instruction only set flags for the following JE.
				pushfd
				pushad
				movzx edx, byte ptr [ecx + eax + 4]
				push eax
				push edx
				push edi
				call FilterJipRawKeyState
				add esp, 12
				mov byte ptr [s_filteredRawState], al
				popad
				popfd
				cmp byte ptr [s_filteredRawState], 0
				ret
			}
		}
	}

	void SetJipKeyEventSuppressionCaptureActive(bool active)
	{
		if (!g_bSuppressJIPKeyEventsDuringMultibyteInput
			|| s_captureActive == active)
		{
			return;
		}

		if (s_captureActive && !active)
			s_captureEndedPendingSnapshot = true;
		s_captureActive = active;
	}

	bool IsJipKeyEventSuppressionHookInstalled()
	{
		return s_hookInstalled;
	}

	void TryInstallJipKeyEventSuppressionHook()
	{
		if (s_hookInstalled || s_installAttempted
			|| !g_bMultibyteInput
			|| !g_bSuppressJIPKeyEventsDuringMultibyteInput)
		{
			return;
		}
		s_installAttempted = true;

		hJIP = GetModuleHandleA("jip_nvse.dll");
		const PluginInfo* info = g_cmdTableInterface
			&& g_cmdTableInterface->GetPluginInfoByName
			? g_cmdTableInterface->GetPluginInfoByName(
				dependencies::kJipPluginName)
			: nullptr;
		if (!hJIP || !dependencies::IsPluginInfoValid(info)
			|| info->version != dependencies::kJipKeyEventFilterVersion)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: JIP key-event suppression unavailable; required JIP LN NVSE 57.30");
			return;
		}

		if (!MatchesJipRawKeyStateSignature())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: JIP 57.30 key-event suppression signature mismatch at RVA 0x%05X; code left untouched",
				static_cast<UInt32>(kJipRawKeyStateCompareRva));
			return;
		}

		WriteRelCall(
			reinterpret_cast<SIZE_T>(hJIP) + kJipRawKeyStateCompareRva,
			&JipRawKeyStateCompareHook);
		s_captureActive = State().textInputSessionActive;
		s_hookInstalled = true;
		gLog.FormattedMessage(
			"tnvse_multibyte_input: installed JIP 57.30 OnKeyDown/OnKeyUp suppression hook at RVA 0x%05X",
			static_cast<UInt32>(kJipRawKeyStateCompareRva));
	}
}
