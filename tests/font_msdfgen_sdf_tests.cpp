#include "font_vector_msdfgen.h"

#include <array>
#include <cstdint>
#include <iostream>

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			std::cerr << "CHECK failed: " #condition \
				<< " at line " << __LINE__ << '\n'; \
			return 1; \
		} \
	} while (false)

int main()
{
	std::array<FT_Vector, 4> points = {{
		{ 0, 0 },
		{ 0, 10 * 64 },
		{ 10 * 64, 10 * 64 },
		{ 10 * 64, 0 },
	}};
	std::array<FT_Byte, 4> tags = {{
		FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON,
	}};
	std::array<FT_UShort, 1> contours = {{ 3 }};
	FT_Outline outline = {};
	outline.n_contours = 1;
	outline.n_points = 4;
	outline.points = points.data();
	outline.tags = tags.data();
	outline.contours = contours.data();

	fonthook::vectorfont::MsdfgenSdfBitmap sdf;
	CHECK(fonthook::vectorfont::GenerateMsdfgenTrueSdf(
		outline, 4, sdf));
	CHECK(sdf.width == 20);
	CHECK(sdf.height == 20);
	CHECK(sdf.left == -5);
	CHECK(sdf.top == 15);
	CHECK(sdf.pixels.size() == 400);

	auto pixel = [&](int x, int y) -> std::uint8_t
	{
		return sdf.pixels[static_cast<size_t>(y) * sdf.width + x];
	};
	CHECK(pixel(0, 0) == 0);
	CHECK(pixel(10, 10) > 128);
	CHECK(pixel(2, 10) < 128);
	CHECK(pixel(17, 10) < 128);
	CHECK(pixel(10, 2) < 128);
	CHECK(pixel(10, 17) < 128);

	// PostScript/CFF outlines use the opposite outer-contour winding. The
	// adapter must still preserve tNVSE's positive-inside shader contract.
	std::array<FT_Vector, 4> postScriptPoints = {{
		{ 0, 0 },
		{ 10 * 64, 0 },
		{ 10 * 64, 10 * 64 },
		{ 0, 10 * 64 },
	}};
	FT_Outline postScriptOutline = outline;
	postScriptOutline.points = postScriptPoints.data();
	fonthook::vectorfont::MsdfgenSdfBitmap postScriptSdf;
	CHECK(fonthook::vectorfont::GenerateMsdfgenTrueSdf(
		postScriptOutline, 4, postScriptSdf));
	CHECK(postScriptSdf.width == sdf.width);
	CHECK(postScriptSdf.height == sdf.height);
	CHECK(postScriptSdf.pixels[static_cast<size_t>(10)
		* postScriptSdf.width + 10] > 128);

	fonthook::vectorfont::MsdfgenSdfBitmap invalid;
	CHECK(!fonthook::vectorfont::GenerateMsdfgenTrueSdf(
		outline, 1, invalid));

	std::array<FT_Vector, 8> ringPoints = {{
		{ 0, 0 },
		{ 0, 10 * 64 },
		{ 10 * 64, 10 * 64 },
		{ 10 * 64, 0 },
		{ 3 * 64, 3 * 64 },
		{ 7 * 64, 3 * 64 },
		{ 7 * 64, 7 * 64 },
		{ 3 * 64, 7 * 64 },
	}};
	std::array<FT_Byte, 8> ringTags = {{
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
	}};
	std::array<FT_UShort, 2> ringContours = {{ 3, 7 }};
	FT_Outline ringOutline = {};
	ringOutline.n_contours = 2;
	ringOutline.n_points = 8;
	ringOutline.points = ringPoints.data();
	ringOutline.tags = ringTags.data();
	ringOutline.contours = ringContours.data();

	fonthook::vectorfont::MsdfgenSdfBitmap ring;
	CHECK(fonthook::vectorfont::GenerateMsdfgenTrueSdf(
		ringOutline, 4, ring));
	auto ringPixel = [&](int x, int y) -> std::uint8_t
	{
		return ring.pixels[static_cast<size_t>(y) * ring.width + x];
	};
	CHECK(ringPixel(7, 10) > 128);
	CHECK(ringPixel(10, 10) < 128);
	CHECK(ringPixel(2, 10) < 128);

	std::cout << "font_msdfgen_sdf_tests: passed\n";
	return 0;
}
