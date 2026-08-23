#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "font_vector_msdfgen.h"
#include "globals.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fonthook::vectorfont
{
		void TouchBitmapCacheEntry(FreeTypeState& state, BitmapCacheEntry& entry,
			const BitmapCacheKey& key)
		{
			state.bitmapLru.splice(state.bitmapLru.begin(), state.bitmapLru, entry.lru);
			entry.lru = state.bitmapLru.begin();
		}

		void TrimBitmapCache(FreeTypeState& state)
		{
			const size_t limit = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::GlyphBitmap, GetBitmapCacheLimit());
			while ((GetCpuMemoryUsage(CpuMemoryCategory::GlyphBitmap) > limit
					|| IsCpuMemoryBudgetExceeded())
				&& !state.bitmapLru.empty())
			{
				const BitmapCacheKey key = state.bitmapLru.back();
				auto it = state.bitmapCache.find(key);
				if (it != state.bitmapCache.end())
				{
					state.bitmapCacheBytes -= it->second.bytes;
					state.bitmapCache.erase(it);
				}
				state.bitmapLru.pop_back();
			}
		}

		void RefreshGlyphBitmapCpuMemory(GlyphBitmap& bitmap)
		{
			bitmap.cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
				sizeof(GlyphBitmap) + bitmap.pixels.capacity());
		}

		constexpr size_t GetBitmapCacheEntryCpuBytes()
		{
			return sizeof(BitmapCacheEntry) + 2u * sizeof(BitmapCacheKey)
				+ 4u * sizeof(void*);
		}

	void SetBitmapCacheReducedAfterPrewarm(bool reduced)
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		if (state.bitmapCacheReducedAfterPrewarm == reduced)
			return;

		const size_t previousLimit = GetBitmapCacheLimit();
		const size_t previousBytes = state.bitmapCacheBytes;
		const size_t previousEntries = state.bitmapCache.size();
		state.bitmapCacheReducedAfterPrewarm = reduced;
		TrimBitmapCache(state);
		ReportCpuMemoryBudget(reduced ? "post-prewarm" : "prewarm", true);
		gLog.FormattedMessage(
			"tnvse_freetype_font: bitmap cache phase=%s limitMiB=%.2f previousLimitMiB=%.2f bytesMiB=%.2f previousBytesMiB=%.2f entries=%u evicted=%u",
			reduced ? "post-prewarm" : "prewarm",
			GetBitmapCacheLimit() / (1024.0 * 1024.0),
			previousLimit / (1024.0 * 1024.0),
			state.bitmapCacheBytes / (1024.0 * 1024.0),
			previousBytes / (1024.0 * 1024.0),
			static_cast<UInt32>(state.bitmapCache.size()),
			static_cast<UInt32>(previousEntries - state.bitmapCache.size()));
	}

		bool CopyGrayBitmap(const FT_Bitmap& source, GlyphBitmap& target)
		{
			constexpr int kBitmapGuardPixels = 1;
			const int sourceWidth = static_cast<int>(source.width);
			const int sourceHeight = static_cast<int>(source.rows);
			if (sourceWidth <= 0 || sourceHeight <= 0)
			{
				target.width = 0;
				target.height = 0;
				return true;
			}
			if (!source.buffer)
				return false;
			// FreeType returns the tight bitmap bounds. Keep a transparent texel around
			// every mask so point/linear sampling and fractional UI transforms cannot
			// clip the first or last coverage row or read a neighbouring atlas region.
			target.width = sourceWidth + kBitmapGuardPixels * 2;
			target.height = sourceHeight + kBitmapGuardPixels * 2;
			target.left -= kBitmapGuardPixels;
			target.top += kBitmapGuardPixels;
			target.pixels.assign(static_cast<size_t>(target.width) * target.height, 0);
			const int pitch = source.pitch;
			for (int y = 0; y < sourceHeight; ++y)
			{
				const int sourceY = pitch >= 0 ? y : sourceHeight - 1 - y;
				const UInt8* row = source.buffer + static_cast<ptrdiff_t>(sourceY) * std::abs(pitch);
				UInt8* output = target.pixels.data()
					+ static_cast<size_t>(y + kBitmapGuardPixels) * target.width
					+ kBitmapGuardPixels;
				if (source.pixel_mode == FT_PIXEL_MODE_GRAY)
				{
					if (source.num_grays == 256)
						std::copy(row, row + sourceWidth, output);
					else
					{
						const UInt32 denominator = std::max<UInt32>(1, source.num_grays - 1);
						for (int x = 0; x < sourceWidth; ++x)
							output[x] = static_cast<UInt8>(std::min<UInt32>(255,
								row[x] * 255u / denominator));
					}
				}
				else if (source.pixel_mode == FT_PIXEL_MODE_MONO)
				{
					for (int x = 0; x < sourceWidth; ++x)
						output[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
				}
				else
				{
					return false;
				}
			}
			return true;
		}

		bool BuildMsdfgenDistanceField(
			FT_GlyphSlot slot,
			UInt8 spread,
			DistanceFieldMethod method,
			GlyphBitmap& target)
		{
			if (!slot || slot->format != FT_GLYPH_FORMAT_OUTLINE
				|| spread < kDistanceFieldMinimumSpread
				|| spread > kDistanceFieldMaximumSpread)
				return false;

			target.distanceFieldMethod = method;
			if (method == DistanceFieldMethod::TrueSdf)
			{
				MsdfgenSdfBitmap generated;
				if (!GenerateMsdfgenTrueSdf(slot->outline, spread, generated))
					return false;
				target.width = generated.width;
				target.height = generated.height;
				target.left = generated.left;
				target.top = generated.top;
				target.pixels = std::move(generated.pixels);
			}
			else
			{
				MsdfgenMtsdfBitmap generated;
				if (!GenerateMsdfgenMtsdf(slot->outline, spread, generated))
					return false;
				target.width = generated.width;
				target.height = generated.height;
				target.left = generated.left;
				target.top = generated.top;
				target.pixels = std::move(generated.bgra);
			}
			return true;
		}

		float SmoothStep(float edge0, float edge1, float value)
		{
			if (!(edge1 > edge0))
				return value >= edge1 ? 1.0f : 0.0f;
			const float normalized = std::clamp(
				(value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return normalized * normalized * (3.0f - 2.0f * normalized);
		}

		float EvaluateCpuGlowCoverage(const FontConfig& config,
			float rasterScale, float signedDistance, float bodyCoverage)
		{
			if (!config.glow.enabled)
				return 0.0f;
			const float inner = std::max(config.glow.inner * rasterScale, 0.0f);
			const float outer = std::max(config.glow.outer * rasterScale,
				inner + 0.0001f);
			const float outsideDistance = std::max(-signedDistance, 0.0f);
			const float normalizedDistance =
				(outsideDistance - inner) / (outer - inner);
			const float fade = outsideDistance <= inner
				? 1.0f
				: std::exp2(-2.0f * std::max(config.glow.power, 0.0001f)
					* std::clamp(normalizedDistance, 0.0f, 1.0f));
			const float outerFeather = 1.0f - SmoothStep(
				outer - 0.5f, outer + 0.5f, outsideDistance);
			return std::clamp((1.0f - bodyCoverage) * fade * outerFeather,
				0.0f, 1.0f);
		}

		float EvaluateCpuOutlineCoverage(const FontConfig& config,
			float rasterScale, float signedDistance, float bodyCoverage)
		{
			if (!config.outline.enabled)
				return 0.0f;
			const float width = std::max(
				config.outline.width * rasterScale, 0.0f);
			const float softness = std::max(
				config.outline.softness * rasterScale, 0.0f);
			const float proxyAntialiasWidth = 0.5f + softness;
			const float proxy = SmoothStep(-width - proxyAntialiasWidth,
				proxyAntialiasWidth, signedDistance);
			return std::clamp(std::max(bodyCoverage, proxy), 0.0f, 1.0f);
		}

		float EvaluateCpuEffectCoverage(const FontConfig& config,
			GlyphMaskType maskType, float rasterScale, float signedDistance,
			float bodyCoverage)
		{
			switch (maskType)
			{
			case GlyphMaskType::Glow:
				return EvaluateCpuGlowCoverage(config, rasterScale,
					signedDistance, bodyCoverage);
			case GlyphMaskType::Outline:
				return EvaluateCpuOutlineCoverage(config, rasterScale,
					signedDistance, bodyCoverage);
			case GlyphMaskType::Shadow:
			{
				if (!config.shadow.enabled)
					return 0.0f;
				const float blur = std::max(
					config.shadow.blur * rasterScale, 0.0f);
				if (blur > 0.001f)
				{
					const float blurred = SmoothStep(-blur - 0.5f,
						blur + 0.5f, signedDistance);
					return std::pow(std::clamp(blurred, 0.0f, 1.0f),
						std::max(config.shadow.power, 0.0001f));
				}

				float glow = 0.0f;
				if (HardShadowIncludesGlow(config))
				{
					glow = EvaluateCpuGlowCoverage(config, rasterScale,
						signedDistance, bodyCoverage)
						* std::clamp(config.glow.color.a, 0.0f, 1.0f);
				}
				float outline = 0.0f;
				if (HardShadowIncludesOutline(config))
				{
					outline = EvaluateCpuOutlineCoverage(config, rasterScale,
						signedDistance, bodyCoverage)
						* std::clamp(config.outline.color.a, 0.0f, 1.0f);
				}
				const float outside = outline + (1.0f - outline) * glow;
				return std::clamp(bodyCoverage
					+ (1.0f - bodyCoverage) * outside, 0.0f, 1.0f);
			}
			default:
				return bodyCoverage;
			}
		}

		float ResolveCpuEffectRadius(const FontConfig& config,
			GlyphMaskType maskType, float rasterScale)
		{
			float radius = 0.0f;
			if (maskType == GlyphMaskType::Glow)
				radius = config.glow.outer;
			else if (maskType == GlyphMaskType::Outline)
				radius = config.outline.width + config.outline.softness;
			else if (maskType == GlyphMaskType::Shadow)
			{
				radius = config.shadow.blur;
				if (HardShadowIncludesGlow(config))
					radius = std::max(radius, config.glow.outer);
				if (HardShadowIncludesOutline(config))
				{
					radius = std::max(radius,
						config.outline.width + config.outline.softness);
				}
			}
			return std::max(radius * rasterScale, 0.0f);
		}

		void RunChamferDistanceTransform(std::vector<float>& distance,
			int width, int height)
		{
			constexpr float kDiagonal = 1.4142135623730950488f;
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					float& value = distance[
						static_cast<size_t>(y) * width + x];
					if (x > 0)
						value = std::min(value, distance[
							static_cast<size_t>(y) * width + x - 1] + 1.0f);
					if (y > 0)
					{
						value = std::min(value, distance[
							static_cast<size_t>(y - 1) * width + x] + 1.0f);
						if (x > 0)
							value = std::min(value, distance[
								static_cast<size_t>(y - 1) * width + x - 1]
								+ kDiagonal);
						if (x + 1 < width)
							value = std::min(value, distance[
								static_cast<size_t>(y - 1) * width + x + 1]
								+ kDiagonal);
					}
				}
			}
			for (int y = height - 1; y >= 0; --y)
			{
				for (int x = width - 1; x >= 0; --x)
				{
					float& value = distance[
						static_cast<size_t>(y) * width + x];
					if (x + 1 < width)
						value = std::min(value, distance[
							static_cast<size_t>(y) * width + x + 1] + 1.0f);
					if (y + 1 < height)
					{
						value = std::min(value, distance[
							static_cast<size_t>(y + 1) * width + x] + 1.0f);
						if (x > 0)
							value = std::min(value, distance[
								static_cast<size_t>(y + 1) * width + x - 1]
								+ kDiagonal);
						if (x + 1 < width)
							value = std::min(value, distance[
								static_cast<size_t>(y + 1) * width + x + 1]
								+ kDiagonal);
					}
				}
			}
		}

		bool BuildCpuEffectBitmap(const FontConfig& config,
			GlyphMaskType maskType, float rasterScale,
			const GlyphBitmap& body, GlyphBitmap& target)
		{
			if (body.width <= 0 || body.height <= 0 || body.pixels.empty())
			{
				target.width = 0;
				target.height = 0;
				target.pixels.clear();
				return true;
			}
			const float radius = ResolveCpuEffectRadius(
				config, maskType, rasterScale);
			if (!std::isfinite(radius)
				|| radius > static_cast<float>(std::numeric_limits<int>::max() / 4))
			{
				return false;
			}
			const int padding = std::max(2,
				static_cast<int>(std::ceil(radius + 1.5f)));
			if (body.width > 4096 - padding * 2
				|| body.height > 4096 - padding * 2)
			{
				return false;
			}
			target.width = body.width + padding * 2;
			target.height = body.height + padding * 2;
			target.left = body.left - padding;
			target.top = body.top + padding;
			const size_t pixelCount =
				static_cast<size_t>(target.width) * target.height;
			if (!pixelCount
				|| pixelCount > kMaximumPersistentSingleChannelBitmapBytes)
			{
				return false;
			}
			target.pixels.assign(pixelCount, 0);
			std::vector<float> distance(pixelCount);
			constexpr float kInfinity = 1.0e20f;
			auto bodyAt = [&](int x, int y) -> UInt8
			{
				const int sourceX = x - padding;
				const int sourceY = y - padding;
				if (sourceX < 0 || sourceY < 0
					|| sourceX >= body.width || sourceY >= body.height)
				{
					return 0;
				}
				return body.pixels[
					static_cast<size_t>(sourceY) * body.width + sourceX];
			};

			bool hasInside = false;
			for (int y = 0; y < target.height; ++y)
			{
				for (int x = 0; x < target.width; ++x)
				{
					const bool inside = bodyAt(x, y) >= 128;
					hasInside = hasInside || inside;
					distance[static_cast<size_t>(y) * target.width + x] =
						inside ? 0.0f : kInfinity;
				}
			}
			if (!hasInside)
			{
				// Preserve very small source-coverage glyphs without manufacturing an
				// effect around an empty 0.5 coverage contour.
				if (maskType == GlyphMaskType::Outline
					|| maskType == GlyphMaskType::Shadow)
				{
					for (int y = 0; y < target.height; ++y)
					{
						for (int x = 0; x < target.width; ++x)
						{
							target.pixels[
								static_cast<size_t>(y) * target.width + x] =
								bodyAt(x, y);
						}
					}
				}
				return true;
			}

			RunChamferDistanceTransform(distance, target.width, target.height);
			for (int y = 0; y < target.height; ++y)
			{
				for (int x = 0; x < target.width; ++x)
				{
					const size_t index =
						static_cast<size_t>(y) * target.width + x;
					const UInt8 bodyAlpha = bodyAt(x, y);
					if (bodyAlpha >= 128)
						continue;
					const float bodyCoverage = bodyAlpha / 255.0f;
					const float signedDistance = bodyAlpha > 0
						? bodyCoverage - 0.5f
						: 0.5f - distance[index];
					const float coverage = EvaluateCpuEffectCoverage(
						config, maskType, rasterScale,
						signedDistance, bodyCoverage);
					target.pixels[index] = static_cast<UInt8>(std::lround(
						std::clamp(coverage, 0.0f, 1.0f) * 255.0f));
				}
			}

			for (int y = 0; y < target.height; ++y)
			{
				for (int x = 0; x < target.width; ++x)
				{
					const bool outside = bodyAt(x, y) < 128;
					distance[static_cast<size_t>(y) * target.width + x] =
						outside ? 0.0f : kInfinity;
				}
			}
			RunChamferDistanceTransform(distance, target.width, target.height);
			for (int y = 0; y < target.height; ++y)
			{
				for (int x = 0; x < target.width; ++x)
				{
					const size_t index =
						static_cast<size_t>(y) * target.width + x;
					const UInt8 bodyAlpha = bodyAt(x, y);
					if (bodyAlpha < 128)
						continue;
					const float bodyCoverage = bodyAlpha / 255.0f;
					const float signedDistance = bodyAlpha < 255
						? bodyCoverage - 0.5f
						: distance[index] - 0.5f;
					const float coverage = EvaluateCpuEffectCoverage(
						config, maskType, rasterScale,
						signedDistance, bodyCoverage);
					target.pixels[index] = static_cast<UInt8>(std::lround(
						std::clamp(coverage, 0.0f, 1.0f) * 255.0f));
				}
			}
			return true;
		}

		NiColorA ResolveCompositeFillColor(const FontConfig& config)
		{
			return config.fontColor.configured
				? config.fontColor.color
				: NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
		}

		NiColorA ResolveCompositeEffectColor(const EffectStyle& effect,
			const FontConfig& config)
		{
			NiColorA color = effect.colorMode == EffectColorMode::Fill
				? ResolveCompositeFillColor(config) : effect.color;
			// Effect alpha is independent from the configured fill alpha.
			color.a = effect.color.a;
			return color;
		}

		UInt8 CompositeColorByte(float value)
		{
			return static_cast<UInt8>(std::lround(
				std::clamp(std::isfinite(value) ? value : 1.0f,
					0.0f, 1.0f) * 255.0f));
		}

		bool TightenCompositeAlphaBounds(GlyphBitmap& target)
		{
			if (target.width <= 0 || target.height <= 0)
				return true;
			const size_t expectedBytes = static_cast<size_t>(target.width)
				* target.height * 4u;
			if (target.pixels.size() != expectedBytes)
				return false;

			int minX = target.width;
			int minY = target.height;
			int maxX = -1;
			int maxY = -1;
			for (int y = 0; y < target.height; ++y)
			{
				const UInt8* row = target.pixels.data()
					+ static_cast<size_t>(y) * target.width * 4u;
				for (int x = 0; x < target.width; ++x)
				{
					if (!row[static_cast<size_t>(x) * 4u + 3u])
						continue;
					minX = std::min(minX, x);
					minY = std::min(minY, y);
					maxX = std::max(maxX, x);
					maxY = std::max(maxY, y);
				}
			}

			// A completely transparent configured glyph still needs to remain a
			// structurally valid non-space record. Do not turn it into the
			// reserved known-empty representation here.
			if (maxX < minX || maxY < minY)
				return true;
			if (!minX && !minY
				&& maxX == target.width - 1
				&& maxY == target.height - 1)
			{
				return true;
			}

			const int sourceWidth = target.width;
			const int croppedWidth = maxX - minX + 1;
			const int croppedHeight = maxY - minY + 1;
			std::vector<UInt8> cropped(
				static_cast<size_t>(croppedWidth) * croppedHeight * 4u);
			for (int y = 0; y < croppedHeight; ++y)
			{
				const UInt8* source = target.pixels.data()
					+ (static_cast<size_t>(minY + y) * sourceWidth + minX) * 4u;
				UInt8* destination = cropped.data()
					+ static_cast<size_t>(y) * croppedWidth * 4u;
				std::copy_n(source, static_cast<size_t>(croppedWidth) * 4u,
					destination);
			}

			// Bitmap rows run downwards while top is measured upwards from the
			// baseline. Preserve the logical advance; direct FontLetter
			// conversion derives the compensating spacing from these corrected
			// bounds, so pen position and whitespace remain unchanged.
			target.left += minX;
			target.top -= minY;
			target.width = croppedWidth;
			target.height = croppedHeight;
			target.pixels.swap(cropped);
			return true;
		}

		bool BuildCpuCompositeBitmap(const FontConfig& config,
			float rasterScale, const GlyphBitmap& body, GlyphBitmap& target)
		{
			if (body.width <= 0 || body.height <= 0 || body.pixels.empty())
			{
				target.width = 0;
				target.height = 0;
				target.pixels.clear();
				return true;
			}

			struct CompositeLayer
			{
				GlyphBitmap bitmap;
				NiColorA color;
				int offsetX = 0;
				int offsetY = 0;
				bool enabled = false;
			};
			std::array<CompositeLayer, 4> layers;
			auto prepareEffect = [&](size_t index, GlyphMaskType mask,
				const EffectStyle& effect)
			{
				CompositeLayer& layer = layers[index];
				layer.enabled = effect.enabled;
				if (!layer.enabled)
					return true;
				layer.bitmap.maskType = mask;
				layer.color = ResolveCompositeEffectColor(effect, config);
				return BuildCpuEffectBitmap(config, mask, rasterScale,
					body, layer.bitmap);
			};
			if (!prepareEffect(0, GlyphMaskType::Shadow, config.shadow)
				|| !prepareEffect(1, GlyphMaskType::Glow, config.glow)
				|| !prepareEffect(2, GlyphMaskType::Outline, config.outline))
			{
				return false;
			}
			layers[0].offsetX = static_cast<int>(std::lround(
				config.shadow.x * rasterScale));
			layers[0].offsetY = static_cast<int>(std::lround(
				config.shadow.y * rasterScale));
			layers[3].bitmap.width = body.width;
			layers[3].bitmap.height = body.height;
			layers[3].bitmap.left = body.left;
			layers[3].bitmap.top = body.top;
			layers[3].bitmap.pixels = body.pixels;
			layers[3].color = ResolveCompositeFillColor(config);
			layers[3].enabled = true;

			int left = std::numeric_limits<int>::max();
			int right = std::numeric_limits<int>::lowest();
			int top = std::numeric_limits<int>::lowest();
			int bottom = std::numeric_limits<int>::max();
			for (const CompositeLayer& layer : layers)
			{
				if (!layer.enabled || layer.bitmap.width <= 0
					|| layer.bitmap.height <= 0)
				{
					continue;
				}
				const int layerLeft = layer.bitmap.left + layer.offsetX;
				const int layerTop = layer.bitmap.top - layer.offsetY;
				left = std::min(left, layerLeft);
				right = std::max(right, layerLeft + layer.bitmap.width);
				top = std::max(top, layerTop);
				bottom = std::min(bottom, layerTop - layer.bitmap.height);
			}
			if (left >= right || bottom >= top
				|| right - left > 4096 || top - bottom > 4096)
			{
				return false;
			}
			target.left = left;
			target.top = top;
			target.width = right - left;
			target.height = top - bottom;
			const size_t pixelCount = static_cast<size_t>(target.width)
				* target.height;
			if (!pixelCount
				|| pixelCount > kMaximumPersistentFourChannelBitmapBytes / 4u)
			{
				return false;
			}
			target.pixels.assign(pixelCount * 4u, 0);

			// Store straight-alpha BGRA, but perform layer composition in
			// premultiplied form to preserve the configured layer order.
			for (const CompositeLayer& layer : layers)
			{
				if (!layer.enabled || layer.bitmap.width <= 0
					|| layer.bitmap.height <= 0)
				{
					continue;
				}
				const float colorR = std::clamp(
					std::isfinite(layer.color.r) ? layer.color.r : 1.0f,
					0.0f, 1.0f);
				const float colorG = std::clamp(
					std::isfinite(layer.color.g) ? layer.color.g : 1.0f,
					0.0f, 1.0f);
				const float colorB = std::clamp(
					std::isfinite(layer.color.b) ? layer.color.b : 1.0f,
					0.0f, 1.0f);
				const float colorA = std::clamp(
					std::isfinite(layer.color.a) ? layer.color.a : 1.0f,
					0.0f, 1.0f);
				const int layerLeft = layer.bitmap.left + layer.offsetX;
				const int layerTop = layer.bitmap.top - layer.offsetY;
				for (int y = 0; y < layer.bitmap.height; ++y)
				{
					const int targetY = top - layerTop + y;
					for (int x = 0; x < layer.bitmap.width; ++x)
					{
						const UInt8 coverage = layer.bitmap.pixels[
							static_cast<size_t>(y) * layer.bitmap.width + x];
						if (!coverage)
							continue;
						const int targetX = layerLeft - left + x;
						const size_t targetIndex = (static_cast<size_t>(targetY)
							* target.width + targetX) * 4u;
						UInt8* destination = target.pixels.data() + targetIndex;
						const float sourceAlpha =
							(coverage / 255.0f) * colorA;
						const float destinationAlpha = destination[3] / 255.0f;
						const float outputAlpha = sourceAlpha
							+ destinationAlpha * (1.0f - sourceAlpha);
						const float destinationR = destination[2] / 255.0f;
						const float destinationG = destination[1] / 255.0f;
						const float destinationB = destination[0] / 255.0f;
						const float inverse = 1.0f - sourceAlpha;
						const float outputR = outputAlpha > 0.0f
							? (colorR * sourceAlpha
								+ destinationR * destinationAlpha * inverse)
								/ outputAlpha : 0.0f;
						const float outputG = outputAlpha > 0.0f
							? (colorG * sourceAlpha
								+ destinationG * destinationAlpha * inverse)
								/ outputAlpha : 0.0f;
						const float outputB = outputAlpha > 0.0f
							? (colorB * sourceAlpha
								+ destinationB * destinationAlpha * inverse)
								/ outputAlpha : 0.0f;
						destination[0] = CompositeColorByte(outputB);
						destination[1] = CompositeColorByte(outputG);
						destination[2] = CompositeColorByte(outputR);
						destination[3] = CompositeColorByte(outputAlpha);
					}
				}
			}
			return TightenCompositeAlphaBounds(target);
		}

		std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(FreeTypeState&,
			RuntimeFont& runtime,
			const ResolvedGlyph& resolved,
			GlyphMaskType maskType,
			float rasterScale, const BitmapCacheKey& key)
		{
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			bitmap->maskType = maskType;
			bitmap->distanceFieldMethod = static_cast<DistanceFieldMethod>(
				key.distanceFieldMethod);
			bitmap->sdfSpread = key.sdfSpread;
			bitmap->strokeWidth26Dot6 = key.strokeWidth26Dot6;
			RuntimeRole& role = *resolved.role;
			if (!ConfigureRuntimeFace(*resolved.runtimeFace, *role.style, rasterScale, true))
				return nullptr;

			// Use one contour source for every rendering route. Grid fitting changes
			// stem widths before Fill is rasterized; glow, outline, shadow and the
			// aggressive composite then inherit that heavier body. The SDF route
			// already consumes the unhinted scalable outline, so CPU coverage must
			// do the same to keep its body/effect footprint visually comparable.
			// The configured transform, embolden and slant still apply below.
			constexpr FT_Int32 loadFlags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP
				| FT_LOAD_NO_SVG | FT_LOAD_NO_HINTING;
			if (FT_Load_Glyph(
				resolved.runtimeFace->ftFace, resolved.glyphIndex, loadFlags))
				return nullptr;
			FT_GlyphSlot slot = resolved.runtimeFace->ftFace->glyph;
			if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
			{
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}
			if (role.style->embolden > 0.0f && slot->outline.n_points)
			{
				const FT_Pos strength = key.embolden26Dot6;
				FT_Outline_EmboldenXY(&slot->outline, strength, strength);
			}

			if (maskType == GlyphMaskType::Fill)
			{
				if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
					return nullptr;
				bitmap->left = slot->bitmap_left;
				bitmap->top = slot->bitmap_top;
				if (!CopyGrayBitmap(slot->bitmap, *bitmap))
					return nullptr;
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			if (maskType == GlyphMaskType::DistanceField)
			{
				if (key.sdfSpread < kDistanceFieldMinimumSpread
					|| key.sdfSpread > kDistanceFieldMaximumSpread)
					return nullptr;
				if (!BuildMsdfgenDistanceField(slot, key.sdfSpread,
					bitmap->distanceFieldMethod, *bitmap))
					return nullptr;
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			if (maskType == GlyphMaskType::Composite)
			{
				if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
					return nullptr;
				GlyphBitmap body;
				body.left = slot->bitmap_left;
				body.top = slot->bitmap_top;
				if (!CopyGrayBitmap(slot->bitmap, body)
					|| !BuildCpuCompositeBitmap(*runtime.config,
						rasterScale, body, *bitmap))
				{
					return nullptr;
				}
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			const bool supportedCpuEffect =
				maskType == GlyphMaskType::Glow
				|| maskType == GlyphMaskType::Outline
				|| maskType == GlyphMaskType::Shadow;
			const bool enabledCpuEffect =
				maskType == GlyphMaskType::Glow
					? runtime.config->glow.enabled
					: maskType == GlyphMaskType::Outline
						? runtime.config->outline.enabled
						: maskType == GlyphMaskType::Shadow
							? runtime.config->shadow.enabled : false;
			if (!supportedCpuEffect || !enabledCpuEffect || !slot->outline.n_points)
			{
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
				return nullptr;
			GlyphBitmap body;
			body.left = slot->bitmap_left;
			body.top = slot->bitmap_top;
			if (!CopyGrayBitmap(slot->bitmap, body)
				|| !BuildCpuEffectBitmap(*runtime.config, maskType,
					rasterScale, body, *bitmap))
			{
				return nullptr;
			}
			RefreshGlyphBitmapCpuMemory(*bitmap);
			return bitmap;
		}

	static float SanitizeBitmapRasterScale(float rasterScale)
	{
		return std::isfinite(rasterScale) && rasterScale >= 0.1f
			&& rasterScale <= 10.0f ? rasterScale : 1.0f;
	}

	static bool ResolveBitmapCacheKey(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMaskType maskType, float safeScale,
		UInt32 sdfSpread, RuntimeFont*& rasterRuntime,
		ResolvedGlyph& resolved, BitmapCacheKey& key)
	{
		rasterRuntime = &runtime;
		UInt32 resolvedSpread = sdfSpread;
		if (maskType == GlyphMaskType::DistanceField)
		{
			DistanceFieldRasterProfile profile;
			if (!ResolveDistanceFieldRasterProfile(*runtime.config,
				glyph.byteClass, safeScale, true, profile))
			{
				return false;
			}
			rasterRuntime = GetDistanceFieldRasterOwnerRuntime(runtime, glyph.byteClass);
			if (!rasterRuntime)
				return false;
			resolvedSpread = profile.sdfSpread;
		}
		if (!ResolveVectorGlyph(*rasterRuntime, glyph, resolved) || !resolved.role
			|| !resolved.role->style || !resolved.runtimeFace
			|| !resolved.runtimeFace->ftFace || !resolved.runtimeFace->file)
		{
			return false;
		}
		const ByteStyle& style = *resolved.role->style;
		const int effectiveWidth = std::clamp(static_cast<int>(std::lround(
			style.pixelSize * style.scaleX * safeScale)), 1, 65535);
		const int effectiveHeight = std::clamp(static_cast<int>(std::lround(
			style.pixelSize * style.scaleY * safeScale)), 1, 65535);
		const SInt32 strokeWidth = ResolveCpuEffectMaskIdentity(
			*rasterRuntime->config, maskType, safeScale);
		const SInt32 embolden = static_cast<SInt32>(std::lround(
			style.embolden * safeScale * 64.0f));
		const UInt8 resolvedSdfSpread = maskType == GlyphMaskType::DistanceField
			&& resolvedSpread >= kDistanceFieldMinimumSpread
			&& resolvedSpread <= kDistanceFieldMaximumSpread
			? static_cast<UInt8>(resolvedSpread) : 0;
		if (maskType == GlyphMaskType::DistanceField && !resolvedSdfSpread)
			return false;
		const UInt8 distanceFieldMethod =
			maskType == GlyphMaskType::DistanceField
			? static_cast<UInt8>(GetConfiguredDistanceFieldMethod()) : 0;
		const float slant = std::tan(style.slantDegrees
			* 3.14159265358979323846f / 180.0f);
		key = {
			resolved.runtimeFace->file->contentHash,
			static_cast<SInt32>(resolved.runtimeFace->ftFace->face_index),
			resolved.glyphIndex,
			GetFreeTypeTextCodePage(),
			static_cast<UInt16>(effectiveWidth),
			static_cast<UInt16>(effectiveHeight),
			embolden,
			strokeWidth,
			static_cast<SInt32>(std::lround(slant * kFixedScale)),
			resolvedSdfSpread,
			static_cast<UInt8>(maskType),
			distanceFieldMethod
		};
		key.stableHash = HashBitmapKey(key);
		return true;
	}

	void ResolveGlyphBitmapCacheIds(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<UInt64>& cacheIds)
	{
		cacheIds.assign(requests.size(), 0);
		if (requests.empty())
			return;
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		for (size_t index = 0; index < requests.size(); ++index)
		{
			const GlyphBitmapRequest& request = requests[index];
			if (!request.glyph)
				continue;
			ResolvedGlyph resolved;
			BitmapCacheKey key;
			RuntimeFont* rasterRuntime = nullptr;
			if (ResolveBitmapCacheKey(runtime, *request.glyph, request.maskType,
				safeScale, request.sdfSpread, rasterRuntime, resolved, key))
			{
				cacheIds[index] = HashBitmapKey(key);
			}
		}
	}

	static std::shared_ptr<const GlyphBitmap> FindCachedGlyphBitmapLocked(
		FreeTypeState& state, RuntimeFont& runtime, const ResolvedGlyph& resolved,
		const BitmapCacheKey& key, PersistentBitmapProfile*& persistentProfile)
	{
		persistentProfile = nullptr;
		auto existing = state.bitmapCache.find(key);
		if (existing != state.bitmapCache.end())
		{
			TouchBitmapCacheEntry(state, existing->second, key);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapMemoryHit);
			if (existing->second.sourceFontId != runtime.config->fontId)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapCrossFontHit);
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedCrossFontBitmapShare)
				{
					state.loggedCrossFontBitmapShare = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first cross-font bitmap cache hit sourceFont=%u targetFont=%u path=%ls face=%d glyph=%u size=%ux%u mask=%u",
						existing->second.sourceFontId, runtime.config->fontId,
						resolved.runtimeFace->file->path.c_str(),
						key.fontFaceIndex, key.glyphIndex,
						key.effectiveWidth, key.effectiveHeight,
						key.maskType);
				}
			}
			return existing->second.bitmap;
		}

		const PersistentBitmapProfileKey persistentKey =
			MakePersistentBitmapProfileKey(key,
				resolved.runtimeFace->file->contentHash);
		persistentProfile =
			GetPersistentBitmapProfile(persistentKey,
				resolved.runtimeFace->file->path, runtime.config->fontId,
				static_cast<UInt32>(std::max<FT_Long>(1,
					resolved.runtimeFace->ftFace->num_glyphs)));
		if (persistentProfile)
		{
			std::shared_ptr<GlyphBitmap> diskBitmap =
				LoadPersistentGlyphBitmap(*persistentProfile, key);
			if (diskBitmap)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskHit);
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskReadBytes,
					diskBitmap->pixels.size());
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedPersistentBitmapHit)
				{
					state.loggedPersistentBitmapHit = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first persistent bitmap cache hit path=%ls font=%u glyph=%u size=%ux%u mask=%u bytes=%u records=%u",
						persistentProfile->path.c_str(), runtime.config->fontId,
						key.glyphIndex, key.effectiveWidth, key.effectiveHeight,
						key.maskType, static_cast<UInt32>(diskBitmap->pixels.size()),
						persistentProfile->recordCount);
				}
				const size_t bytes =
					sizeof(GlyphBitmap) + diskBitmap->pixels.capacity();
				state.bitmapLru.push_front(key);
				const auto [inserted, success] = state.bitmapCache.emplace(key,
					BitmapCacheEntry{ diskBitmap, bytes,
						state.bitmapLru.begin(), runtime.config->fontId });
				if (!success)
				{
					state.bitmapLru.pop_front();
					return inserted->second.bitmap;
				}
				inserted->second.cpuMemory.Reset(
					CpuMemoryCategory::GlyphBitmap,
					GetBitmapCacheEntryCpuBytes());
				state.bitmapCacheBytes += bytes;
				TrimBitmapCache(state);
				return diskBitmap;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskMiss);
		}
		return nullptr;
	}

	static void InsertGlyphBitmapCacheLocked(FreeTypeState& state,
		RuntimeFont& runtime, const BitmapCacheKey& key,
		const std::shared_ptr<GlyphBitmap>& bitmap)
	{
		if (!bitmap)
			return;
		const size_t bytes = sizeof(GlyphBitmap) + bitmap->pixels.capacity();
		state.bitmapLru.push_front(key);
		const auto [inserted, success] = state.bitmapCache.emplace(key,
			BitmapCacheEntry{ bitmap, bytes, state.bitmapLru.begin(),
				runtime.config->fontId });
		if (!success)
		{
			state.bitmapLru.pop_front();
			return;
		}
		inserted->second.cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
			GetBitmapCacheEntryCpuBytes());
		state.bitmapCacheBytes += bytes;
	}

	void TrimFreeTypeCpuCachesForTotalBudget()
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		TrimBitmapCache(state);
	}

	static std::shared_ptr<const GlyphBitmap> GetGlyphBitmapLocked(
		FreeTypeState& state, RuntimeFont& runtime,
		const ResolvedGlyph& resolved, GlyphMaskType maskType, float safeScale,
		const BitmapCacheKey& key)
	{
		PersistentBitmapProfile* persistentProfile = nullptr;
		if (std::shared_ptr<const GlyphBitmap> cached = FindCachedGlyphBitmapLocked(
			state, runtime, resolved, key, persistentProfile))
		{
			return cached;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized);
		std::shared_ptr<GlyphBitmap> bitmap =
			BuildGlyphBitmap(state, runtime, resolved, maskType, safeScale, key);
		if (!bitmap)
			return nullptr;
		if (persistentProfile
			&& StorePersistentGlyphBitmap(*persistentProfile, key, *bitmap))
		{
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWrite);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWriteBytes,
				bitmap->pixels.size());
		}
		InsertGlyphBitmapCacheLocked(state, runtime, key, bitmap);
		TrimBitmapCache(state);
		return bitmap;
	}

	struct BitmapBatchDedupeSlot
	{
		BitmapCacheKey key;
		size_t resultIndex = 0;
		UInt32 generation = 0;
	};

	struct BitmapBatchDedupeScratch
	{
		std::vector<BitmapBatchDedupeSlot> slots;
		UInt32 generation = 0;

		void Prepare(size_t requestCount)
		{
			size_t required = 8;
			const size_t target = std::max<size_t>(8, requestCount * 2);
			while (required < target)
				required <<= 1;
			if (slots.size() < required)
				slots.resize(required);
			if (++generation == 0)
			{
				for (BitmapBatchDedupeSlot& slot : slots)
					slot.generation = 0;
				generation = 1;
			}
		}
	};

	void GetGlyphBitmaps(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& results)
	{
		results.assign(requests.size(), nullptr);
		if (requests.empty())
			return;

		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		thread_local BitmapBatchDedupeScratch cachedScratch;
		BitmapBatchDedupeScratch& scratch = cachedScratch;
		scratch.Prepare(requests.size());
		const size_t slotMask = scratch.slots.size() - 1;
		UInt64 duplicateCount = 0;
		for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
		{
			const GlyphBitmapRequest& request = requests[requestIndex];
			if (!request.glyph)
				continue;
			ResolvedGlyph resolved;
			BitmapCacheKey key;
			RuntimeFont* rasterRuntime = nullptr;
			if (!ResolveBitmapCacheKey(runtime, *request.glyph, request.maskType,
				safeScale, request.sdfSpread, rasterRuntime, resolved, key)
				|| !rasterRuntime)
			{
				continue;
			}

			size_t slotIndex = BitmapCacheKeyHash{}(key) & slotMask;
			for (;;)
			{
				BitmapBatchDedupeSlot& slot = scratch.slots[slotIndex];
				if (slot.generation != scratch.generation)
				{
					slot.generation = scratch.generation;
					slot.key = key;
					slot.resultIndex = requestIndex;
					results[requestIndex] = GetGlyphBitmapLocked(state, *rasterRuntime,
						resolved, request.maskType, safeScale, key);
					break;
				}
				if (slot.key == key)
				{
					++duplicateCount;
					results[requestIndex] = results[slot.resultIndex];
					break;
				}
				slotIndex = (slotIndex + 1) & slotMask;
			}
		}
		RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchRequest,
			static_cast<UInt64>(requests.size()));
		if (duplicateCount)
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchDedupe, duplicateCount);
	}

	struct PrewarmBitmapWorkItem
	{
		size_t requestIndex = 0;
		RuntimeFont* rasterRuntime = nullptr;
		ResolvedGlyph resolved;
		BitmapCacheKey key;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		PersistentBitmapProfile* persistentProfile = nullptr;
		std::shared_ptr<GlyphBitmap> bitmap;
	};

	struct PrewarmWorkerFace
	{
		std::shared_ptr<MappedFontFile> file;
		SInt32 faceIndex = 0;
		RuntimeFace runtimeFace;
	};

	struct PrewarmRasterWorkerContext
	{
		FreeTypeState state;
		std::vector<PrewarmWorkerFace> faces;

		PrewarmRasterWorkerContext() = default;
		PrewarmRasterWorkerContext(const PrewarmRasterWorkerContext&) = delete;
		PrewarmRasterWorkerContext& operator=(
			const PrewarmRasterWorkerContext&) = delete;

		~PrewarmRasterWorkerContext()
		{
			// RuntimeFace must release every FT_Face before its owning library.
			faces.clear();
			if (state.library)
				FT_Done_FreeType(state.library);
			state.library = nullptr;
		}

		bool Prepare()
		{
			if (!state.library && FT_Init_FreeType(&state.library))
				return false;
			if (faces.capacity() < 8u)
				faces.reserve(8u);
			return true;
		}
	};

	constexpr DWORD kPrewarmRasterBatchTimeoutMs = 120000;
	constexpr DWORD kPrewarmRasterPoolStopTimeoutMs = 2000;

	// Every dispatched batch is heap-owned. A worker that outlives a timeout keeps
	// only this immutable task snapshot and its worker-local FreeType state alive;
	// it never retains the coordinator's stack or the live RuntimeFont graph.
	class PrewarmRasterBatchTask
	{
	public:
		virtual ~PrewarmRasterBatchTask() = default;
		virtual void Run(UInt32 workerIndex) noexcept = 0;
		virtual void Cancel() noexcept = 0;
	};

	class PrewarmRasterWorkerPool
	{
	public:
		UInt32 Prepare(UInt32 requestedWorkers) noexcept
		{
			requestedWorkers = std::clamp<UInt32>(
				requestedWorkers, 1, kMaximumPrewarmRasterWorkers);
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stopping || m_poisoned || m_batchActive)
				return 0;
			while (m_workers.size() < requestedWorkers)
			{
				const UInt32 workerIndex =
					static_cast<UInt32>(m_workers.size());
				try
				{
					++m_liveWorkers;
					m_workers.emplace_back(
						[this, workerIndex]
						{
							WorkerMain(workerIndex);
						});
				}
				catch (...)
				{
					--m_liveWorkers;
					break;
				}
			}
			return static_cast<UInt32>(std::min<size_t>(
				requestedWorkers, m_workers.size()));
		}

		bool Execute(UInt32 workerCount,
			const std::shared_ptr<PrewarmRasterBatchTask>& task) noexcept
		{
			if (!task || !workerCount)
				return false;
			std::unique_lock<std::timed_mutex> dispatchLock(
				m_dispatchMutex, std::defer_lock);
			if (!dispatchLock.try_lock_for(
				std::chrono::milliseconds(kPrewarmRasterPoolStopTimeoutMs)))
			{
				task->Cancel();
				return false;
			}
			workerCount = std::clamp<UInt32>(
				workerCount, 1, kMaximumPrewarmRasterWorkers);
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_stopping || m_poisoned || m_batchActive
					|| m_workers.empty())
				{
					task->Cancel();
					return false;
				}
				m_activeWorkers = static_cast<UInt32>(std::min<size_t>(
					workerCount, m_workers.size()));
				m_completedWorkers = 0;
				m_task = task;
				m_batchActive = true;
				if (++m_generation == 0)
					++m_generation;
			}

			m_workAvailable.notify_all();
			bool completed = false;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				completed = m_batchCompleted.wait_for(lock,
					std::chrono::milliseconds(kPrewarmRasterBatchTimeoutMs), [this]
				{
					return m_stopping || m_completedWorkers >= m_activeWorkers;
				});
				completed = completed && !m_stopping
					&& m_completedWorkers >= m_activeWorkers;
				if (!completed)
				{
					task->Cancel();
					m_poisoned = true;
					m_stopping = true;
				}
				m_task = nullptr;
				m_activeWorkers = 0;
				m_completedWorkers = 0;
				m_batchActive = false;
			}
			if (!completed)
				m_workAvailable.notify_all();
			return completed;
		}

		void Stop() noexcept
		{
			std::unique_lock<std::timed_mutex> dispatchLock(
				m_dispatchMutex, std::defer_lock);
			if (!dispatchLock.try_lock_for(
				std::chrono::milliseconds(kPrewarmRasterPoolStopTimeoutMs)))
			{
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_poisoned = true;
					m_stopping = true;
					if (m_task)
						m_task->Cancel();
					m_workAvailable.notify_all();
					m_batchCompleted.notify_all();
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm raster pool stop arbitration timed out timeoutMs=%u policy=no-join-process-lifetime-quarantine",
					kPrewarmRasterPoolStopTimeoutMs);
				return;
			}
			std::vector<std::thread> workers;
			bool exited = false;
			UInt32 remainingWorkers = 0;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				if (m_workers.empty())
					return;
				m_stopping = true;
				if (m_task)
					m_task->Cancel();
				m_workAvailable.notify_all();
				m_batchCompleted.notify_all();
				exited = m_workersExited.wait_for(lock,
					std::chrono::milliseconds(kPrewarmRasterPoolStopTimeoutMs),
					[this]
					{
						return m_liveWorkers == 0;
					});
				if (exited)
				{
					std::array<HANDLE, kMaximumPrewarmRasterWorkers> handles{};
					const DWORD handleCount = static_cast<DWORD>(m_workers.size());
					for (DWORD index = 0; index < handleCount; ++index)
						handles[index] = m_workers[index].native_handle();
					exited = WaitForMultipleObjects(handleCount, handles.data(), TRUE,
						kPrewarmRasterPoolStopTimeoutMs) == WAIT_OBJECT_0;
				}
				if (exited)
					workers.swap(m_workers);
				else
				{
					m_poisoned = true;
					remainingWorkers = m_liveWorkers;
				}
			}
			if (!exited)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm raster pool exit acknowledgement timed out timeoutMs=%u liveWorkers=%u policy=no-join-process-lifetime-quarantine",
					kPrewarmRasterPoolStopTimeoutMs, remainingWorkers);
			}
			for (std::thread& worker : workers)
			{
				if (worker.joinable())
					worker.join();
			}
			if (exited)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_task = nullptr;
				m_activeWorkers = 0;
				m_completedWorkers = 0;
				m_batchActive = false;
				if (!m_poisoned)
					m_stopping = false;
			}
		}

	private:
		void WorkerMain(UInt32 workerIndex) noexcept
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			UInt64 observedGeneration = 0;
			for (;;)
			{
				std::shared_ptr<PrewarmRasterBatchTask> task;
				UInt64 generation = 0;
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_workAvailable.wait(lock,
						[this, workerIndex, &observedGeneration]
						{
							return m_stopping
								|| (m_batchActive
									&& m_generation != observedGeneration
									&& workerIndex < m_activeWorkers);
					});
					if (m_stopping)
						break;
					task = m_task;
					generation = m_generation;
					observedGeneration = generation;
				}

				if (task)
					task->Run(workerIndex);
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					if (m_batchActive && m_generation == generation)
					{
						++m_completedWorkers;
						if (m_completedWorkers >= m_activeWorkers)
						{
							m_batchCompleted.notify_one();
						}
					}
				}
			}
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_liveWorkers)
					--m_liveWorkers;
			}
			m_workersExited.notify_all();
		}

		std::timed_mutex m_dispatchMutex;
		std::mutex m_mutex;
		std::condition_variable m_workAvailable;
		std::condition_variable m_batchCompleted;
		std::condition_variable m_workersExited;
		std::vector<std::thread> m_workers;
		std::shared_ptr<PrewarmRasterBatchTask> m_task;
		UInt64 m_generation = 0;
		UInt32 m_activeWorkers = 0;
		UInt32 m_completedWorkers = 0;
		UInt32 m_liveWorkers = 0;
		bool m_batchActive = false;
		bool m_stopping = false;
		bool m_poisoned = false;
	};

	static PrewarmRasterWorkerPool* TryGetRasterWorkerPool() noexcept
	{
		// Explicit prewarm lifecycle owns every thread. The process-lifetime
		// allocation prevents a joinable std::thread destructor under loader lock.
		// Allocation failure safely selects the caller-only path for this process.
		static PrewarmRasterWorkerPool* pool = []() noexcept
		{
			try
			{
				return new PrewarmRasterWorkerPool;
			}
			catch (...)
			{
				return static_cast<PrewarmRasterWorkerPool*>(nullptr);
			}
		}();
		return pool;
	}

	void ReleasePrewarmRasterWorkers() noexcept
	{
		// A healthy pool exits and joins inside the bounded grace period. A poisoned
		// pool remains process-lifetime-owned so an unresponsive external raster call
		// can never turn shutdown into another permanent wait.
		if (PrewarmRasterWorkerPool* pool = TryGetRasterWorkerPool())
			pool->Stop();
	}

	static RuntimeFace* GetPrewarmWorkerFace(FT_Library library,
		std::vector<PrewarmWorkerFace>& faces,
		const std::shared_ptr<MappedFontFile>& file, SInt32 faceIndex)
	{
		for (PrewarmWorkerFace& face : faces)
		{
			if (face.file == file && face.faceIndex == faceIndex)
				return &face.runtimeFace;
		}
		if (!library || !file || !file->data || file->size <= 0)
			return nullptr;
		PrewarmWorkerFace workerFace;
		workerFace.file = file;
		workerFace.faceIndex = faceIndex;
		workerFace.runtimeFace.file = file;
		if (FT_New_Memory_Face(library, file->data, file->size, faceIndex,
			&workerFace.runtimeFace.ftFace))
		{
			return nullptr;
		}
		faces.push_back(std::move(workerFace));
		return &faces.back().runtimeFace;
	}

	struct PrewarmRasterRuntimeSnapshot
	{
		const RuntimeFont* runtimeIdentity = nullptr;
		const ByteStyle* styleIdentity = nullptr;
		FontConfig config;
		ByteStyle style;
		RuntimeFont runtime;
		RuntimeRole role;

		explicit PrewarmRasterRuntimeSnapshot(
			const PrewarmBitmapWorkItem& source)
			: runtimeIdentity(source.rasterRuntime),
			styleIdentity(source.resolved.role
				? source.resolved.role->style : nullptr)
		{
			if (!runtimeIdentity || !runtimeIdentity->config || !styleIdentity)
				throw std::runtime_error("invalid prewarm runtime snapshot");
			config = *runtimeIdentity->config;
			style = *styleIdentity;
			runtime.config = &config;
			role.owner = &runtime;
			role.style = &style;
		}
	};

	struct PrewarmRasterWorkSnapshot
	{
		PrewarmRasterRuntimeSnapshot* runtime = nullptr;
		ResolvedGlyph resolved;
		std::shared_ptr<MappedFontFile> file;
		SInt32 faceIndex = 0;
		BitmapCacheKey key;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		std::shared_ptr<GlyphBitmap> bitmap;

		PrewarmRasterWorkSnapshot(const PrewarmBitmapWorkItem& source,
			PrewarmRasterRuntimeSnapshot& runtimeSnapshot)
			: runtime(&runtimeSnapshot)
		{
			if (!source.rasterRuntime || !source.rasterRuntime->config
				|| !source.resolved.role || !source.resolved.role->style
				|| !source.resolved.runtimeFace
				|| !source.resolved.runtimeFace->file)
			{
				throw std::runtime_error("invalid prewarm raster snapshot");
			}
			resolved = source.resolved;
			resolved.role = &runtimeSnapshot.role;
			resolved.runtimeFace = nullptr;
			file = source.resolved.runtimeFace->file;
			faceIndex = source.key.fontFaceIndex;
			key = source.key;
			maskType = source.maskType;
		}
	};

	class PrewarmRasterBatch final : public PrewarmRasterBatchTask
	{
	public:
		PrewarmRasterBatch(const std::vector<PrewarmBitmapWorkItem>& workItems,
			UInt32 workerCount, float safeScale, size_t workChunk)
			: m_safeScale(safeScale), m_workChunk(std::max<size_t>(1, workChunk))
		{
			m_contexts.reserve(workerCount);
			for (UInt32 index = 0; index < workerCount; ++index)
				m_contexts.push_back(
					std::make_unique<PrewarmRasterWorkerContext>());
			m_items.reserve(workItems.size());
			for (const PrewarmBitmapWorkItem& item : workItems)
			{
				PrewarmRasterRuntimeSnapshot* runtimeSnapshot = nullptr;
				for (const std::unique_ptr<PrewarmRasterRuntimeSnapshot>& existing
					: m_runtimeSnapshots)
				{
					if (existing->runtimeIdentity == item.rasterRuntime
						&& existing->styleIdentity
							== (item.resolved.role
								? item.resolved.role->style : nullptr))
					{
						runtimeSnapshot = existing.get();
						break;
					}
				}
				if (!runtimeSnapshot)
				{
					m_runtimeSnapshots.push_back(
						std::make_unique<PrewarmRasterRuntimeSnapshot>(item));
					runtimeSnapshot = m_runtimeSnapshots.back().get();
				}
				m_items.push_back(
					std::make_unique<PrewarmRasterWorkSnapshot>(
						item, *runtimeSnapshot));
			}
		}

		void Run(UInt32 workerIndex) noexcept override
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			try
			{
				if (workerIndex >= m_contexts.size())
					throw std::runtime_error("prewarm worker index out of range");
				PrewarmRasterWorkerContext& context = *m_contexts[workerIndex];
				if (!context.Prepare())
					throw std::runtime_error("prewarm FreeType worker unavailable");
				while (!m_cancelled.load(std::memory_order_acquire)
					&& !IsFontPrewarmStopRequested())
				{
					const size_t first = m_nextWork.fetch_add(
						m_workChunk, std::memory_order_relaxed);
					if (first >= m_items.size())
						break;
					const size_t end = std::min(
						m_items.size(), first + m_workChunk);
					for (size_t index = first; index < end; ++index)
					{
						if (m_cancelled.load(std::memory_order_acquire)
							|| IsFontPrewarmStopRequested())
						{
							break;
						}
						PrewarmRasterWorkSnapshot& item = *m_items[index];
						RuntimeFace* face = GetPrewarmWorkerFace(
							context.state.library, context.faces,
							item.file, item.faceIndex);
						if (!face)
							throw std::runtime_error(
								"prewarm cloned face unavailable");
						ResolvedGlyph workerResolved = item.resolved;
						workerResolved.runtimeFace = face;
						item.bitmap = BuildGlyphBitmap(context.state,
							item.runtime->runtime, workerResolved, item.maskType,
							m_safeScale, item.key);
						if (!item.bitmap)
							throw std::runtime_error(
								"prewarm glyph raster failed");
					}
				}
			}
			catch (const std::bad_alloc&)
			{
				m_allocationFailed.store(true, std::memory_order_release);
				Cancel();
			}
			catch (...)
			{
				m_unexpectedFailure.store(true, std::memory_order_release);
				Cancel();
			}
		}

		void Cancel() noexcept override
		{
			m_cancelled.store(true, std::memory_order_release);
		}

		[[nodiscard]] bool AllocationFailed() const noexcept
		{
			return m_allocationFailed.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool UnexpectedFailure() const noexcept
		{
			return m_unexpectedFailure.load(std::memory_order_acquire);
		}

		void CopyResultsTo(
			std::vector<PrewarmBitmapWorkItem>& workItems) const
		{
			if (workItems.size() != m_items.size())
				throw std::runtime_error("prewarm raster result size mismatch");
			for (size_t index = 0; index < workItems.size(); ++index)
				workItems[index].bitmap = m_items[index]->bitmap;
		}

	private:
		std::vector<std::unique_ptr<PrewarmRasterWorkerContext>> m_contexts;
		std::vector<std::unique_ptr<PrewarmRasterRuntimeSnapshot>>
			m_runtimeSnapshots;
		std::vector<std::unique_ptr<PrewarmRasterWorkSnapshot>> m_items;
		std::atomic<size_t> m_nextWork{ 0 };
		std::atomic_bool m_cancelled{ false };
		std::atomic_bool m_allocationFailed{ false };
		std::atomic_bool m_unexpectedFailure{ false };
		float m_safeScale = 1.0f;
		size_t m_workChunk = 1;
	};

	static bool IsExpensivePrewarmWork(
		const std::vector<PrewarmBitmapWorkItem>& workItems)
	{
		return std::any_of(workItems.begin(), workItems.end(),
			[](const PrewarmBitmapWorkItem& item)
			{
				return item.maskType != GlyphMaskType::Fill;
			});
	}

	static UInt32 ResolvePrewarmWorkerCount(
		const std::vector<PrewarmBitmapWorkItem>& workItems,
		bool expensiveWork, UInt32 maximumWorkers)
	{
		const size_t workCount = workItems.size();
		const size_t parallelThreshold = expensiveWork
			? kExpensivePrewarmParallelThreshold
			: kFillPrewarmParallelThreshold;
		if (workCount < parallelThreshold)
			return 1;
		UInt32 processors = std::thread::hardware_concurrency();
		if (!processors)
		{
			SYSTEM_INFO info = {};
			GetSystemInfo(&info);
			processors = std::max<DWORD>(1, info.dwNumberOfProcessors);
		}
		// Preserve the former four-worker ceiling on small hosts. Wider hosts may
		// use at most half their logical processors (and never more than the shared
		// hard cap), leaving ample capacity for the blocked main thread's window and
		// D3D service plus system work. The memory policy can reduce this further.
		const UInt32 workers = ResolvePrewarmCpuWorkerLimit(processors);
		const size_t usefulWorkers = expensiveWork
			? workCount
			: (workCount + kFillPrewarmWorkChunk - 1u)
				/ kFillPrewarmWorkChunk;
		const UInt32 configuredMaximum = std::clamp<UInt32>(
			maximumWorkers, 1, kMaximumPrewarmRasterWorkers);
		return static_cast<UInt32>(std::min<size_t>(usefulWorkers,
			std::min(std::clamp<UInt32>(
				workers, 1, kMaximumPrewarmRasterWorkers), configuredMaximum)));
	}

	void GetPrewarmGlyphBitmaps(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& results,
		UInt32 maximumWorkers)
	{
		results.assign(requests.size(), nullptr);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		FreeTypeState& state = State();
		std::vector<PrewarmBitmapWorkItem> workItems;
		workItems.reserve(requests.size());
		std::vector<std::pair<size_t, size_t>> duplicates;
		duplicates.reserve(requests.size() / 8);
		UInt64 duplicateCount = 0;
		{
			std::lock_guard<std::recursive_mutex> lock(state.mutex);
			thread_local BitmapBatchDedupeScratch cachedScratch;
			BitmapBatchDedupeScratch& scratch = cachedScratch;
			scratch.Prepare(requests.size());
			const size_t slotMask = scratch.slots.size() - 1;
			for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
			{
				const GlyphBitmapRequest& request = requests[requestIndex];
				if (!request.glyph)
					continue;
				ResolvedGlyph resolved;
				BitmapCacheKey key;
				RuntimeFont* rasterRuntime = nullptr;
				if (!ResolveBitmapCacheKey(runtime, *request.glyph,
					request.maskType, safeScale, request.sdfSpread,
					rasterRuntime, resolved, key) || !rasterRuntime)
				{
					continue;
				}
				size_t slotIndex = BitmapCacheKeyHash{}(key) & slotMask;
				for (;;)
				{
					BitmapBatchDedupeSlot& slot = scratch.slots[slotIndex];
					if (slot.generation != scratch.generation)
					{
						slot.generation = scratch.generation;
						slot.key = key;
						slot.resultIndex = requestIndex;
						PersistentBitmapProfile* persistentProfile = nullptr;
						if (std::shared_ptr<const GlyphBitmap> cached =
							FindCachedGlyphBitmapLocked(state, *rasterRuntime, resolved,
								key, persistentProfile))
						{
							results[requestIndex] = std::move(cached);
						}
						else
						{
							PrewarmBitmapWorkItem item;
							item.requestIndex = requestIndex;
							item.rasterRuntime = rasterRuntime;
							item.resolved = resolved;
							item.key = key;
							item.maskType = request.maskType;
							item.persistentProfile = persistentProfile;
							workItems.push_back(std::move(item));
						}
						break;
					}
					if (slot.key == key)
					{
						++duplicateCount;
						duplicates.push_back({ requestIndex, slot.resultIndex });
						break;
					}
					slotIndex = (slotIndex + 1) & slotMask;
				}
			}
		}

		if (!workItems.empty())
		{
			const bool expensiveWork =
				IsExpensivePrewarmWork(workItems);
			const UInt32 requestedWorkerCount = ResolvePrewarmWorkerCount(
				workItems, expensiveWork, maximumWorkers);
			PrewarmRasterWorkerPool* workerPool = TryGetRasterWorkerPool();
			const UInt32 workerCount = workerPool
				? workerPool->Prepare(requestedWorkerCount) : 0u;
			if (!workerPool || !workerCount)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm raster worker unavailable requested=%u policy=abort-prewarm-runtime-demand",
					requestedWorkerCount);
				RequestFontPrewarmStop();
				throw std::runtime_error("prewarm raster worker unavailable");
			}
			if (workerCount < requestedWorkerCount)
			{
				// A constrained x86 process may refuse another thread stack. Keep
				// the workers that did start and let the caller thread consume the
				// same atomic queue instead of failing the font.
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm worker creation limited requested=%u active=%u; continuing with reduced parallelism",
					requestedWorkerCount, workerCount);
			}
			static UInt32 loggedMaximumWorkers = 0;
			if (workerCount > loggedMaximumWorkers)
			{
				loggedMaximumWorkers = workerCount;
				gLog.FormattedMessage(
					"tnvse_freetype_font: parallel prewarm raster workers=%u batchMisses=%u workload=%s threshold=%u",
					workerCount, static_cast<UInt32>(workItems.size()),
					expensiveWork ? "distance-effect" : "fill",
					expensiveWork
						? kExpensivePrewarmParallelThreshold
						: kFillPrewarmParallelThreshold);
			}
			const size_t workChunk = expensiveWork
				? 1u : kFillPrewarmWorkChunk;
			const std::shared_ptr<PrewarmRasterBatch> batch =
				std::make_shared<PrewarmRasterBatch>(
					workItems, workerCount, safeScale, workChunk);
			const bool batchCompleted = workerPool->Execute(workerCount, batch);
			if (!batchCompleted)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm raster batch timed out or cancelled workers=%u glyphs=%u timeoutMs=%u policy=close-run-commit-rollback-runtime-demand",
					workerCount, static_cast<UInt32>(workItems.size()),
					kPrewarmRasterBatchTimeoutMs);
				RequestFontPrewarmStop();
				throw std::runtime_error("prewarm raster batch timeout");
			}
			if (IsFontPrewarmStopRequested())
				return;
			if (batch->AllocationFailed())
				throw std::bad_alloc();
			if (batch->UnexpectedFailure())
				throw std::runtime_error("parallel prewarm raster worker failed");
			batch->CopyResultsTo(workItems);

			std::lock_guard<std::recursive_mutex> lock(state.mutex);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized,
				static_cast<UInt64>(workItems.size()));

			std::unordered_map<PersistentBitmapProfile*,
				std::vector<PersistentBitmapStoreRequest>> stores;
			for (PrewarmBitmapWorkItem& item : workItems)
			{
				if (item.bitmap && item.persistentProfile)
					stores[item.persistentProfile].push_back(
						{ &item.key, item.bitmap.get() });
			}
			for (auto& pair : stores)
			{
				UInt64 storedBytes = 0;
				const UInt32 stored = StorePersistentGlyphBitmaps(
					*pair.first, pair.second, storedBytes);
				if (stored)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWrite, stored);
					RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWriteBytes,
						storedBytes);
				}
			}
			for (PrewarmBitmapWorkItem& item : workItems)
			{
				if (!item.bitmap)
					continue;
				auto existing = state.bitmapCache.find(item.key);
				if (existing != state.bitmapCache.end())
				{
					TouchBitmapCacheEntry(state, existing->second, item.key);
					results[item.requestIndex] = existing->second.bitmap;
					continue;
				}
				InsertGlyphBitmapCacheLocked(state, *item.rasterRuntime,
					item.key, item.bitmap);
				results[item.requestIndex] = item.bitmap;
			}
			TrimBitmapCache(state);
		}

		for (const auto& duplicate : duplicates)
			results[duplicate.first] = results[duplicate.second];
		RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchRequest,
			static_cast<UInt64>(requests.size()));
		if (duplicateCount)
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchDedupe, duplicateCount);
	}
}
