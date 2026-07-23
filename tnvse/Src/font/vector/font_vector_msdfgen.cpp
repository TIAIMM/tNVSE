#include "font_vector_msdfgen.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <msdfgen.h>
#include <ext/import-font.h>

namespace fonthook::vectorfont
{
	namespace
	{
		float MedianMtsdfRgb(float red, float green, float blue)
		{
			return std::max(std::min(red, green),
				std::min(std::max(red, green), blue));
		}

		bool IsEncodableMtsdfPixel(const float* rgba)
		{
			if (!rgba || std::isnan(rgba[0]) || std::isnan(rgba[1])
				|| std::isnan(rgba[2]) || !std::isfinite(rgba[3]))
			{
				return false;
			}
			// An unused edge-color channel may be infinite. Saturating that
			// channel is valid when the RGB median and Alpha remain finite.
			return std::isfinite(MedianMtsdfRgb(
				rgba[0], rgba[1], rgba[2]));
		}
	}

	bool GenerateMsdfgenMtsdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenMtsdfBitmap& output,
		std::size_t maximumBytes)
	{
		output = {};
		if (spread < kMtsdfMinimumSpread || spread > kMtsdfMaximumSpread
			|| maximumBytes < 4)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (msdfgen::readFreetypeOutline(shape, &outline, 1.0 / 64.0)
			|| shape.contours.empty())
		{
			return false;
		}
		// tNVSE's shader contract is positive-inside. Normalize the outer winding
		// before edge coloring so TrueType and PostScript/CFF faces produce the
		// same channel/sign contract.
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
		if (!shape.validate())
			return false;
		// Follow msdfgen's documented library sequence. A fixed seed makes disk
		// caches and atlas snapshots reproducible.
		msdfgen::edgeColoringSimple(
			shape, kMtsdfCornerAngleThreshold, 0);

		const msdfgen::Shape::Bounds bounds = shape.getBounds();
		if (!std::isfinite(bounds.l) || !std::isfinite(bounds.b)
			|| !std::isfinite(bounds.r) || !std::isfinite(bounds.t)
			|| bounds.l > bounds.r || bounds.b > bounds.t
			|| bounds.l < std::numeric_limits<int>::min() + 64.0
			|| bounds.b < std::numeric_limits<int>::min() + 64.0
			|| bounds.r > std::numeric_limits<int>::max() - 64.0
			|| bounds.t > std::numeric_limits<int>::max() - 64.0)
		{
			return false;
		}

		// One extra outside-distance texel protects the glyph field from
		// bilinear samples that reach the atlas padding.
		const std::int64_t guard = static_cast<std::int64_t>(spread) + 1;
		const std::int64_t fieldLeft64 =
			static_cast<std::int64_t>(std::floor(bounds.l)) - guard;
		const std::int64_t fieldBottom64 =
			static_cast<std::int64_t>(std::floor(bounds.b)) - guard;
		const std::int64_t fieldRight64 =
			static_cast<std::int64_t>(std::ceil(bounds.r)) + guard;
		const std::int64_t fieldTop64 =
			static_cast<std::int64_t>(std::ceil(bounds.t)) + guard;
		const std::int64_t fieldWidth64 = fieldRight64 - fieldLeft64;
		const std::int64_t fieldHeight64 = fieldTop64 - fieldBottom64;
		if (fieldWidth64 <= 0 || fieldHeight64 <= 0
			|| fieldWidth64 > 4096 || fieldHeight64 > 4096
			|| static_cast<std::uint64_t>(fieldWidth64)
				* static_cast<std::uint64_t>(fieldHeight64)
				> maximumBytes / 4u)
		{
			return false;
		}
		const int fieldLeft = static_cast<int>(fieldLeft64);
		const int fieldBottom = static_cast<int>(fieldBottom64);
		const int fieldTop = static_cast<int>(fieldTop64);
		const int fieldWidth = static_cast<int>(fieldWidth64);
		const int fieldHeight = static_cast<int>(fieldHeight64);

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

		output.width = fieldWidth;
		output.height = fieldHeight;
		output.left = fieldLeft;
		output.top = fieldTop;
		output.bgra.resize(static_cast<size_t>(fieldWidth)
			* fieldHeight * 4u);
		for (int y = 0; y < fieldHeight; ++y)
		{
			const int sourceY = fieldHeight - 1 - y;
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float* source = field(x, sourceY);
				if (!IsEncodableMtsdfPixel(source))
					return false;
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
}
