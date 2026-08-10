#include "font_vector_msdfgen_detail.h"

#include <algorithm>
#include <cmath>

namespace fonthook::vectorfont
{
	using namespace implementation::font_vector_msdfgen;

	bool GenerateMsdfgenMtsdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenMtsdfBitmap& output,
		std::size_t maximumBytes)
	{
		output = {};
		if (spread < kDistanceFieldMinimumSpread
			|| spread > kDistanceFieldMaximumSpread
			|| maximumBytes < 4)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (!PrepareShape(outline, shape))
			return false;
		// Follow msdfgen's documented library sequence. A fixed seed makes disk
		// caches and atlas snapshots reproducible.
		msdfgen::edgeColoringSimple(
			shape, kMtsdfCornerAngleThreshold, 0);

		int fieldLeft = 0;
		int fieldBottom = 0;
		int fieldTop = 0;
		int fieldWidth = 0;
		int fieldHeight = 0;
		if (!ResolveFieldBounds(shape, spread, maximumBytes, 4u,
			fieldLeft, fieldBottom, fieldTop, fieldWidth, fieldHeight))
			return false;

		msdfgen::Bitmap<float, 4> field(fieldWidth, fieldHeight);
		const msdfgen::Projection projection(
			msdfgen::Vector2(1.0),
			msdfgen::Vector2(-fieldLeft, -fieldBottom));
		const msdfgen::SDFTransformation transformation(
			projection, msdfgen::Range(
				-static_cast<double>(spread),
				static_cast<double>(spread)));
		const msdfgen::FillRule fillRule =
			outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;

		// The scanline pass preserves FreeType's fill rule, including even-odd
		// holes. As in msdfgen's standalone pipeline, generate without correction,
		// correct signs, then run the compatible edge-priority pass.
		msdfgen::MSDFGeneratorConfig generationConfig;
		generationConfig.overlapSupport = true;
		generationConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::DISABLED;
		msdfgen::generateMTSDF(
			field, shape, transformation, generationConfig);
		msdfgen::distanceSignCorrection(
			field, shape, projection, 0.5f, fillRule);
		msdfgen::MSDFGeneratorConfig correctionConfig;
		correctionConfig.overlapSupport = true;
		correctionConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY;
		correctionConfig.errorCorrection.distanceCheckMode =
			msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
		msdfgen::msdfErrorCorrection(
			field, shape, transformation, correctionConfig);
		// The renderer samples BGRA8, so all validation and later shader tests
		// must see the same quantized field.
		msdfgen::simulate8bit(field);
		// Quantization plus bilinear RGB-median reconstruction can depress a
		// narrow interior connection even when every texel-center sign is right.
		// Repair only candidates that improve exact-shape distance at every
		// affected sample and preserve the scanline contour. Alpha is never
		// touched, so effects retain the independently generated true SDF.
		RepairMtsdfRgbInterpolationDeficits(
			field, shape, projection, fillRule, spread);
		// Revision 7 appends a strictly separate rescue phase. The revision-6
		// result above is its rollback baseline: only rescue-owned RGB writes are
		// recorded, and every failure restores those original values. Alpha
		// remains the independently generated true SDF.
		RescueMtsdfRgbInterpolationDeficits(
			field, shape, projection, fillRule, spread);

		for (int y = 0; y < fieldHeight; ++y)
		{
			for (int x = 0; x < fieldWidth; ++x)
			{
				if (!IsEncodableMtsdfPixel(field(x, y)))
					return false;
			}
		}
		output.bgra.resize(static_cast<size_t>(fieldWidth)
			* fieldHeight * 4u);
		output.width = fieldWidth;
		output.height = fieldHeight;
		output.left = fieldLeft;
		output.top = fieldTop;
		for (int y = 0; y < fieldHeight; ++y)
		{
			const int sourceY = fieldHeight - 1 - y;
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float* source = field(x, sourceY);
				std::uint8_t* destination = output.bgra.data()
					+ (static_cast<size_t>(y) * fieldWidth + x) * 4u;
				// D3DFMT_A8R8G8B8 is BGRA in little-endian memory.
				destination[0] = msdfgen::pixelFloatToByte(source[2]);
				destination[1] = msdfgen::pixelFloatToByte(source[1]);
				destination[2] = msdfgen::pixelFloatToByte(source[0]);
				destination[3] = msdfgen::pixelFloatToByte(source[3]);
			}
		}
		return true;
	}

	bool GenerateMsdfgenTrueSdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenSdfBitmap& output,
		std::size_t maximumBytes)
	{
		output = {};
		if (spread < kDistanceFieldMinimumSpread
			|| spread > kDistanceFieldMaximumSpread
			|| maximumBytes < 1)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (!PrepareShape(outline, shape))
			return false;
		int fieldLeft = 0;
		int fieldBottom = 0;
		int fieldTop = 0;
		int fieldWidth = 0;
		int fieldHeight = 0;
		if (!ResolveFieldBounds(shape, spread, maximumBytes, 1u,
			fieldLeft, fieldBottom, fieldTop, fieldWidth, fieldHeight))
		{
			return false;
		}

		msdfgen::Bitmap<float, 1> field(fieldWidth, fieldHeight);
		const msdfgen::Projection projection(
			msdfgen::Vector2(1.0),
			msdfgen::Vector2(-fieldLeft, -fieldBottom));
		const msdfgen::SDFTransformation transformation(
			projection, msdfgen::Range(
				-static_cast<double>(spread),
				static_cast<double>(spread)));
		const msdfgen::FillRule fillRule =
			outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;
		msdfgen::generateSDF(
			field, shape, transformation, msdfgen::GeneratorConfig(true));
		msdfgen::distanceSignCorrection(
			field, shape, projection, 0.5f, fillRule);
		for (int y = 0; y < fieldHeight; ++y)
			for (int x = 0; x < fieldWidth; ++x)
				if (!std::isfinite(*field(x, y)))
					return false;

		// Keep the single output allocation in msdfgen's bottom-up coordinate
		// system until repair is complete, then invert rows in place.
		output.pixels.resize(static_cast<std::size_t>(fieldWidth) * fieldHeight);
		output.width = fieldWidth;
		output.height = fieldHeight;
		output.left = fieldLeft;
		output.top = fieldTop;
		for (int y = 0; y < fieldHeight; ++y)
		{
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float encoded = *field(x, y);
				// The true-SDF shader decodes byte N as
				// (N / 128 - 1) * spread, with byte 128 as exact zero.
				// Its nearest-integer inverse is round(256 * encoded), not
				// the conventional normalized-UNORM round(255 * encoded).
				// Quantize to that deployed contract here; MTSDF remains on
				// the conventional 255 scale used by its 0.5-centered decoder.
				const long runtimeByte = std::lround(
					std::clamp(static_cast<double>(encoded), 0.0, 1.0)
					* 256.0);
				output.pixels[static_cast<std::size_t>(y) * fieldWidth + x] =
					static_cast<std::uint8_t>(std::clamp(runtimeByte, 0L, 255L));
			}
		}
		// Byte-128-centered quantization is not uniformly optimal under the
		// shader's bilinear decoder.  Before the top-down output flip, consider
		// only +/-1 byte corrections that pass continuous-center prefiltering,
		// exact-center confirmation, exact 4x distance/coverage checks, an
		// independent 8x sign check, and the final contour-error rollback gate.
		RepairTrueSdfQuantization(output.pixels, field, shape, projection,
			fillRule, spread);

		for (int y = 0; y < fieldHeight / 2; ++y)
		{
			std::swap_ranges(output.pixels.begin()
					+ static_cast<std::size_t>(y) * fieldWidth,
				output.pixels.begin()
					+ static_cast<std::size_t>(y + 1) * fieldWidth,
				output.pixels.begin()
					+ static_cast<std::size_t>(fieldHeight - 1 - y) * fieldWidth);
		}
		return true;
	}
}
