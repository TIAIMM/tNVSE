#pragma once

#include <algorithm>
#include <cstdint>

namespace fonthook::vectorfont
{
	inline constexpr unsigned kMtsdfMinimumSpread = 2;
	inline constexpr unsigned kMtsdfMaximumSpread = 32;
	// Part of every distance-field bitmap, manifest, and atlas identity. Increment
	// whenever edge coloring, correction, quantization, or channel packing changes.
	inline constexpr unsigned kMtsdfGeneratorRevision = 1;
	inline constexpr double kMtsdfCornerAngleThreshold = 3.0;

	inline std::uint8_t MedianMtsdfRgb(const std::uint8_t* bgra)
	{
		if (!bgra)
			return 0;
		return std::max(std::min(bgra[0], bgra[1]),
			std::min(std::max(bgra[0], bgra[1]), bgra[2]));
	}
}
