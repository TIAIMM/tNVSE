#include "font_vector_msdfgen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <msdfgen.h>
#include <ext/import-font.h>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (condition)
			return;
		std::fprintf(stderr, "font_mtsdf_tests: %s\n", message);
		std::exit(1);
	}

	float Median(float red, float green, float blue)
	{
		return std::max(std::min(red, green),
			std::min(std::max(red, green), blue));
	}

	std::array<float, 4> SamplePacked(
		const fonthook::vectorfont::MsdfgenMtsdfBitmap& bitmap,
		double shapeX, double shapeY)
	{
		const int bottom = bitmap.top - bitmap.height;
		const double sampleX = shapeX - bitmap.left - 0.5;
		const double sampleY = shapeY - bottom - 0.5;
		const int x0 = std::clamp(static_cast<int>(std::floor(sampleX)),
			0, bitmap.width - 1);
		const int y0 = std::clamp(static_cast<int>(std::floor(sampleY)),
			0, bitmap.height - 1);
		const int x1 = std::min(x0 + 1, bitmap.width - 1);
		const int y1 = std::min(y0 + 1, bitmap.height - 1);
		const float tx = static_cast<float>(sampleX - std::floor(sampleX));
		const float ty = static_cast<float>(sampleY - std::floor(sampleY));
		auto texel = [&](int x, int y)
		{
			const int storedY = bitmap.height - 1 - y;
			const std::uint8_t* bgra = bitmap.bgra.data()
				+ (static_cast<std::size_t>(storedY) * bitmap.width + x) * 4u;
			return std::array<float, 4>{
				bgra[2] / 255.0f, bgra[1] / 255.0f,
				bgra[0] / 255.0f, bgra[3] / 255.0f
			};
		};
		const auto p00 = texel(x0, y0);
		const auto p10 = texel(x1, y0);
		const auto p01 = texel(x0, y1);
		const auto p11 = texel(x1, y1);
		std::array<float, 4> result = {};
		for (int channel = 0; channel < 4; ++channel)
		{
			const float bottomValue =
				p00[channel] + (p10[channel] - p00[channel]) * tx;
			const float topValue =
				p01[channel] + (p11[channel] - p01[channel]) * tx;
			result[channel] =
				bottomValue + (topValue - bottomValue) * ty;
		}
		return result;
	}

	float SampleTrueSdf(
		const fonthook::vectorfont::MsdfgenSdfBitmap& bitmap,
		double shapeX, double shapeY)
	{
		const int bottom = bitmap.top - bitmap.height;
		const double sampleX = shapeX - bitmap.left - 0.5;
		const double sampleY = shapeY - bottom - 0.5;
		const int x0 = std::clamp(static_cast<int>(std::floor(sampleX)),
			0, bitmap.width - 1);
		const int y0 = std::clamp(static_cast<int>(std::floor(sampleY)),
			0, bitmap.height - 1);
		const int x1 = std::min(x0 + 1, bitmap.width - 1);
		const int y1 = std::min(y0 + 1, bitmap.height - 1);
		const float tx = static_cast<float>(sampleX - std::floor(sampleX));
		const float ty = static_cast<float>(sampleY - std::floor(sampleY));
		auto texel = [&](int x, int y)
		{
			const int storedY = bitmap.height - 1 - y;
			return bitmap.pixels[static_cast<std::size_t>(storedY)
				* bitmap.width + x] / 255.0f;
		};
		const float bottomValue =
			texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * tx;
		const float topValue =
			texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * tx;
		return bottomValue + (topValue - bottomValue) * ty;
	}

	bool BuildNormalizedShape(FT_Outline& outline, msdfgen::Shape& shape)
	{
		if (msdfgen::readFreetypeOutline(shape, &outline, 1.0 / 64.0)
			|| shape.contours.empty())
		{
			return false;
		}
		const FT_Orientation orientation = FT_Outline_Get_Orientation(&outline);
		if (orientation == FT_ORIENTATION_POSTSCRIPT)
		{
			for (msdfgen::Contour& contour : shape.contours)
				contour.reverse();
		}
		else if (orientation != FT_ORIENTATION_TRUETYPE)
		{
			shape.orientContours();
		}
		shape.normalize();
		return shape.validate();
	}

	void CheckSubpixelTopology(FT_Outline& outline,
		const fonthook::vectorfont::MsdfgenMtsdfBitmap& bitmap,
		unsigned spread, const char* label)
	{
		msdfgen::Shape shape;
		Check(BuildNormalizedShape(outline, shape),
			"could not rebuild normalized outline");
		const msdfgen::FillRule fillRule =
			outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;
		const auto bounds = shape.getBounds();
		const double encodedQuarterPixel =
			0.25 / (2.0 * static_cast<double>(spread));
		std::uint64_t confidentSamples = 0;
		std::uint64_t alphaErrors = 0;
		std::uint64_t rgbErrors = 0;
		for (double y = std::floor(bounds.b) - 1.0;
			y <= std::ceil(bounds.t) + 1.0; y += 0.125)
		{
			msdfgen::Scanline scanline;
			shape.scanline(scanline, y);
			for (double x = std::floor(bounds.l) - 1.0;
				x <= std::ceil(bounds.r) + 1.0; x += 0.125)
			{
				const auto sample = SamplePacked(bitmap, x, y);
				if (std::abs(sample[3] - 0.5f) < encodedQuarterPixel)
					continue;
				const bool exactInside = scanline.filled(x, fillRule);
				const bool alphaInside = sample[3] > 0.5f;
				const float rgbMedian =
					Median(sample[0], sample[1], sample[2]);
				const bool rgbInside = rgbMedian > 0.5f;
				++confidentSamples;
				alphaErrors += alphaInside != exactInside;
				// Only count an RGB failure where Alpha and the exact outline
				// agree. This isolates multi-channel interpolation artifacts from
				// samples genuinely inside the contour's AA footprint.
				if (alphaInside == exactInside && rgbInside != exactInside
					&& std::abs(rgbMedian - 0.5f) >= encodedQuarterPixel)
				{
					if (rgbErrors < 12)
					{
						std::fprintf(stderr,
							"font_mtsdf_tests: %s mismatch x=%.3f y=%.3f exact=%u rgb=%.6f alpha=%.6f\n",
							label, x, y, exactInside ? 1u : 0u,
							rgbMedian, sample[3]);
					}
					++rgbErrors;
				}
			}
		}
		std::fprintf(stderr,
			"font_mtsdf_tests: %s confident=%llu alphaErrors=%llu rgbErrors=%llu\n",
			label, static_cast<unsigned long long>(confidentSamples),
			static_cast<unsigned long long>(alphaErrors),
			static_cast<unsigned long long>(rgbErrors));
		Check(confidentSamples != 0, "subpixel topology probe sampled no points");
		Check(rgbErrors == 0,
			"quantized bilinear RGB median inverted a confident contour sample");
	}

	void ProbeFuturaNIfAvailable()
	{
		const char* path = std::getenv("TNVSE_MTSDF_PROBE_FONT");
		if (!path || !*path)
			return;
		FT_Library library = nullptr;
		FT_Face face = nullptr;
		Check(!FT_Init_FreeType(&library) && library,
			"could not initialize FreeType for real-font probe");
		Check(!FT_New_Face(library, path, 0, &face) && face,
			"could not open real-font probe");
		Check(!FT_Set_Pixel_Sizes(face, 52, 52),
			"could not set Futura regression size");
		const FT_UInt glyphIndex = FT_Get_Char_Index(face, 'N');
		Check(glyphIndex != 0, "real-font probe has no N glyph");
		Check(!FT_Load_Glyph(face, glyphIndex,
			FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING
				| FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG),
			"could not load Futura N outline");
		fonthook::vectorfont::MsdfgenMtsdfBitmap bitmap;
		Check(fonthook::vectorfont::GenerateMsdfgenMtsdf(
			face->glyph->outline, 5, bitmap),
			"could not generate Futura N MTSDF");
		CheckSubpixelTopology(face->glyph->outline, bitmap, 5, "Futura-N");
		FT_Done_Face(face);
		FT_Done_FreeType(library);
	}
}

