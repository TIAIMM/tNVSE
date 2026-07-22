#pragma once

#include "font_mtsdf_contract.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen-ext.h>
#include <msdfgen.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	struct MtsdfBitmap
	{
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		// D3D9-native BGRA bytes. The three distance channels are symmetric,
		// while alpha contains msdfgen's quantized true signed distance.
		std::vector<std::uint8_t> bgra;
	};

	inline float MedianMtsdfRgb(float red, float green, float blue)
	{
		return std::max(std::min(red, green),
			std::min(std::max(red, green), blue));
	}

	inline bool IsMtsdfPixelEncodable(const float* rgba)
	{
		if (!rgba || std::isnan(rgba[0]) || std::isnan(rgba[1])
			|| std::isnan(rgba[2]) || !std::isfinite(rgba[3]))
		{
			return false;
		}
		// An unused colored edge channel may be infinite. Its saturated byte is
		// valid as long as the RGB median and true-distance alpha stay finite.
		return std::isfinite(MedianMtsdfRgb(rgba[0], rgba[1], rgba[2]));
	}

	inline bool GenerateMtsdfBitmap(FT_Outline& outline, unsigned spread,
		MtsdfBitmap& result, std::size_t maximumBytes = 16u * 1024u * 1024u)
	{
		result = {};
		if (!outline.n_points || !outline.n_contours)
			return true;
		if (spread < kMtsdfMinimumSpread || spread > kMtsdfMaximumSpread)
			return false;

		msdfgen::Shape shape;
		if (msdfgen::readFreetypeOutline(shape, &outline, 1.0 / 64.0)
			|| shape.contours.empty())
		{
			return false;
		}
		shape.normalize();
		if (!shape.validate())
			return false;

		// Ink-trap coloring is deterministic and avoids channel conflicts at
		// dense CJK junctions without the quadratic cost of distance coloring.
		msdfgen::edgeColoringInkTrap(shape, kMtsdfCornerAngleThreshold, 0);
		const msdfgen::Shape::Bounds bounds = shape.getBounds();
		const double guard = static_cast<double>(spread) + 1.0;
		const double leftValue = std::floor(bounds.l) - guard;
		const double rightValue = std::ceil(bounds.r) + guard;
		const double bottomValue = std::floor(bounds.b) - guard;
		const double topValue = std::ceil(bounds.t) + guard;
		if (!std::isfinite(leftValue) || !std::isfinite(rightValue)
			|| !std::isfinite(bottomValue) || !std::isfinite(topValue)
			|| rightValue <= leftValue || topValue <= bottomValue
			|| leftValue < std::numeric_limits<int>::min()
			|| rightValue > std::numeric_limits<int>::max()
			|| bottomValue < std::numeric_limits<int>::min()
			|| topValue > std::numeric_limits<int>::max())
		{
			return false;
		}

		const int left = static_cast<int>(leftValue);
		const int right = static_cast<int>(rightValue);
		const int bottom = static_cast<int>(bottomValue);
		const int top = static_cast<int>(topValue);
		const int width = right - left;
		const int height = top - bottom;
		if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
			return false;
		const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
		if (pixelCount > maximumBytes / 4u)
			return false;

		msdfgen::Bitmap<float, 4> mtsdf(width, height);
		const msdfgen::Projection projection(msdfgen::Vector2(1.0),
			msdfgen::Vector2(-static_cast<double>(left),
				-static_cast<double>(bottom)));
		const msdfgen::SDFTransformation transformation(
			projection, msdfgen::Range(2.0 * static_cast<double>(spread)));
		const msdfgen::FillRule fillRule = outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;

		// Correct signs against the outline's FreeType fill rule before the fast edge-priority
		// interpolation pass. Distance checks are intentionally disabled in the
		// second pass because they are incompatible with the preceding sign fix.
		msdfgen::MSDFGeneratorConfig generationConfig;
		generationConfig.overlapSupport = true;
		generationConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::DISABLED;
		msdfgen::generateMTSDF(mtsdf, shape, transformation, generationConfig);
		msdfgen::distanceSignCorrection(mtsdf, shape, projection,
			0.5f, fillRule);
		msdfgen::MSDFGeneratorConfig correctionConfig;
		correctionConfig.overlapSupport = true;
		correctionConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY;
		correctionConfig.errorCorrection.distanceCheckMode =
			msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
		msdfgen::msdfErrorCorrection(mtsdf, shape, transformation,
			correctionConfig);
		msdfgen::simulate8bit(mtsdf);

		result.width = width;
		result.height = height;
		result.left = left;
		result.top = top;
		result.bgra.resize(pixelCount * 4u);
		for (int y = 0; y < height; ++y)
		{
			const int sourceY = height - 1 - y;
			for (int x = 0; x < width; ++x)
			{
				const float* source = mtsdf(x, sourceY);
				if (!IsMtsdfPixelEncodable(source))
					return false;
				std::uint8_t* destination = result.bgra.data()
					+ (static_cast<std::size_t>(y) * width + x) * 4u;
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
