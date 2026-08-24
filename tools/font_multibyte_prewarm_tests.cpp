#include "font_multibyte_prewarm_policy.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace policy = fonthook::multibyte_prewarm;

namespace
{
	struct CodePageExpectation
	{
		std::uint32_t codePage = 0;
		std::uint32_t structuralPairs = 0;
		std::uint32_t assignedPairs = 0;
		std::uint32_t assignedSingleBytes = 0;
		std::uint32_t uniquePairCodePoints = 0;
		std::uint32_t aliasedPairAssignments = 0;
		std::uint32_t singleDoubleCodePointOverlaps = 0;
	};

	struct MappingStatistics
	{
		std::uint32_t assignedPairs = 0;
		std::uint32_t uniquePairCodePoints = 0;
		std::uint32_t aliasedPairAssignments = 0;
		std::uint32_t singleDoubleCodePointOverlaps = 0;
	};

	bool Expect(bool condition, const char* message)
	{
		if (condition)
			return true;
		std::cerr << "FAILED: " << message << '\n';
		return false;
	}

	bool ExpectCount(std::uint32_t actual, std::uint32_t expected,
		const char* message)
	{
		if (actual == expected)
			return true;
		std::cerr << "FAILED: " << message << " (actual=" << actual
			<< ", expected=" << expected << ")\n";
		return false;
	}

	std::uint32_t CountStructuralPairs(std::uint32_t codePage)
	{
		std::uint32_t result = 0;
		for (std::uint32_t lead = 0; lead <= 0xFF; ++lead)
		{
			for (std::uint32_t trail = 0; trail <= 0xFF; ++trail)
			{
				result += policy::IsStructurallyValidPair(codePage,
					static_cast<std::uint8_t>(lead),
					static_cast<std::uint8_t>(trail)) ? 1u : 0u;
			}
		}
		return result;
	}

	std::uint32_t CountAssignedPairs(std::uint32_t codePage)
	{
		std::uint32_t result = 0;
		for (std::uint32_t lead = 0; lead <= 0xFF; ++lead)
		{
			for (std::uint32_t trail = 0; trail <= 0xFF; ++trail)
			{
				if (!policy::IsStructurallyValidPair(codePage,
					static_cast<std::uint8_t>(lead),
					static_cast<std::uint8_t>(trail)))
				{
					continue;
				}
				const char bytes[2] = {
					static_cast<char>(lead), static_cast<char>(trail)
				};
				wchar_t decoded[2] = {};
				const int count = MultiByteToWideChar(codePage,
					MB_ERR_INVALID_CHARS, bytes, 2, decoded, 2);
				if (count == 1 || (count == 2
					&& decoded[0] >= 0xD800 && decoded[0] <= 0xDBFF
					&& decoded[1] >= 0xDC00 && decoded[1] <= 0xDFFF))
				{
					++result;
				}
			}
		}
		return result;
	}

	bool TryDecode(std::uint32_t codePage, const char* bytes, int length,
		std::uint32_t& codePoint)
	{
		wchar_t decoded[2] = {};
		const int count = MultiByteToWideChar(codePage,
			MB_ERR_INVALID_CHARS, bytes, length, decoded, 2);
		if (count == 1)
		{
			codePoint = static_cast<std::uint16_t>(decoded[0]);
			return true;
		}
		if (count == 2
			&& decoded[0] >= 0xD800 && decoded[0] <= 0xDBFF
			&& decoded[1] >= 0xDC00 && decoded[1] <= 0xDFFF)
		{
			codePoint = 0x10000u
				+ ((static_cast<std::uint32_t>(decoded[0]) - 0xD800u) << 10)
				+ (static_cast<std::uint32_t>(decoded[1]) - 0xDC00u);
			return true;
		}
		return false;
	}

