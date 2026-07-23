#include "font_vector_msdfgen.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <msdfgen.h>
#include <ext/import-font.h>

namespace fonthook::vectorfont
{
	bool GenerateMsdfgenTrueSdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenSdfBitmap& output)
	{
		output = {};
		if (spread < 2 || spread > 32)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (msdfgen::readFreetypeOutline(shape, &outline, 1.0 / 64.0)
			|| !shape.validate())
		{
			return false;
		}
		// tNVSE's shader contract is positive-inside. FreeType preserves the
		// opposite outer-contour winding used by PostScript/CFF outlines, so
		// normalize it to the TrueType winding expected by that contract.
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

		const std::int64_t fieldLeft64 =
			static_cast<std::int64_t>(std::floor(bounds.l)) - spread;
		const std::int64_t fieldBottom64 =
			static_cast<std::int64_t>(std::floor(bounds.b)) - spread;
		const std::int64_t fieldRight64 =
			static_cast<std::int64_t>(std::ceil(bounds.r)) + spread;
		const std::int64_t fieldTop64 =
			static_cast<std::int64_t>(std::ceil(bounds.t)) + spread;
		const std::int64_t fieldWidth64 = fieldRight64 - fieldLeft64;
		const std::int64_t fieldHeight64 = fieldTop64 - fieldBottom64;
		constexpr std::uint64_t kMaximumBitmapBytes = 16ull * 1024ull * 1024ull;
		if (fieldWidth64 <= 0 || fieldHeight64 <= 0
			|| fieldWidth64 > 65533 || fieldHeight64 > 65533
			|| static_cast<std::uint64_t>(fieldWidth64 + 2)
				* static_cast<std::uint64_t>(fieldHeight64 + 2)
				> kMaximumBitmapBytes)
		{
			return false;
		}
		const int fieldLeft = static_cast<int>(fieldLeft64);
		const int fieldBottom = static_cast<int>(fieldBottom64);
		const int fieldTop = static_cast<int>(fieldTop64);
		const int fieldWidth = static_cast<int>(fieldWidth64);
		const int fieldHeight = static_cast<int>(fieldHeight64);

		msdfgen::Bitmap<float, 1> field(fieldWidth, fieldHeight);
		const msdfgen::SDFTransformation transformation(
			msdfgen::Projection(
				msdfgen::Vector2(1.0),
				msdfgen::Vector2(-fieldLeft, -fieldBottom)),
			msdfgen::Range(-static_cast<double>(spread),
				static_cast<double>(spread)));
		msdfgen::generateSDF(
			field, shape, transformation, msdfgen::GeneratorConfig(true));

		// tNVSE's A8 atlas is top-down; msdfgen stores a Y-up bitmap. Preserve
		// the existing transparent guard and 8-bit shader contract.
		constexpr int kBitmapGuardPixels = 1;
		output.width = fieldWidth + kBitmapGuardPixels * 2;
		output.height = fieldHeight + kBitmapGuardPixels * 2;
		output.left = fieldLeft - kBitmapGuardPixels;
		output.top = fieldTop + kBitmapGuardPixels;
		output.pixels.assign(
			static_cast<size_t>(output.width) * output.height, 0);
		for (int y = 0; y < fieldHeight; ++y)
		{
			const int sourceY = fieldHeight - 1 - y;
			std::uint8_t* destination = output.pixels.data()
				+ static_cast<size_t>(y + kBitmapGuardPixels) * output.width
				+ kBitmapGuardPixels;
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float encoded = std::clamp(*field(x, sourceY), 0.0f, 1.0f);
				destination[x] = static_cast<std::uint8_t>(
					std::lround(encoded * 255.0f));
			}
		}
		return true;
	}
}
