#include "font_vector_msdfgen_detail.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <ext/import-font.h>

namespace fonthook::vectorfont::implementation::font_vector_msdfgen
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

		bool PrepareShape(FT_Outline& outline, msdfgen::Shape& shape)
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
		bool ResolveFieldBounds(const msdfgen::Shape& shape,
			std::uint8_t spread, std::size_t maximumBytes,
			std::uint32_t bytesPerPixel, int& fieldLeft, int& fieldBottom,
			int& fieldTop, int& fieldWidth, int& fieldHeight)
		{
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
			if (!bytesPerPixel || maximumBytes < bytesPerPixel
				|| fieldWidth64 <= 0 || fieldHeight64 <= 0
				|| fieldWidth64 > 4096 || fieldHeight64 > 4096
				|| static_cast<std::uint64_t>(fieldWidth64)
					* static_cast<std::uint64_t>(fieldHeight64)
					> maximumBytes / bytesPerPixel)
			{
				return false;
			}
			fieldLeft = static_cast<int>(fieldLeft64);
			fieldBottom = static_cast<int>(fieldBottom64);
			fieldTop = static_cast<int>(fieldTop64);
			fieldWidth = static_cast<int>(fieldWidth64);
			fieldHeight = static_cast<int>(fieldHeight64);
			return true;
		}
}