int main()
{
	using namespace fonthook::vectorfont;
	Check(kMtsdfGeneratorRevision == 4,
		"unexpected MTSDF generator/cache revision");
	Check(kTrueSdfGeneratorRevision == 2,
		"unexpected true-SDF generator/cache revision");
	Check(DistanceFieldBytesPerPixel(DistanceFieldMethod::TrueSdf) == 1
		&& DistanceFieldBytesPerPixel(DistanceFieldMethod::Mtsdf) == 4,
		"distance-field storage selection is incorrect");
	Check(DistanceFieldGeneratorRevision(DistanceFieldMethod::TrueSdf)
			== kTrueSdfGeneratorRevision
		&& DistanceFieldGeneratorRevision(DistanceFieldMethod::Mtsdf)
			== kMtsdfGeneratorRevision,
		"distance-field revision selection is incorrect");

	std::array<FT_Vector, 10> nPoints = {{
		{ 0, 0 }, { 2 * 64, 0 }, { 8 * 64, 9 * 64 },
		{ 8 * 64, 0 }, { 10 * 64, 0 }, { 10 * 64, 12 * 64 },
		{ 8 * 64, 12 * 64 }, { 2 * 64, 3 * 64 },
		{ 2 * 64, 12 * 64 }, { 0, 12 * 64 }
	}};
	std::array<unsigned char, 10> nTags = {{
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
		FT_CURVE_TAG_ON
	}};
	std::array<unsigned short, 1> nContours = {{ 9 }};
	FT_Outline nOutline = {};
	nOutline.n_contours = static_cast<unsigned short>(nContours.size());
	nOutline.n_points = static_cast<unsigned short>(nPoints.size());
	nOutline.points = nPoints.data();
	nOutline.tags = nTags.data();
	nOutline.contours = nContours.data();

	MsdfgenMtsdfBitmap bitmap;
	Check(GenerateMsdfgenMtsdf(nOutline, 4, bitmap),
		"synthetic N generation failed");
	Check(bitmap.width == 20 && bitmap.height == 22,
		"spread and outside guard bounds are incorrect");
	Check(bitmap.left == -5 && bitmap.top == 17,
		"baseline-relative MTSDF bounds are incorrect");
	Check(bitmap.bgra.size() == static_cast<std::size_t>(bitmap.width)
		* bitmap.height * 4u, "MTSDF is not four bytes per texel");

	bool hasColoredDistance = false;
	for (std::size_t offset = 0; offset < bitmap.bgra.size(); offset += 4)
	{
		const std::uint8_t* sample = bitmap.bgra.data() + offset;
		hasColoredDistance |= sample[0] != sample[1]
			|| sample[1] != sample[2];
	}
	Check(hasColoredDistance, "RGB field collapsed to monochrome true SDF");
	CheckSubpixelTopology(nOutline, bitmap, 4, "synthetic-N");

	MsdfgenMtsdfBitmap repeated;
	Check(GenerateMsdfgenMtsdf(nOutline, 4, repeated)
		&& repeated.width == bitmap.width && repeated.height == bitmap.height
		&& repeated.left == bitmap.left && repeated.top == bitmap.top
		&& repeated.bgra == bitmap.bgra,
		"fixed edge-coloring seed did not produce deterministic bytes");
	Check(!GenerateMsdfgenMtsdf(nOutline, 1, repeated),
		"unsupported spread was accepted");
	Check(!GenerateMsdfgenMtsdf(nOutline, 4, repeated, 100),
		"four-channel byte budget was not enforced");

	MsdfgenSdfBitmap trueSdf;
	Check(GenerateMsdfgenTrueSdf(nOutline, 4, trueSdf),
		"synthetic N true-SDF generation failed");
	Check(trueSdf.width == bitmap.width && trueSdf.height == bitmap.height
		&& trueSdf.left == bitmap.left && trueSdf.top == bitmap.top,
		"true-SDF and MTSDF field bounds diverged");
	Check(trueSdf.pixels.size() == static_cast<std::size_t>(trueSdf.width)
		* trueSdf.height, "true SDF is not one byte per texel");
	Check(std::any_of(trueSdf.pixels.begin(), trueSdf.pixels.end(),
			[](std::uint8_t value) { return value < 128; })
		&& std::any_of(trueSdf.pixels.begin(), trueSdf.pixels.end(),
			[](std::uint8_t value) { return value > 128; }),
		"true SDF does not contain signed interior and exterior distances");
	MsdfgenSdfBitmap repeatedTrueSdf;
	Check(GenerateMsdfgenTrueSdf(nOutline, 4, repeatedTrueSdf)
		&& repeatedTrueSdf.pixels == trueSdf.pixels,
		"true-SDF generation is not deterministic");
	Check(!GenerateMsdfgenTrueSdf(nOutline, 4, repeatedTrueSdf, 100),
		"single-channel byte budget was not enforced");

	// Equally oriented contours require FreeType's even-odd fill rule for a hole.
	std::array<FT_Vector, 8> ringPoints = {{
		{ 0, 0 }, { 12 * 64, 0 }, { 12 * 64, 12 * 64 }, { 0, 12 * 64 },
		{ 3 * 64, 3 * 64 }, { 9 * 64, 3 * 64 },
		{ 9 * 64, 9 * 64 }, { 3 * 64, 9 * 64 }
	}};
	std::array<unsigned char, 8> ringTags;
	ringTags.fill(FT_CURVE_TAG_ON);
	std::array<unsigned short, 2> ringContours = {{ 3, 7 }};
	FT_Outline ring = {};
	ring.n_contours = static_cast<unsigned short>(ringContours.size());
	ring.n_points = static_cast<unsigned short>(ringPoints.size());
	ring.points = ringPoints.data();
	ring.tags = ringTags.data();
	ring.contours = ringContours.data();
	ring.flags = FT_OUTLINE_EVEN_ODD_FILL;
	MsdfgenMtsdfBitmap ringBitmap;
	Check(GenerateMsdfgenMtsdf(ring, 4, ringBitmap),
		"even-odd ring generation failed");
	const auto body = SamplePacked(ringBitmap, 1.5, 6.0);
	const auto hole = SamplePacked(ringBitmap, 6.0, 6.0);
	Check(Median(body[0], body[1], body[2]) > 0.5f && body[3] > 0.5f,
		"ring body was classified as exterior");
	Check(Median(hole[0], hole[1], hole[2]) < 0.5f && hole[3] < 0.5f,
		"even-odd hole was classified as interior");
	MsdfgenSdfBitmap ringTrueSdf;
	Check(GenerateMsdfgenTrueSdf(ring, 4, ringTrueSdf),
		"even-odd ring true-SDF generation failed");
	Check(SampleTrueSdf(ringTrueSdf, 1.5, 6.0) > 0.5f,
		"true-SDF ring body was classified as exterior");
	Check(SampleTrueSdf(ringTrueSdf, 6.0, 6.0) < 0.5f,
		"true-SDF even-odd hole was classified as interior");

	ProbeFuturaNIfAvailable();
	return 0;
}
