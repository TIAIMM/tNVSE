#include "font_atlas_internal.h"
#include "font_render_route.h"

#include "globals.h"
#include "load_config.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_render {}
	using namespace implementation::font_atlas_render;

	namespace implementation::font_atlas_render
	{
		bool IsControlCodePoint(UInt32 codePoint)
		{
			return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
		}

		bool IsSpaceCodePoint(UInt32 codePoint)
		{
			return codePoint == 0x20 || codePoint == 0xA0 || codePoint == 0x1680
				|| (codePoint >= 0x2000 && codePoint <= 0x200A)
				|| codePoint == 0x202F || codePoint == 0x205F || codePoint == 0x3000;
		}

		GlyphAtlasMaskFailure ToDiagnosticMaskFailure(
			PendingQuadBuildFailure failure)
		{
			switch (failure)
			{
			case PendingQuadBuildFailure::Fill:
				return GlyphAtlasMaskFailure::Fill;
			case PendingQuadBuildFailure::Shadow:
				return GlyphAtlasMaskFailure::Shadow;
			case PendingQuadBuildFailure::Glow:
				return GlyphAtlasMaskFailure::Glow;
			case PendingQuadBuildFailure::Outline:
				return GlyphAtlasMaskFailure::Outline;
			default:
				return GlyphAtlasMaskFailure::None;
			}
		}

		void InitializeBuildDiagnostics(const std::vector<AtlasGlyphInstance>& glyphs,
			GlyphAtlasBuildDiagnostics* diagnostics)
		{
			if (!diagnostics)
				return;
			*diagnostics = {};
			diagnostics->inputGlyphCount = static_cast<UInt32>(glyphs.size());
			if (!glyphs.empty())
			{
				diagnostics->firstEncodedCode = glyphs.front().glyph.encodedCode;
				diagnostics->firstCodePoint = glyphs.front().glyph.codePoint;
				diagnostics->firstGlyphIndex = glyphs.front().glyph.glyphIndex;
				diagnostics->firstByteLength = glyphs.front().glyph.byteLength;
				diagnostics->firstByteClass = static_cast<UInt8>(
					glyphs.front().glyph.byteClass);
			}
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				if (!HasVectorGlyphMetrics(instance.glyph))
					++diagnostics->missingMetricsCount;
				if (!instance.glyph.byteLength)
					++diagnostics->zeroByteLengthCount;
				if (IsControlCodePoint(instance.glyph.codePoint))
					++diagnostics->controlGlyphCount;
				else if (IsSpaceCodePoint(instance.glyph.codePoint))
					++diagnostics->spaceGlyphCount;
			}
		}
	}

	NiTriShape* TryCreateSealedGlyphAtlasShape(Font& font,
		RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& profile,
		const std::vector<DirectGlyphCommand>& glyphs, float rasterScale,
		bool prepareObject, const NiColorA& tileColor,
		bool suppressEffects, GlyphAtlasBuildDiagnostics* diagnostics)
	{
		if (diagnostics)
		{
			*diagnostics = {};
			diagnostics->inputGlyphCount =
				static_cast<UInt32>(glyphs.size());
			if (!glyphs.empty())
			{
				diagnostics->firstEncodedCode =
					glyphs.front().encodedCode;
				diagnostics->firstByteLength =
					glyphs.front().byteLength;
				diagnostics->firstByteClass =
					glyphs.front().byteClass;
			}
			for (const DirectGlyphCommand& glyph : glyphs)
			{
				if (glyph.encodedCode < 0x20
					|| (glyph.encodedCode >= 0x7F
						&& glyph.encodedCode <= 0x9F))
				{
					++diagnostics->controlGlyphCount;
				}
				else if (glyph.encodedCode == 0x20
					|| glyph.encodedCode == 0xA0)
				{
					++diagnostics->spaceGlyphCount;
				}
			}
		}
		if (!profile)
		{
			if (diagnostics)
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
			return nullptr;
		}
		if (glyphs.empty())
		{
			if (diagnostics)
			{
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::EmptyInput;
				diagnostics->expectedEmpty = true;
			}
			return nullptr;
		}
		if (!State().directProfilesAvailable.load(
			std::memory_order_acquire))
		{
			// A DEFAULT-pool reset revokes GPU publication without discarding
			// immutable metrics or reopening FreeType. Reject only this shape;
			// the same profile becomes usable again after page restoration.
			if (diagnostics)
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
			return nullptr;
		}

		const bool precomposed =
			profile->recordKind
				== DirectCachedLetterKind::VanillaFontLetter
			&& profile->renderMode == AtlasRenderMode::CpuEffects
			&& profile->pixelMode == AtlasPixelMode::Argb32;
		const bool distanceField =
			profile->recordKind
				== DirectCachedLetterKind::EffectLayers
			&& profile->renderMode == AtlasRenderMode::ShaderEffects;
		const bool cpuEffects =
			profile->recordKind
				== DirectCachedLetterKind::EffectLayers
			&& profile->renderMode == AtlasRenderMode::CpuEffects
			&& profile->pixelMode == AtlasPixelMode::A8;
		if (!precomposed && !distanceField && !cpuEffects)
		{
			InvalidateSealedDirectFontProfileIfCurrent(runtime, profile);
			if (diagnostics)
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
			return nullptr;
		}

		EffectQuality quality = GetRuntimeConfig(runtime).effectQuality;
		if (distanceField)
			ResolveA8EffectQuality(quality, quality);
		if (diagnostics)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			diagnostics->hasEffects = !suppressEffects
				&& (config.shadow.enabled || config.glow.enabled
					|| config.outline.enabled);
			diagnostics->requestsSdfFill = distanceField;
			diagnostics->wantsShaderPath = true;
			diagnostics->a8RendererAvailable =
				IsA8RendererAvailable();
			diagnostics->requestedQuality =
				static_cast<UInt8>(config.effectQuality);
			diagnostics->resolvedQuality =
				static_cast<UInt8>(quality);
		}

		DirectAtlasShapeBuildResult direct;
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::DirectCompile);
			direct = cpuEffects
				? TryCreateSealedCpuEffectShape(
					font, runtime, profile, glyphs, rasterScale,
					prepareObject, tileColor, suppressEffects)
				: TryCreateDirectCachedLetterShape(
					font, runtime, profile, glyphs, rasterScale,
					prepareObject, tileColor, suppressEffects,
					precomposed ? GlyphMaskType::Composite
						: GlyphMaskType::DistanceField,
					quality);
		}
		switch (direct.outcome)
		{
		case DirectAtlasShapeOutcome::Created:
			if (diagnostics)
			{
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::Created;
				if (distanceField)
				{
					diagnostics->shaderQuadsBuilt = true;
					diagnostics->shaderQuadCount =
						direct.geometryQuadCount;
					++diagnostics->shaderShapeAttempts;
				}
				else
				{
					diagnostics->cpuQuadsBuilt = true;
					diagnostics->cpuQuadCount =
						direct.geometryQuadCount;
					++diagnostics->cpuAttempts;
					++diagnostics->cpuShapeAttempts;
				}
			}
			if (distanceField)
				RecordFreeTypePerf(
					FreeTypePerfCounter::ShaderEffectBatch);
			return direct.shape;
		case DirectAtlasShapeOutcome::Empty:
			if (diagnostics)
			{
				diagnostics->outcome = distanceField
					? GlyphAtlasBuildOutcome::NoDrawableShaderQuads
					: GlyphAtlasBuildOutcome::NoDrawableCpuQuads;
				diagnostics->expectedEmpty = true;
			}
			return nullptr;
		default:
			InvalidateSealedDirectFontProfileIfCurrent(runtime, profile);
			if (diagnostics)
			{
				diagnostics->outcome =
					GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
				diagnostics->shaderAtlasOrShapeFailed =
					distanceField;
			}
			return nullptr;
		}
	}

	NiTriShape* TryCreateGlyphAtlasShape(Font& font, RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		bool prepareObject, const NiColorA& requestedTileColor, bool suppressEffects,
		GlyphAtlasBuildDiagnostics* diagnostics)
	{
		InitializeBuildDiagnostics(glyphs, diagnostics);
		if (glyphs.empty())
		{
			if (diagnostics)
			{
				diagnostics->outcome = GlyphAtlasBuildOutcome::EmptyInput;
				diagnostics->expectedEmpty = true;
			}
			return nullptr;
		}
		AtlasState& state = State();
		const FontConfig& config = GetRuntimeConfig(runtime);
		const NiColorA tileColor = ResolveSafeTileColor(glyphs, requestedTileColor);
		const bool hasEffects = !suppressEffects
			&& (config.shadow.enabled || config.glow.enabled || config.outline.enabled);
		const bool a8RendererAvailable = IsA8RendererAvailable();
		const FontAtlasRoute atlasRoute = ResolveFontAtlasRoute(
			a8RendererAvailable,
			UsesBakedEffectRoute());
		const bool requestsDistanceField =
			atlasRoute == FontAtlasRoute::ShaderDistanceField;
		const bool requestsBakedCoverage =
			atlasRoute == FontAtlasRoute::ShaderA8Coverage;
		const bool wantsShaderPath =
			atlasRoute != FontAtlasRoute::ArgbFallback;
		if (diagnostics)
		{
			diagnostics->hasEffects = hasEffects;
			diagnostics->requestsSdfFill = requestsDistanceField;
			diagnostics->wantsShaderPath = wantsShaderPath;
			diagnostics->a8RendererAvailable = a8RendererAvailable;
			diagnostics->requestedQuality = static_cast<UInt8>(config.effectQuality);
			diagnostics->resolvedQuality = diagnostics->requestedQuality;
		}
		EffectQuality resolvedQuality = config.effectQuality;
		if (atlasRoute == FontAtlasRoute::ShaderDistanceField
			&& ResolveA8EffectQuality(config.effectQuality, resolvedQuality))
		{
			if (diagnostics)
				diagnostics->resolvedQuality = static_cast<UInt8>(resolvedQuality);
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
			DirectAtlasShapeBuildResult directShape =
				TryCreateDirectCachedLetterShape(font, runtime, glyphs,
					rasterScale, prepareObject, tileColor, suppressEffects,
					GlyphMaskType::DistanceField, resolvedQuality);
			if (directShape.outcome == DirectAtlasShapeOutcome::Empty)
			{
				if (diagnostics)
				{
					diagnostics->shaderQuadsBuilt = true;
					diagnostics->shaderQuadCount = 0;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::NoDrawableShaderQuads;
					diagnostics->expectedEmpty = true;
				}
				return nullptr;
			}
			if (directShape.outcome == DirectAtlasShapeOutcome::Created)
			{
				if (diagnostics)
				{
					diagnostics->shaderQuadsBuilt = true;
					diagnostics->shaderQuadCount =
						directShape.geometryQuadCount;
					++diagnostics->shaderShapeAttempts;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::Created;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::ShaderEffectBatch);
				const UInt64 avoided =
					static_cast<UInt64>(glyphs.size())
					* static_cast<UInt64>(
						(config.glow.enabled ? 1 : 0)
						+ (config.outline.enabled ? 1 : 0));
				RecordFreeTypePerf(
					FreeTypePerfCounter::CpuEffectMasksAvoided, avoided);
				return directShape.shape;
			}
			if (directShape.outcome == DirectAtlasShapeOutcome::Failed)
			{
				if (diagnostics)
				{
					diagnostics->shaderAtlasOrShapeFailed = true;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
				}
				return nullptr;
			}
			thread_local std::vector<PendingQuad> shaderQuads;
			ShaderEffectBuild shaderBuild;
			const bool shaderQuadsBuilt = BuildShaderEffectQuads(runtime, glyphs,
				rasterScale, resolvedQuality, tileColor, suppressEffects,
				shaderQuads, shaderBuild);
			if (diagnostics)
			{
				diagnostics->shaderQuadsBuilt = shaderQuadsBuilt;
				diagnostics->shaderQuadCount = static_cast<UInt32>(shaderQuads.size());
			}
			// Spaces and control-only fragments legitimately have metrics but no bitmap
			// area. They need no shape and are not a shader failure; falling through to
			// CPU masks only wastes work and consumes the real failure-log quota.
			if (shaderQuadsBuilt && shaderQuads.empty())
			{
				if (diagnostics)
				{
					diagnostics->outcome = GlyphAtlasBuildOutcome::NoDrawableShaderQuads;
					diagnostics->expectedEmpty = true;
				}
				return nullptr;
			}
			const bool shaderQuadCountValid = shaderQuadsBuilt && !shaderQuads.empty()
				&& shaderQuads.size() <= kMaximumQuads;
			bool shaderAtlasOrShapeFailed = false;
			if (shaderQuadCountValid)
			{
				if (diagnostics)
					++diagnostics->shaderShapeAttempts;
				std::vector<std::shared_ptr<AtlasResource>> shaderAtlases;
				NiTriShape* shaderShape = TryCreateAtlasShapeForMode(font,
					shaderQuads, config, rasterScale, prepareObject,
					GetConfiguredDistanceFieldAtlasPixelMode(),
					AtlasRenderMode::ShaderEffects,
					shaderBuild.padding, shaderAtlases, tileColor, true,
					&shaderBuild.config);
				if (shaderShape)
				{
					if (diagnostics)
						diagnostics->outcome = GlyphAtlasBuildOutcome::Created;
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectBatch);
					const UInt64 avoided = static_cast<UInt64>(glyphs.size())
						* static_cast<UInt64>((config.glow.enabled ? 1 : 0)
							+ (config.outline.enabled ? 1 : 0));
					RecordFreeTypePerf(FreeTypePerfCounter::CpuEffectMasksAvoided, avoided);
					return shaderShape;
				}
				shaderAtlasOrShapeFailed = true;
				if (diagnostics)
					diagnostics->shaderAtlasOrShapeFailed = true;
			}
			if (g_bEnableFreeTypeFontRenderingLog
				&& state.shaderBatchFailureLogCount++ < 32)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: shader batch failed font=%u stage=%s requestedFill=%s resolvedRoute=argb-fallback quads=%u; using CPU masks",
					font.iFontNum,
					!shaderQuadsBuilt ? "mask-build"
						: shaderQuads.empty() ? "empty-batch"
						: shaderQuads.size() > kMaximumQuads ? "quad-limit"
						: shaderAtlasOrShapeFailed ? "atlas-or-shape"
						: "unknown",
					GetConfiguredDistanceFieldMethodName(),
					static_cast<UInt32>(shaderQuads.size()));
			}
		}
		if (requestsBakedCoverage)
		{
			DirectAtlasShapeBuildResult directShape =
				TryCreateDirectCachedLetterShape(font, runtime, glyphs,
					rasterScale, prepareObject, tileColor, suppressEffects,
					GlyphMaskType::Composite, resolvedQuality);
			if (directShape.outcome == DirectAtlasShapeOutcome::Empty)
			{
				if (diagnostics)
				{
					diagnostics->cpuQuadsBuilt = true;
					diagnostics->cpuQuadCount = 0;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::NoDrawableCpuQuads;
					diagnostics->expectedEmpty = true;
				}
				return nullptr;
			}
			if (directShape.outcome == DirectAtlasShapeOutcome::Created)
			{
				if (diagnostics)
				{
					++diagnostics->cpuAttempts;
					++diagnostics->cpuShapeAttempts;
					diagnostics->cpuQuadsBuilt = true;
					diagnostics->cpuQuadCount =
						directShape.geometryQuadCount;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::Created;
				}
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: direct aggressive composite batch font=%u sourceScale=%.3f glyphs=%u quads=%u pages=%u texture0=%ux%u compiler=dense-cached-letter",
						font.iFontNum, rasterScale,
						static_cast<UInt32>(glyphs.size()),
						directShape.geometryQuadCount,
						directShape.pageCount,
						directShape.firstAtlasWidth,
						directShape.firstAtlasHeight);
				}
				return directShape.shape;
			}
			if (directShape.outcome == DirectAtlasShapeOutcome::Failed)
			{
				if (state.aggressiveCompositeShapeFailureLogCount++ < 32)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: direct aggressive composite shape failed font=%u glyphs=%u quads=%u pages=%u; batch rejected",
						font.iFontNum,
						static_cast<UInt32>(glyphs.size()),
						directShape.geometryQuadCount,
						directShape.pageCount);
				}
				if (diagnostics)
				{
					diagnostics->cpuQuadsBuilt =
						directShape.geometryQuadCount != 0;
					diagnostics->cpuQuadCount =
						directShape.geometryQuadCount;
					diagnostics->outcome =
						GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
				}
				return nullptr;
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
			if (diagnostics)
				++diagnostics->cpuAttempts;
			thread_local std::vector<PendingQuad> quads;
			quads.clear();
			PendingQuadBuildFailure buildFailure = PendingQuadBuildFailure::None;
			if (!BuildPendingQuads(runtime, glyphs, rasterScale, included,
				tileColor, requestsBakedCoverage, quads, buildFailure))
			{
				if (diagnostics)
				{
					diagnostics->cpuQuadsBuilt = false;
					diagnostics->cpuQuadCount = static_cast<UInt32>(quads.size());
					diagnostics->cpuMaskFailure = ToDiagnosticMaskFailure(buildFailure);
				}
				AtlasLayer failedLayer = AtlasLayer::Fill;
				if (buildFailure == PendingQuadBuildFailure::Shadow)
					failedLayer = AtlasLayer::Shadow;
				else if (buildFailure == PendingQuadBuildFailure::Glow)
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
							: buildFailure == PendingQuadBuildFailure::Shadow
								? "shadow"
								: buildFailure == PendingQuadBuildFailure::Glow ? "glow"
								: buildFailure == PendingQuadBuildFailure::Outline
									? "outline" : "unknown",
						optionalFailure ? "disable-layer-and-retry" : "shape-build-failed");
				}
				if (optionalFailure)
				{
					included[static_cast<size_t>(failedLayer)] = false;
					if (diagnostics)
						++diagnostics->degradedLayerCount;
					continue;
				}
				if (diagnostics)
					diagnostics->outcome = GlyphAtlasBuildOutcome::CpuMaskBuildFailure;
				return nullptr;
			}
			if (diagnostics)
			{
				diagnostics->cpuQuadsBuilt = true;
				diagnostics->cpuQuadCount = static_cast<UInt32>(quads.size());
				diagnostics->cpuMaskFailure = GlyphAtlasMaskFailure::None;
			}
			if (quads.empty())
			{
				if (diagnostics)
				{
					diagnostics->outcome = GlyphAtlasBuildOutcome::NoDrawableCpuQuads;
					diagnostics->expectedEmpty = true;
				}
				return nullptr;
			}
			if (quads.size() <= kMaximumQuads)
			{
				if (requestsBakedCoverage)
				{
					if (diagnostics)
						++diagnostics->cpuShapeAttempts;
					std::vector<std::shared_ptr<AtlasResource>>
						compositeAtlases;
					A8EffectShapeConfig compositeConfig;
					compositeConfig.enabled = true;
					compositeConfig.precomposedArgb = true;
					NiTriShape* compositeShape = TryCreateAtlasShapeForMode(
						font, quads, config, rasterScale, prepareObject,
						AtlasPixelMode::Argb32, AtlasRenderMode::CpuEffects,
						kArgbAtlasPadding, compositeAtlases, tileColor, false,
						&compositeConfig);
					if (compositeShape)
					{
						if (diagnostics)
							diagnostics->outcome =
								GlyphAtlasBuildOutcome::Created;
						bool shouldLog = false;
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							const UInt64 logKey =
								(static_cast<UInt64>(font.iFontNum) << 32)
								| (static_cast<UInt32>(std::lround(
									rasterScale * 1000.0f)) << 1)
								| static_cast<UInt32>(AtlasPixelMode::Argb32);
							std::lock_guard<std::mutex> lock(state.atlasMutex);
							shouldLog =
								state.loggedAtlasBatches.insert(logKey).second;
						}
						if (shouldLog)
						{
							const AtlasResource& first = *compositeAtlases[0];
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: aggressive BGRA composite batch font=%u sourceScale=%.3f glyphs=%u quads=%u pages=%u texture0=%ux%u levels=%u backend=%s singleQuad=1",
								font.iFontNum, rasterScale,
								static_cast<UInt32>(glyphs.size()),
								static_cast<UInt32>(quads.size()),
								static_cast<UInt32>(compositeAtlases.size()),
								first.width, first.height, first.mipLevels,
								first.backend == AtlasBackend::DefaultPool
									? "default" : "managed");
						}
						return compositeShape;
					}
					if (state.aggressiveCompositeShapeFailureLogCount++ < 32)
						gLog.FormattedMessage(
							"tnvse_freetype_font: aggressive BGRA composite shape failed font=%u quads=%u; batch rejected",
							font.iFontNum,
							static_cast<UInt32>(quads.size()));
					if (diagnostics)
						diagnostics->outcome =
							GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
					return nullptr;
				}

				constexpr bool useCustomA8Shader = false;
				AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
				std::vector<std::shared_ptr<AtlasResource>> atlases;
				if (diagnostics)
					++diagnostics->cpuShapeAttempts;
				NiTriShape* shape = TryCreateAtlasShapeForMode(font, quads,
					config, rasterScale, prepareObject, pixelMode,
					AtlasRenderMode::CpuEffects, kArgbAtlasPadding, atlases, tileColor,
					useCustomA8Shader);
				if (shape)
				{
					if (diagnostics)
						diagnostics->outcome = GlyphAtlasBuildOutcome::Created;
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
							pixelMode == AtlasPixelMode::A8 ? "a8"
								: pixelMode == AtlasPixelMode::Mtsdf32
									? "mtsdf32" : "argb32",
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
				if (diagnostics)
					diagnostics->outcome = GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
			}
			else if (diagnostics)
			{
				diagnostics->outcome = GlyphAtlasBuildOutcome::QuadLimit;
			}
			if (attempt < degradationOrder.size())
			{
				for (AtlasLayer layer : degradationOrder)
				{
					if (included[static_cast<size_t>(layer)])
					{
						included[static_cast<size_t>(layer)] = false;
						if (diagnostics)
							++diagnostics->degradedLayerCount;
						break;
					}
				}
			}
		}

		if (state.atlasFailureLogCount++ < 32)
			gLog.FormattedMessage("tnvse_freetype_font: atlas batch failed font=%u; returning empty shape",
				font.iFontNum);
		if (diagnostics && diagnostics->outcome == GlyphAtlasBuildOutcome::Unknown)
			diagnostics->outcome = GlyphAtlasBuildOutcome::AtlasOrShapeFailure;
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
