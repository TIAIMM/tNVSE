#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <ft2build.h>
#include FT_OUTLINE_H

namespace fonthook::vectorfont
{
	enum class DistanceFieldMethod : std::uint8_t
	{
		TrueSdf = 0,
		Mtsdf = 1,
	};

	inline constexpr std::uint8_t kMtsdfMinimumSpread = 2;
	inline constexpr std::uint8_t kMtsdfMaximumSpread = 32;
	inline constexpr std::uint32_t kTrueSdfGeneratorRevision = 2;
	// Part of bitmap, persistent-cache, atlas, and prewarm identity. Revision 4
	// is the RGBA8 contract: unhinted outlines, deterministic simple edge
	// coloring, scanline sign correction, post-correction, and 8-bit simulation.
	inline constexpr std::uint32_t kMtsdfGeneratorRevision = 4;
	inline constexpr double kMtsdfCornerAngleThreshold = 3.0;

	inline constexpr std::uint32_t DistanceFieldBytesPerPixel(
		DistanceFieldMethod method)
	{
		return method == DistanceFieldMethod::Mtsdf ? 4u : 1u;
	}

	inline constexpr std::uint32_t DistanceFieldGeneratorRevision(
		DistanceFieldMethod method)
	{
		return method == DistanceFieldMethod::Mtsdf
			? kMtsdfGeneratorRevision : kTrueSdfGeneratorRevision;
	}

	struct MsdfgenSdfBitmap
	{
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		std::vector<std::uint8_t> pixels;
	};

	struct MsdfgenMtsdfBitmap
	{
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		// D3DFMT_A8R8G8B8 byte order on little-endian Windows. RGB carries the
		// multi-channel field used for the body; Alpha carries true signed
		// distance used exclusively by effects.
		std::vector<std::uint8_t> bgra;
	};

	inline std::uint8_t MedianMtsdfRgb(const std::uint8_t* bgra)
	{
		if (!bgra)
			return 0;
		return std::max(std::min(bgra[0], bgra[1]),
			std::min(std::max(bgra[0], bgra[1]), bgra[2]));
	}

	bool GenerateMsdfgenMtsdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenMtsdfBitmap& output,
		std::size_t maximumBytes = 64u * 1024u * 1024u);

	bool GenerateMsdfgenTrueSdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenSdfBitmap& output,
		std::size_t maximumBytes = 16u * 1024u * 1024u);
}