	MappingStatistics CollectMappingStatistics(std::uint32_t codePage)
	{
		std::unordered_map<std::uint32_t, std::uint32_t> pairAssignments;
		std::unordered_set<std::uint32_t> singleAssignments;
		for (std::uint32_t value = 0; value <= 0xFF; ++value)
		{
			const char byte = static_cast<char>(value);
			std::uint32_t codePoint = 0;
			if (TryDecode(codePage, &byte, 1, codePoint))
				singleAssignments.insert(codePoint);
		}
		MappingStatistics result;
		for (std::uint32_t lead = 0; lead <= 0xFF; ++lead)
		{
			for (std::uint32_t trail = 0; trail <= 0xFF; ++trail)
			{
				if (!policy::IsStructurallyValidPair(codePage,
					static_cast<std::uint8_t>(lead),
					static_cast<std::uint8_t>(trail)))
				{
					continue;
				}
				const char bytes[2] = {
					static_cast<char>(lead), static_cast<char>(trail)
				};
				std::uint32_t codePoint = 0;
				if (!TryDecode(codePage, bytes, 2, codePoint))
					continue;
				++result.assignedPairs;
				++pairAssignments[codePoint];
			}
		}
		result.uniquePairCodePoints =
			static_cast<std::uint32_t>(pairAssignments.size());
		result.aliasedPairAssignments =
			result.assignedPairs - result.uniquePairCodePoints;
		for (const auto& [codePoint, count] : pairAssignments)
		{
			if (count && singleAssignments.contains(codePoint))
				++result.singleDoubleCodePointOverlaps;
		}
		return result;
	}

	std::uint32_t CountAssignedSingleBytes(std::uint32_t codePage)
	{
		std::uint32_t result = 0;
		for (std::uint32_t value = 0; value <= 0xFF; ++value)
		{
			const char byte = static_cast<char>(value);
			wchar_t decoded[2] = {};
			const int count = MultiByteToWideChar(codePage,
				MB_ERR_INVALID_CHARS, &byte, 1, decoded, 2);
			if (count == 1)
				++result;
		}
		return result;
	}

	std::uint32_t CountAssignedGb2312Pairs()
	{
		constexpr std::uint32_t gb2312CodePage = 20936;
		std::uint32_t result = 0;
		for (std::uint32_t lead = 0xA1; lead <= 0xF7; ++lead)
		{
			for (std::uint32_t trail = 0xA1; trail <= 0xFE; ++trail)
			{
				const char bytes[2] = {
					static_cast<char>(lead), static_cast<char>(trail)
				};
				wchar_t decoded[2] = {};
				const int decodedCount = MultiByteToWideChar(
					gb2312CodePage, MB_ERR_INVALID_CHARS,
					bytes, 2, decoded, 2);
				if (decodedCount != 1)
					continue;
				++result;
			}
		}
		return result;
	}
}

