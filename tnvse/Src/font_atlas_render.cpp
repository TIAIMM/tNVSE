#include "font_atlas_internal.h"

#include "globals.h"
#include "load_config.h"

namespace fonthook::vectorfont
{
	NiTriShape* TryCreateGlyphAtlasShape(Font& font, RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		bool prepareObject, const NiColorA& requestedTileColor, bool suppressEffects)
	{
		if (glyphs.empty())
			return nullptr;
		AtlasState& state = State();
		const FontConfig& config = GetRuntimeConfig(runtime);
		const NiColorA tileColor = ResolveSafeTileColor(glyphs, requestedTileColor);
		const bool hasEffects = !suppressEffects
			&& (config.shadow.enabled || config.glow.enabled || config.outline.enabled);
		const bool requestsSdfFill = UsesSdfFill(config);
		const bool wantsShaderPath = hasEffects || requestsSdfFill;
		EffectQuality resolvedQuality = config.effectQuality;
		if (wantsShaderPath && IsA8RendererAvailable()
			&& ResolveA8EffectQuality(config.effectQuality, resolvedQuality))
		{
			if (resolvedQuality != config.effectQuality && g_bEnableFreeTypeFontRenderingLog)
			{
				const UInt64 downgradeKey = (static_cast<UInt64>(font.iFontNum) << 32)
					| (static_cast<UInt32>(config.effectQuality) << 8)
					| static_cast<UInt32>(resolvedQuality);
				bool shouldLog = false;
				{
					std::lock_guard<std::mutex> lock(state.atlasMutex);
					shouldLog = state.loggedQualityDowngrades.insert(downgradeKey).second;
				}
				if (shouldLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: effect shader quality downgraded font=%u requested=%u resolved=%u",
						font.iFontNum, static_cast<UInt32>(config.effectQuality),
						static_cast<UInt32>(resolvedQuality));
				}
			}
			thread_local std::vector<PendingQuad> shaderQuads;
			ShaderEffectBuild shaderBuild;
			const bool shaderQuadsBuilt = BuildShaderEffectQuads(runtime, glyphs,
				rasterScale, resolvedQuality, tileColor, suppressEffects,
				shaderQuads, shaderBuild);
			// Spaces and control-only fragments legitimately have metrics but no bitmap
			// area. They need no shape and are not a shader failure; falling through to
			// CPU masks only wastes work and consumes the real failure-log quota.
			if (shaderQuadsBuilt && shaderQuads.empty())
				return nullptr;
			const bool shaderQuadCountValid = shaderQuadsBuilt && !shaderQuads.empty()
				&& shaderQuads.size() <= kMaximumQuads;
			bool shaderAtlasOrShapeFailed = false;
			if (shaderQuadCountValid)
			{
				thread_local std::unordered_map<UInt64,
					std::shared_ptr<const GlyphBitmap>> shaderUnique;
				shaderUnique.clear();
				for (const PendingQuad& quad : shaderQuads)
					shaderUnique.emplace(quad.bitmap->cacheId, quad.bitmap);
				thread_local std::vector<std::shared_ptr<const GlyphBitmap>> shaderBitmaps;
				shaderBitmaps.clear();
				shaderBitmaps.reserve(shaderUnique.size());
				for (auto& [id, bitmap] : shaderUnique)
					shaderBitmaps.push_back(std::move(bitmap));
				std::sort(shaderBitmaps.begin(), shaderBitmaps.end(),
					[](const auto& lhs, const auto& rhs)
					{
						return lhs->cacheId < rhs->cacheId;
					});
				std::vector<std::shared_ptr<AtlasResource>> shaderAtlases;
				NiTriShape* shaderShape = TryCreateAtlasShapeForMode(font,
					shaderQuads, shaderBitmaps, config, rasterScale, prepareObject,
					AtlasPixelMode::A8, AtlasRenderMode::ShaderEffects,
					shaderBuild.padding, shaderAtlases, tileColor, true,
					&shaderBuild.config);
				if (shaderShape)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectBatch);
					const UInt64 avoided = static_cast<UInt64>(glyphs.size())
						* static_cast<UInt64>((config.glow.enabled ? 1 : 0)
							+ (config.outline.enabled ? 1 : 0));
					RecordFreeTypePerf(FreeTypePerfCounter::CpuEffectMasksAvoided, avoided);
					if (g_bEnableFreeTypeFontRenderingLog)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: shader batch font=%u requestedFill=%s resolvedFill=%s quality=%u spread=%.0f glyphs=%u quads=%u padding=%u pages=%u texture0=%ux%u abi=%u",
							font.iFontNum,
							requestsSdfFill ? "sdf" : "grayscale",
							shaderBuild.config.fillUsesSdf ? "sdf" : "grayscale",
							static_cast<UInt32>(resolvedQuality),
							shaderBuild.config.sdfSpreadPixels,
							static_cast<UInt32>(glyphs.size()),
							static_cast<UInt32>(shaderQuads.size()), shaderBuild.padding,
							static_cast<UInt32>(shaderAtlases.size()),
							shaderAtlases[0]->width, shaderAtlases[0]->height,
							A8ShapeColorContract::kTileUniformColorAbi);
					}
					return shaderShape;
				}
				shaderAtlasOrShapeFailed = true;
			}
			if (g_bEnableFreeTypeFontRenderingLog
				&& state.shaderBatchFailureLogCount++ < 32)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: shader batch failed font=%u stage=%s requestedFill=%s resolvedFill=grayscale quads=%u; using CPU masks",
					font.iFontNum,
					!shaderQuadsBuilt ? "mask-build"
						: shaderQuads.empty() ? "empty-batch"
						: shaderQuads.size() > kMaximumQuads ? "quad-limit"
						: shaderAtlasOrShapeFailed ? "atlas-or-shape"
						: "unknown",
					requestsSdfFill ? "sdf" : "grayscale",
					static_cast<UInt32>(shaderQuads.size()));
			}
		}
		std::array<bool, 4> included = {
			!suppressEffects && config.shadow.enabled,
			!suppressEffects && config.glow.enabled,
			!suppressEffects && config.outline.enabled,
			true
		};
		const std::array<AtlasLayer, 3> degradationOrder = {
			AtlasLayer::Glow, AtlasLayer::Shadow, AtlasLayer::Outline
		};
		for (size_t attempt = 0; attempt <= degradationOrder.size(); ++attempt)
		{
			thread_local std::vector<PendingQuad> quads;
			quads.clear();
			PendingQuadBuildFailure buildFailure = PendingQuadBuildFailure::None;
			if (!BuildPendingQuads(runtime, glyphs, rasterScale, included,
				tileColor, quads, buildFailure))
			{
				AtlasLayer failedLayer = AtlasLayer::Fill;
				if (buildFailure == PendingQuadBuildFailure::Glow)
					failedLayer = AtlasLayer::Glow;
				else if (buildFailure == PendingQuadBuildFailure::Outline)
					failedLayer = AtlasLayer::Outline;
				const bool optionalFailure = buildFailure != PendingQuadBuildFailure::Fill
					&& included[static_cast<size_t>(failedLayer)];
				if (g_bEnableFreeTypeFontRenderingLog
					&& state.cpuMaskFailureLogCount++ < 32)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: CPU mask batch failure font=%u layer=%s action=%s",
						font.iFontNum,
						buildFailure == PendingQuadBuildFailure::Fill ? "fill"
							: buildFailure == PendingQuadBuildFailure::Glow ? "glow"
							: buildFailure == PendingQuadBuildFailure::Outline ? "outline"
							: "unknown",
						optionalFailure ? "disable-layer-and-retry" : "vector-fallback");
				}
				if (optionalFailure)
				{
					included[static_cast<size_t>(failedLayer)] = false;
					continue;
				}
				return nullptr;
			}
			if (quads.empty())
				return nullptr;
			if (quads.size() <= kMaximumQuads)
			{
				thread_local std::unordered_map<UInt64,
					std::shared_ptr<const GlyphBitmap>> unique;
				unique.clear();
				for (const PendingQuad& quad : quads)
					unique.emplace(quad.bitmap->cacheId, quad.bitmap);
				thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps;
				bitmaps.clear();
				bitmaps.reserve(unique.size());
				for (auto& [id, bitmap] : unique)
					bitmaps.push_back(std::move(bitmap));
				std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs->cacheId < rhs->cacheId;
				});
				bool useCustomA8Shader = IsA8RendererAvailable();
				AtlasPixelMode pixelMode = useCustomA8Shader
					? AtlasPixelMode::A8 : AtlasPixelMode::Argb32;
				std::vector<std::shared_ptr<AtlasResource>> atlases;
				NiTriShape* shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
					config, rasterScale, prepareObject, pixelMode,
					AtlasRenderMode::CpuEffects, kAtlasPadding, atlases, tileColor,
					useCustomA8Shader);
				if (!shape && pixelMode == AtlasPixelMode::A8)
				{
					// A failed A8 attempt may coincide with a device/Shader Loader
					// transition. Snapshot a fresh, internally consistent route for the
					// ARGB retry rather than mixing baked colors with a custom shader.
					useCustomA8Shader = IsA8RendererAvailable();
					pixelMode = AtlasPixelMode::Argb32;
					shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
						config, rasterScale, prepareObject, pixelMode,
						AtlasRenderMode::CpuEffects, kAtlasPadding, atlases, tileColor,
						useCustomA8Shader);
				}
				if (shape)
				{
					AtlasResource& atlas = *atlases[0];
					pixelMode = atlas.pixelMode;
					const UInt64 logKey = (static_cast<UInt64>(font.iFontNum) << 32)
						| (static_cast<UInt32>(std::lround(rasterScale * 1000.0f)) << 1)
						| static_cast<UInt32>(pixelMode);
					bool shouldLog = false;
					if (g_bEnableFreeTypeFontRenderingLog)
					{
						std::lock_guard<std::mutex> lock(state.atlasMutex);
						shouldLog = state.loggedAtlasBatches.insert(logKey).second;
					}
					if (shouldLog)
					{
						UInt64 gpuBytes = 0;
						UInt64 residentMaskBytes = 0;
						UInt64 compactSnapshotBytes = 0;
						for (const auto& page : atlases)
						{
							if (!page)
								continue;
							gpuBytes += GetAtlasStorageBytes(page->width, page->height,
								page->pixelMode, page->mipLevels);
							residentMaskBytes += GetResidentMaskBytes(*page);
							compactSnapshotBytes += GetCompactSnapshotBytes(*page);
						}
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: atlas batch font=%u sourceScale=%.3f mode=%s backend=%s glyphs=%u quads=%u pages=%u texture0=%ux%u levels=%u generation=%u gpuBytes=%llu residentMaskBytes=%llu compactSnapshotBytes=%llu",
							font.iFontNum, rasterScale,
							pixelMode == AtlasPixelMode::A8 ? "a8" : "argb32",
							atlas.backend == AtlasBackend::DefaultPool
								? "default" : "managed",
							static_cast<UInt32>(glyphs.size()),
							static_cast<UInt32>(quads.size()),
							static_cast<UInt32>(atlases.size()),
							atlas.width, atlas.height, atlas.mipLevels, atlas.generation,
							static_cast<unsigned long long>(gpuBytes),
							static_cast<unsigned long long>(residentMaskBytes),
							static_cast<unsigned long long>(compactSnapshotBytes));
					}
					return shape;
				}
			}
			if (attempt < degradationOrder.size())
			{
				for (AtlasLayer layer : degradationOrder)
				{
					if (included[static_cast<size_t>(layer)])
					{
						included[static_cast<size_t>(layer)] = false;
						break;
					}
				}
			}
		}

		if (state.atlasFailureLogCount++ < 32)
			gLog.FormattedMessage("tnvse_freetype_font: atlas batch failed font=%u; using vector fallback",
				font.iFontNum);
		return nullptr;
	}
}

namespace fonthook
{
	void InitializeFreeTypeDefaultPoolAtlas()
	{
		vectorfont::InitializeDefaultPoolAtlasLifecycle();
	}

	void HandleFreeTypeDefaultPoolAtlasMainLoop()
	{
		vectorfont::PumpDefaultPoolAtlasLifecycle();
	}

	void ShutdownFreeTypeDefaultPoolAtlas()
	{
		vectorfont::ShutdownDefaultPoolAtlasLifecycle();
	}
}
