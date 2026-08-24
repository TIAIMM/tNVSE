#pragma once

#include <cstdint>

namespace fonthook::multibyte_prewarm
{
	inline constexpr std::uint32_t kCodePageGbk = 936;
	inline constexpr std::uint32_t kCodePageBig5 = 950;
	inline constexpr std::uint32_t kCodePageShiftJis = 932;
	inline constexpr std::uint32_t kCodePageUhc = 949;

	constexpr bool IsDbcsCodePage(std::uint32_t codePage) noexcept
	{
		return codePage == kCodePageGbk
			|| codePage == kCodePageBig5
			|| codePage == kCodePageShiftJis
			|| codePage == kCodePageUhc;
	}

	constexpr bool IsLeadByte(
		std::uint32_t codePage, std::uint8_t value) noexcept
	{
		switch (codePage)
		{
		case kCodePageGbk:
		case kCodePageBig5:
			return value >= 0x81 && value <= 0xFE;
		case kCodePageUhc:
			// Windows-949 is Unified Hangul Code, not the KS X 1001-only
			// subset. Its extension and CJK/PUA assignments continue through
			// lead byte 0xFE.
			return value >= 0x81 && value <= 0xFE;
		case kCodePageShiftJis:
			return (value >= 0x81 && value <= 0x9F)
				|| (value >= 0xE0 && value <= 0xFC);
		default:
			return false;
		}
	}

	constexpr bool IsTrailByte(
		std::uint32_t codePage, std::uint8_t value) noexcept
	{
		switch (codePage)
		{
		case kCodePageGbk:
			return value >= 0x40 && value <= 0xFE && value != 0x7F;
		case kCodePageBig5:
			return (value >= 0x40 && value <= 0x7E)
				|| (value >= 0xA1 && value <= 0xFE);
		case kCodePageShiftJis:
			return (value >= 0x40 && value <= 0x7E)
				|| (value >= 0x80 && value <= 0xFC);
		case kCodePageUhc:
			return (value >= 0x41 && value <= 0x5A)
				|| (value >= 0x61 && value <= 0x7A)
				|| (value >= 0x81 && value <= 0xFE);
		default:
			return false;
		}
	}

	constexpr bool IsStructurallyValidPair(std::uint32_t codePage,
		std::uint8_t lead, std::uint8_t trail) noexcept
	{
		return IsLeadByte(codePage, lead) && IsTrailByte(codePage, trail);
	}

	// The first manifest byte is a bit field. Version 13 retains the packed
	// record size while adding a data-derived empty-outline witness.
	inline constexpr std::uint8_t kGlyphManifestEntryValid = 1u << 0;
	inline constexpr std::uint8_t kGlyphManifestEntryKnownEmpty = 1u << 1;
	inline constexpr std::uint8_t kGlyphManifestEntryKnownFlags =
		kGlyphManifestEntryValid | kGlyphManifestEntryKnownEmpty;

	constexpr std::uint8_t MakeGlyphManifestEntryFlags(
		bool knownEmpty) noexcept
	{
		return kGlyphManifestEntryValid
			| (knownEmpty ? kGlyphManifestEntryKnownEmpty : 0);
	}

	constexpr bool IsGlyphManifestEntryValid(std::uint8_t flags) noexcept
	{
		return (flags & kGlyphManifestEntryValid) != 0
			&& (flags & ~kGlyphManifestEntryKnownFlags) == 0;
	}

	constexpr bool IsGlyphManifestEntryKnownEmpty(std::uint8_t flags) noexcept
	{
		return IsGlyphManifestEntryValid(flags)
			&& (flags & kGlyphManifestEntryKnownEmpty) != 0;
	}

	// A stable bitmap cache ID deliberately aliases every encoded character
	// that resolves to the same face/glyph/raster profile. Repacking may store
	// that bitmap once only when all placement semantics also agree.
	struct RepackedGlyphSemantic
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::int32_t left = 0;
		std::int32_t top = 0;
		std::int32_t effectiveWidth = 0;
		std::int32_t effectiveHeight = 0;
		std::int32_t strokeWidth26Dot6 = 0;
		std::uint32_t atlasRgb = 0;
		std::uint32_t bakedRgba = 0;
		std::uint8_t maskType = 0;
		std::uint8_t sdfSpread = 0;
		std::uint8_t colorBaked = 0;
		std::uint8_t bakedLayer = 0;

		constexpr bool operator==(
			const RepackedGlyphSemantic&) const noexcept = default;
	};
}