int main()
{
	bool success = true;
	constexpr std::array expectations = {
		CodePageExpectation{ policy::kCodePageGbk,
			23940, 23940, 129, 23940, 0, 0 },
		CodePageExpectation{ policy::kCodePageBig5,
			19782, 19720, 129, 19710, 10, 0 },
		CodePageExpectation{ policy::kCodePageShiftJis,
			11280, 9604, 192, 9206, 398, 0 },
		CodePageExpectation{ policy::kCodePageUhc,
			22428, 17236, 129, 17236, 0, 0 },
	};
	for (const CodePageExpectation& expectation : expectations)
	{
		const MappingStatistics stats =
			CollectMappingStatistics(expectation.codePage);
		success &= Expect(policy::IsDbcsCodePage(expectation.codePage),
			"supported DBCS code page rejected");
		success &= ExpectCount(CountStructuralPairs(expectation.codePage),
			expectation.structuralPairs,
			"structural pair count changed");
		success &= ExpectCount(CountAssignedPairs(expectation.codePage),
			expectation.assignedPairs,
			"Windows assignment count changed");
		success &= ExpectCount(CountAssignedSingleBytes(expectation.codePage),
			expectation.assignedSingleBytes,
			"Windows single-byte assignment count changed");
		success &= ExpectCount(stats.uniquePairCodePoints,
			expectation.uniquePairCodePoints,
			"unique double-byte code-point count changed");
		success &= ExpectCount(stats.aliasedPairAssignments,
			expectation.aliasedPairAssignments,
			"double-byte alias count changed");
		success &= ExpectCount(stats.singleDoubleCodePointOverlaps,
			expectation.singleDoubleCodePointOverlaps,
			"single/double-byte code-point overlap count changed");
	}
	success &= Expect(IsValidCodePage(20936) != FALSE,
		"GB2312 validation code page unavailable");
	success &= ExpectCount(CountAssignedGb2312Pairs(), 7445,
		"GB2312 assigned-pair count changed");
	const char gb2312Alias[2] = {
		static_cast<char>(0xA1), static_cast<char>(0xAC)
	};
	const char gb2312Canonical[2] = {
		static_cast<char>(0xA1), static_cast<char>(0xCE)
	};
	std::uint32_t aliasCodePoint = 0;
	std::uint32_t canonicalCodePoint = 0;
	success &= Expect(TryDecode(20936, gb2312Alias, 2, aliasCodePoint)
		&& aliasCodePoint == 0x2225,
		"GB2312 non-canonical assigned pair rejected");
	success &= Expect(TryDecode(policy::kCodePageGbk, gb2312Alias, 2,
		aliasCodePoint) && aliasCodePoint == 0x2016,
		"CP936 identity for GB2312 0xA1AC changed");
	success &= Expect(TryDecode(policy::kCodePageGbk, gb2312Canonical, 2,
		canonicalCodePoint) && canonicalCodePoint == 0x2225
		&& canonicalCodePoint != aliasCodePoint,
		"GB2312 assignment filtering collapsed distinct CP936 glyphs");

	success &= Expect(policy::IsLeadByte(policy::kCodePageUhc, 0xC9),
		"CP949 extension lead 0xC9 omitted");
	success &= Expect(policy::IsLeadByte(policy::kCodePageUhc, 0xFE),
		"CP949 extension lead 0xFE omitted");
	success &= Expect(!policy::IsTrailByte(policy::kCodePageUhc, 0x60),
		"CP949 invalid trail 0x60 accepted");
	success &= Expect(policy::IsTrailByte(policy::kCodePageUhc, 0x61),
		"CP949 lowercase trail 0x61 rejected");

	constexpr std::uint8_t drawableFlags =
		policy::MakeGlyphManifestEntryFlags(false);
	constexpr std::uint8_t emptyFlags =
		policy::MakeGlyphManifestEntryFlags(true);
	success &= Expect(policy::IsGlyphManifestEntryValid(drawableFlags),
		"drawable manifest entry invalid");
	success &= Expect(!policy::IsGlyphManifestEntryKnownEmpty(drawableFlags),
		"drawable manifest entry marked empty");
	success &= Expect(policy::IsGlyphManifestEntryKnownEmpty(emptyFlags),
		"empty-outline manifest entry lost");
	success &= Expect(!policy::IsGlyphManifestEntryValid(0x80),
		"unknown manifest flags accepted");

	constexpr policy::RepackedGlyphSemantic fallbackQuestion{
		32, 40, -1, 38, 30, 38, 0, 0x00FFFFFF, 0,
		2, 8, 0, 0
	};
	constexpr policy::RepackedGlyphSemantic sameFallback = fallbackQuestion;
	constexpr policy::RepackedGlyphSemantic incompatibleFallback{
		33, 40, -1, 38, 30, 38, 0, 0x00FFFFFF, 0,
		2, 8, 0, 0
	};
	success &= Expect(fallbackQuestion == sameFallback,
		"compatible cross-role fallback alias rejected");
	success &= Expect(!(fallbackQuestion == incompatibleFallback),
		"incompatible cache-ID alias accepted");

	if (!success)
		return EXIT_FAILURE;
	std::cout << "font multibyte prewarm policy tests passed\n";
	return EXIT_SUCCESS;
}
