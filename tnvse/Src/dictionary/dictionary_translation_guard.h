#pragma once

#include <cstdint>

namespace fonthook::dictionary_translation_guard
{
	// FalloutNV 1.4.0.525 publishes the console-open state as a byte.  JIP's
	// MenuMode 3 implementation and other NVSE plugins use the same address.
	// Reading it directly avoids calling or creating MenuConsole from font and
	// LoadingMenu translation paths.
	inline constexpr std::uintptr_t kRetailConsoleOpenStateAddress = 0x11DEA2E;

	inline bool IsConsoleOpen(
		const volatile std::uint8_t* consoleOpenState) noexcept
	{
		return consoleOpenState && *consoleOpenState != 0;
	}

	inline bool IsRetailConsoleOpen() noexcept
	{
		return IsConsoleOpen(
			reinterpret_cast<const volatile std::uint8_t*>(
				kRetailConsoleOpenStateAddress));
	}

	inline bool ShouldBypassDictionaryTranslation(
		bool dictionaryEnabled,
		bool currentTileSuppressed,
		bool suppressWhenConsoleOpen,
		bool consoleOpen) noexcept
	{
		return !dictionaryEnabled || currentTileSuppressed
			|| (suppressWhenConsoleOpen && consoleOpen);
	}
}
