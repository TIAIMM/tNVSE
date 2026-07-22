#include "font_mtsdf_generator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (condition)
			return;
		std::fprintf(stderr, "font_mtsdf_tests: %s\n", message);
		std::exit(1);
	}
}

int main()
{
	using namespace fonthook::vectorfont;
	std::array<FT_Vector, 4> points = {{
		{ 0, 0 }, { 8 * 64, 0 }, { 8 * 64, 8 * 64 }, { 0, 8 * 64 }
	}};
	std::array<unsigned char, 4> tags = {{
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON
	}};
	std::array<unsigned short, 1> contours = {{ 3 }};
	FT_Outline outline = {};
	outline.n_contours = static_cast<unsigned short>(contours.size());
	outline.n_points = static_cast<unsigned short>(points.size());
	outline.points = points.data();
	outline.tags = tags.data();
	outline.contours = contours.data();

	MtsdfBitmap bitmap;
	Check(GenerateMtsdfBitmap(outline, 4, bitmap),
		"square outline generation failed");
	Check(bitmap.width == 18 && bitmap.height == 18,
		"spread and guard bounds are incorrect");
	Check(bitmap.left == -5 && bitmap.top == 13,
		"FreeType baseline-relative bitmap bounds are incorrect");
	Check(bitmap.bgra.size() == static_cast<std::size_t>(bitmap.width)
		* bitmap.height * 4u, "MTSDF is not stored as four bytes per texel");

	const auto pixel = [&](int x, int y) -> const std::uint8_t*
	{
		return bitmap.bgra.data()
			+ (static_cast<std::size_t>(y) * bitmap.width + x) * 4u;
	};
	const std::uint8_t* center = pixel(9, 9);
	const std::uint8_t* outside = pixel(0, 0);
	Check(MedianMtsdfRgb(center) > 127 && center[3] > 127,
		"RGB median and Alpha do not classify the glyph interior consistently");
	Check(MedianMtsdfRgb(outside) < 128 && outside[3] < 128,
		"RGB median and Alpha do not classify the glyph exterior consistently");

	bool hasColoredDistance = false;
	for (std::size_t offset = 0; offset < bitmap.bgra.size(); offset += 4)
	{
		const std::uint8_t* sample = bitmap.bgra.data() + offset;
		hasColoredDistance |= sample[0] != sample[1] || sample[1] != sample[2];
	}
	Check(hasColoredDistance, "edge coloring collapsed to a monochrome SDF");

	MtsdfBitmap repeated;
	Check(GenerateMtsdfBitmap(outline, 4, repeated)
		&& repeated.width == bitmap.width && repeated.height == bitmap.height
		&& repeated.left == bitmap.left && repeated.top == bitmap.top
		&& repeated.bgra == bitmap.bgra,
		"fixed edge-coloring seed did not produce deterministic output");
	Check(!GenerateMtsdfBitmap(outline, 1, repeated),
		"unsupported spread was accepted");
	Check(!GenerateMtsdfBitmap(outline, 4, repeated, 100),
		"MTSDF byte-budget guard was not enforced");

	// Two equally oriented contours distinguish FreeType's even-odd rule from
	// the default non-zero rule. The inner contour must remain a hole.
	std::array<FT_Vector, 8> ringPoints = {{
		{ 0, 0 }, { 12 * 64, 0 }, { 12 * 64, 12 * 64 }, { 0, 12 * 64 },
		{ 3 * 64, 3 * 64 }, { 9 * 64, 3 * 64 }, { 9 * 64, 9 * 64 }, { 3 * 64, 9 * 64 }
	}};
	std::array<unsigned char, 8> ringTags = {{
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON
	}};
	std::array<unsigned short, 2> ringContours = {{ 3, 7 }};
	FT_Outline ring = {};
	ring.n_contours = static_cast<unsigned short>(ringContours.size());
	ring.n_points = static_cast<unsigned short>(ringPoints.size());
	ring.points = ringPoints.data();
	ring.tags = ringTags.data();
	ring.contours = ringContours.data();
	ring.flags = FT_OUTLINE_EVEN_ODD_FILL;
	MtsdfBitmap ringBitmap;
	Check(GenerateMtsdfBitmap(ring, 4, ringBitmap),
		"even-odd outline generation failed");
	const auto ringPixel = [&](int x, int y) -> const std::uint8_t*
	{
		return ringBitmap.bgra.data()
			+ (static_cast<std::size_t>(y) * ringBitmap.width + x) * 4u;
	};
	const std::uint8_t* ringBody = ringPixel(6, 15);
	const std::uint8_t* ringHole = ringPixel(11, 10);
	Check(MedianMtsdfRgb(ringBody) > 127 && ringBody[3] > 127,
		"even-odd ring body was classified as exterior");
	Check(MedianMtsdfRgb(ringHole) < 128 && ringHole[3] < 128,
		"FreeType even-odd fill rule was not preserved");
	return 0;
}
