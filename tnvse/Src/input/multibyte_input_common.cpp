#include "multibyte_input_internal.h"

// Encoding-aware character boundaries and diagnostics shared by all input targets.

namespace fonthook
{
	namespace multibyte_input
	{
		namespace implementation::multibyte_input_common {}
		using namespace implementation::multibyte_input_common;

		namespace implementation::multibyte_input_common
		{
			constexpr UInt8 FoldAsciiLetter(UInt8 value)
			{
				return value >= 'A' && value <= 'Z'
					? static_cast<UInt8>(value + ('a' - 'A'))
					: value;
			}

			constexpr bool SameAsciiKeyIdentity(UInt8 lhs, UInt8 rhs)
			{
				return lhs <= 0x7F
					&& rhs <= 0x7F
					&& FoldAsciiLetter(lhs) == FoldAsciiLetter(rhs);
			}

			static_assert(FoldAsciiLetter('A') == 'a');
			static_assert(FoldAsciiLetter('a') == 'a');
			static_assert(FoldAsciiLetter('!') == '!');
			static_assert(SameAsciiKeyIdentity('A', 'a'));
			static_assert(!SameAsciiKeyIdentity('A', 'b'));
			static_assert(!SameAsciiKeyIdentity('!', '1'));
		}

		void DebugLog(const char* fmt, ...)
		{
			if (!g_bMultibyteInputLog)
				return;

			va_list args;
			va_start(args, fmt);
			gLog.FormattedMessage(fmt, args);
			va_end(args);
		}


		char PrintableAscii(UInt32 value)
		{
			return (value >= 0x20 && value <= 0x7E) ? static_cast<char>(value) : '.';
		}

		bool AsciiEqualsIgnoreCase(UInt8 lhs, UInt8 rhs)
		{
			return SameAsciiKeyIdentity(lhs, rhs);
		}

		bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs)
		{
			return rhs >= 0 && rhs <= 0x7F
				&& AsciiEqualsIgnoreCase(lhs, static_cast<UInt8>(rhs));
		}

		UInt8 ResolveAsciiLetterCaseFromKeyboard(UInt8 input)
		{
			const UInt8 folded = FoldAsciiLetter(input);
			if (folded < 'a' || folded > 'z')
				return input;

			const bool capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
			const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const UInt8 resolved = capsLock != shiftDown
				? static_cast<UInt8>(folded - ('a' - 'A'))
				: folded;

			if (resolved != input)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=KeyboardCase action=resolve_ascii_letter raw=0x%02X resolved=0x%02X caps=%u shift=%u",
					static_cast<UInt32>(input),
					static_cast<UInt32>(resolved),
					capsLock ? 1 : 0,
					shiftDown ? 1 : 0);
			}
			return resolved;
		}

		void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input)
		{
			if (!g_bMultibyteInputLog)
				return;

			const UInt32 textLen = menu ? menu->xEditState.xText.GetLength() : 0;
			const UInt32 caret = menu ? menu->xEditState.iCaretByteOffset : 0;
			gLog.FormattedMessage(
				"tnvse_multibyte_input_event: source=%s action=%s input=0x%08X ascii='%c' composing=%u active=0x%08X current=0x%08X caret=%u textLen=%u",
				source,
				action,
				static_cast<UInt32>(input),
				PrintableAscii(static_cast<UInt32>(input)),
				s_imeComposing ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(TextEditMenu::GetCurrent()),
				caret,
				textLen);
		}

		std::string GetText(const TextEditState& state)
		{
			const UInt32 length = state.xText.GetLength();
			if (!length)
				return {};

			return std::string(state.xText.c_str(), length);
		}

		size_t NextOffset(const std::string& text, size_t offset)
		{
			if (offset >= text.size())
				return text.size();

			UInt32 code = 0;
			if (offset + 1 < text.size() && TryDecodeDoubleByte(&text[offset], code))
				return offset + 2;

			return offset + 1;
		}

		bool IsCharBoundary(const std::string& text, size_t offset)
		{
			if (offset > text.size())
				return false;

			size_t current = 0;
			while (current < text.size())
			{
				if (current == offset)
					return true;

				current = NextOffset(text, current);
			}

			return offset == text.size();
		}

		size_t PrevCharBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			size_t previous = 0;
			size_t current = 0;
			while (current < offset)
			{
				previous = current;
				current = NextOffset(text, current);
			}

			return previous;
		}

		size_t ClampToPrevBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			if (IsCharBoundary(text, offset))
				return offset;

			return PrevCharBoundary(text, offset);
		}

		size_t NextCharBoundary(const std::string& text, size_t offset)
		{
			offset = ClampToPrevBoundary(text, offset);
			return NextOffset(text, offset);
		}

		size_t NextUTF8Offset(const std::string& text, size_t offset)
		{
			if (offset >= text.size())
				return text.size();

			const UInt8 c = static_cast<UInt8>(text[offset]);

			if (c < 0x80)
				return offset + 1;

			auto hasTrail = [&](size_t index) -> bool
				{
					return index < text.size()
						&& IsUTF8Trail(static_cast<UInt8>(text[index]));
				};

			if (c >= 0xC2 && c <= 0xDF)
			{
				if (hasTrail(offset + 1))
					return offset + 2;
			}

			if (c >= 0xE0 && c <= 0xEF)
			{
				if (hasTrail(offset + 1) && hasTrail(offset + 2))
					return offset + 3;
			}

			if (c >= 0xF0 && c <= 0xF4)
			{
				if (hasTrail(offset + 1) && hasTrail(offset + 2) && hasTrail(offset + 3))
					return offset + 4;
			}

			return offset + 1;
		}

		bool IsUTF8CharBoundary(const std::string& text, size_t offset)
		{
			if (offset > text.size())
				return false;

			size_t current = 0;
			while (current < text.size())
			{
				if (current == offset)
					return true;

				current = NextUTF8Offset(text, current);
			}

			return offset == text.size();
		}

		size_t PrevUTF8CharBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());

			size_t previous = 0;
			size_t current = 0;
			while (current < offset)
			{
				previous = current;
				current = NextUTF8Offset(text, current);
			}

			return previous;
		}

		size_t ClampToPrevUTF8Boundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			if (IsUTF8CharBoundary(text, offset))
				return offset;

			return PrevUTF8CharBoundary(text, offset);
		}

		size_t NextUTF8CharBoundary(const std::string& text, size_t offset)
		{
			offset = ClampToPrevUTF8Boundary(text, offset);
			return NextUTF8Offset(text, offset);
		}

		bool IsCtrlKeyDown()
		{
			return (GetKeyState(VK_CONTROL) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		}
	}
}
