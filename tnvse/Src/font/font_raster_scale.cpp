#include "font_vector.h"
#include "load_config.h"

#include <algorithm>
#include <cmath>

namespace fonthook::implementation::font_raster_scale
{
	constexpr float kMinimumRasterScale = 0.1f;
	constexpr float kMaximumRasterScale = 10.0f;
	constexpr int kRasterScalePrecision = 1000;

	int CanonicalScaleMilli(float scale)
	{
		if (!std::isfinite(scale))
			scale = 1.0f;
		const float clamped = std::max(kMinimumRasterScale,
			std::min(kMaximumRasterScale, scale));
		return std::max(1, static_cast<int>(std::lround(
			clamped * static_cast<float>(kRasterScalePrecision))));
	}
}

namespace fonthook
{
	float GetCanonicalFreeTypeRasterScale()
	{
		return static_cast<float>(
			implementation::font_raster_scale::CanonicalScaleMilli(
				g_fFreeTypeFontResolutionScale))
			/ implementation::font_raster_scale::kRasterScalePrecision;
	}
}
