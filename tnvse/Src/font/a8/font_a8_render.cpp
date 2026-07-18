#include "font_a8_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiTriShapeData.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fonthook::vectorfont
{
	namespace
	{
		A8State s_a8State;

		bool IsFiniteColor(const NiColorA& color)
		{
			return std::isfinite(color.r) && std::isfinite(color.g)
				&& std::isfinite(color.b) && std::isfinite(color.a);
		}

		bool RejectA8Shape(const char* reason)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& State().shapeValidationFailureLogCount++
					< kMaximumShapeValidationFailureLogs)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag: rejected shape contract=tile-fill-effect-rgb-v8 reason=%s",
					reason ? reason : "unknown");
			}
			return false;
		}

		bool ValidateA8Shape(NiTriShape* shape,
			const A8EffectShapeConfig* effectConfig,
			const A8ShapeColorContract* colorContract)
		{
			if (!shape || !colorContract)
				return RejectA8Shape("missing-shape-or-color-contract");
			if (colorContract->abiVersion
				!= A8ShapeColorContract::kTileUniformColorAbi)
			{
				return RejectA8Shape("color-contract-abi-mismatch");
			}
			if (!IsFiniteColor(colorContract->minimumModifier)
				|| !IsFiniteColor(colorContract->maximumModifier))
			{
				return RejectA8Shape("non-finite-color-contract");
			}

			NiTriShapeData* data = shape->GetModelData();
			if (!data || !data->m_usVertices || !data->m_usTriangles
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				return RejectA8Shape("missing-geometry-data");
			}
			if (data->m_pkColor)
				return RejectA8Shape("unexpected-vertex-color-stream");
			if (!effectConfig || !effectConfig->enabled)
				return true;

			const std::array<float, 9> scalarValues = {
				effectConfig->inverseAtlasWidth,
				effectConfig->inverseAtlasHeight,
				effectConfig->sdfSpreadPixels,
				effectConfig->shadowBlurPixels,
				effectConfig->shadowPower,
				effectConfig->glowInnerPixels,
				effectConfig->glowOuterPixels,
				effectConfig->glowPower,
				effectConfig->outlineWidthPixels
			};
			if (!std::all_of(scalarValues.begin(), scalarValues.end(),
				[](float value) { return std::isfinite(value); })
				|| !std::isfinite(effectConfig->outlineSoftnessPixels))
			{
				return RejectA8Shape("non-finite-effect-configuration");
			}
			if (static_cast<UInt32>(effectConfig->quality)
				> static_cast<UInt32>(EffectQuality::High))
			{
				return RejectA8Shape("invalid-effect-quality");
			}

			const UInt64 triangleIndices = static_cast<UInt64>(data->m_usTriangles) * 3;
			const UInt64 availableIndices = data->m_uiTriListLength
				? std::min<UInt64>(triangleIndices, data->m_uiTriListLength)
				: triangleIndices;
			UInt64 previousVertexEnd = 0;
			UInt64 previousIndexEnd = 0;
			UInt32 previousLayerRank = 0;
			bool firstRange = true;
			bool haveFill = false;
			if (effectConfig->atlasTextures.size()
				!= effectConfig->atlasInverseSizes.size())
			{
				return RejectA8Shape("atlas-page-metadata-size-mismatch");
			}
			if (!effectConfig->atlasProperties.empty()
				&& effectConfig->atlasProperties.size()
					!= effectConfig->atlasTextures.size())
			{
				return RejectA8Shape("atlas-page-property-size-mismatch");
			}
			for (const NiPoint2& inverseSize : effectConfig->atlasInverseSizes)
			{
				if (!std::isfinite(inverseSize.x) || !std::isfinite(inverseSize.y))
					return RejectA8Shape("non-finite-atlas-inverse-size");
			}
			for (const A8DrawRange& range : effectConfig->ranges)
			{
				if (range.layer > 3 || !range.vertexCount || !range.primitiveCount
					|| !IsFiniteColor(range.colorModifier))
				{
					return RejectA8Shape("invalid-draw-range");
				}
				if (!effectConfig->atlasTextures.empty()
					&& (range.atlasPage >= effectConfig->atlasTextures.size()
						|| !effectConfig->atlasTextures[range.atlasPage]))
				{
					return RejectA8Shape("invalid-atlas-page");
				}
				const UInt64 vertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 indexEnd = static_cast<UInt64>(range.startIndex)
					+ static_cast<UInt64>(range.primitiveCount) * 3;
				const UInt32 layerRank = GetA8LayerDrawRank(range.layer);
				if (vertexEnd > data->m_usVertices || indexEnd > availableIndices)
					return RejectA8Shape("draw-range-out-of-bounds");
				if (!firstRange && (layerRank < previousLayerRank
					|| range.firstVertex < previousVertexEnd
					|| range.startIndex < previousIndexEnd))
				{
					return RejectA8Shape("draw-ranges-not-global-and-monotonic");
				}
				for (UInt64 index = range.startIndex; index < indexEnd; ++index)
				{
					if (data->m_pusTriList[index] >= data->m_usVertices)
						return RejectA8Shape("triangle-index-out-of-bounds");
				}
				if (range.usesSdf && effectConfig->sdfSpreadPixels <= 0.0f)
					return RejectA8Shape("sdf-range-without-positive-spread");
				haveFill = haveFill || range.layer == 3;
				previousLayerRank = layerRank;
				previousVertexEnd = vertexEnd;
				previousIndexEnd = indexEnd;
				firstRange = false;
			}
			return haveFill || RejectA8Shape("missing-fill-range");
		}

		void CompileA8DrawRanges(A8ShapeMetadata& metadata)
		{
			metadata.compiledRanges.clear();
			metadata.compiledRanges.reserve(metadata.effects.ranges.size());
			for (const A8DrawRange& range : metadata.effects.ranges)
			{
				float inverseAtlasWidth = metadata.effects.inverseAtlasWidth;
				float inverseAtlasHeight = metadata.effects.inverseAtlasHeight;
				if (!metadata.effects.atlasInverseSizes.empty())
				{
					const NiPoint2& inverseSize =
						metadata.effects.atlasInverseSizes[range.atlasPage];
					inverseAtlasWidth = inverseSize.x;
					inverseAtlasHeight = inverseSize.y;
				}

				float parameter0 = metadata.effects.shadowBlurPixels;
				float parameter1 = metadata.effects.shadowPower;
				float parameter2 = 0.0f;
				if (range.layer == 1)
				{
					parameter0 = metadata.effects.glowInnerPixels;
					parameter1 = metadata.effects.glowOuterPixels;
					parameter2 = metadata.effects.glowPower;
				}
				else if (range.layer == 2)
				{
					parameter0 = metadata.effects.outlineWidthPixels;
					parameter1 = metadata.effects.outlineSoftnessPixels;
				}
				else if (range.layer == 3)
				{
					parameter0 = 0.0f;
					parameter1 = 0.0f;
				}

				A8CompiledRange compiled;
				compiled.range = range;
				compiled.constants = {{
					range.colorModifier.r, range.colorModifier.g,
					range.colorModifier.b, range.colorModifier.a,
					inverseAtlasWidth, inverseAtlasHeight,
					static_cast<float>(range.layer), metadata.effects.sdfSpreadPixels,
					parameter0, parameter1, parameter2, 0.0f,
					range.usesSdf ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
				}};
				if (metadata.effects.useOriginalShader)
					compiled.shaderClass = A8CompiledShaderClass::Original;
				else if (!range.usesSdf)
					compiled.shaderClass = A8CompiledShaderClass::Coverage;
				else if (metadata.effects.shaderEffects && range.layer != 3)
					compiled.shaderClass = A8CompiledShaderClass::Effect;
				else
					compiled.shaderClass = A8CompiledShaderClass::Body;
				compiled.staticSmoothSampling = range.layer == 1 || range.layer == 2
					|| (range.layer == 0
						&& metadata.effects.shadowBlurPixels > 0.001f)
					|| range.usesSdf;
				metadata.compiledRanges.push_back(std::move(compiled));
			}
		}

		void TrimPacketTemplateCache(A8State& state)
		{
			const size_t limit = static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
				* 1024u * 1024u / 12u;
			while (state.packetTemplateCacheBytes > limit
				&& !state.packetTemplateLru.empty())
			{
				const NativeA8TemplateCacheKey key = state.packetTemplateLru.back();
				state.packetTemplateLru.pop_back();
				const auto existing = state.packetTemplateCache.find(key);
				if (existing == state.packetTemplateCache.end())
					continue;
				state.packetTemplateCacheBytes -= existing->second.bytes;
				state.packetTemplateCache.erase(existing);
			}
		}

		NativeA8PayloadTemplatePtr GetNativeA8PayloadTemplate(
			NiTriShape* shape, const A8ShapeMetadata& metadata,
			UInt64 contentHash, const NiPoint3& geometryOrigin)
		{
			if (!contentHash)
				return BuildNativeA8PayloadTemplate(shape, metadata, geometryOrigin);
			const NativeA8TemplateCacheKey key = {
				contentHash,
				metadata.quadCount,
				static_cast<UInt32>(metadata.effects.atlasProperties.size())
			};
			A8State& state = State();
			{
				std::lock_guard<std::mutex> lock(state.packetTemplateMutex);
				const auto existing = state.packetTemplateCache.find(key);
				if (existing != state.packetTemplateCache.end())
				{
					state.packetTemplateLru.splice(state.packetTemplateLru.begin(),
						state.packetTemplateLru, existing->second.lru);
					existing->second.lru = state.packetTemplateLru.begin();
					RecordFreeTypePerf(FreeTypePerfCounter::PacketTemplateHit);
					return existing->second.data;
				}
			}
			RecordFreeTypePerf(FreeTypePerfCounter::PacketTemplateMiss);
			NativeA8PayloadTemplatePtr result =
				BuildNativeA8PayloadTemplate(shape, metadata, geometryOrigin);
			if (!result)
				return {};
			const size_t bytes = GetNativeA8PayloadTemplateBytes(*result);
			{
				std::lock_guard<std::mutex> lock(state.packetTemplateMutex);
				const auto existing = state.packetTemplateCache.find(key);
				if (existing != state.packetTemplateCache.end())
				{
					state.packetTemplateLru.splice(state.packetTemplateLru.begin(),
						state.packetTemplateLru, existing->second.lru);
					existing->second.lru = state.packetTemplateLru.begin();
					return existing->second.data;
				}
				state.packetTemplateLru.push_front(key);
				state.packetTemplateCache.emplace(key, NativeA8TemplateCacheEntry{
					result, bytes, state.packetTemplateLru.begin() });
				state.packetTemplateCacheBytes += bytes;
				TrimPacketTemplateCache(state);
			}
			return result;
		}
	}

	A8State& State()
	{
		return s_a8State;
	}

	bool NeedsScaledFillSampling(const NiTriShape* shape)
	{
		if (!shape)
			return false;
		const float worldScale = std::abs(shape->m_kWorld.m_fScale);
		return std::isfinite(worldScale) && std::abs(worldScale - 1.0f) > 0.001f;
	}

	void FinalizeA8RendererDetection()
	{
		const bool accumulatorReady = HookNativeA8Accumulator();
		const bool tileRouteReady = HookTileRenderPass();
		const bool shaderReady = InitializeNativeA8Renderer(true, true);
		gLog.FormattedMessage(
			"tnvse_freetype_native: initialization nativeReady=%u accumulator=%u tileRoute=%u shader=%u",
			accumulatorReady && tileRouteReady && shaderReady ? 1 : 0,
			accumulatorReady ? 1 : 0, tileRouteReady ? 1 : 0,
			shaderReady ? 1 : 0);
	}

	void HandleA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		HandleNativeA8RendererMainLoop();
		HookNativeA8Accumulator();
		HookTileRenderPass();
	}

	void HandleA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType == kShaderRefreshMessage)
			HandleNativeA8ShaderLoaderMessage(messageType);
	}

	bool IsA8RendererAvailable()
	{
		return g_bEnableFreeTypeFontRendering
			&& HookNativeA8Accumulator() && HookTileRenderPass()
			&& InitializeNativeA8Renderer(false, false);
	}

	bool ResolveA8EffectQuality(EffectQuality requested, EffectQuality& resolved)
	{
		if (static_cast<UInt32>(requested) > static_cast<UInt32>(EffectQuality::High)
			|| !IsA8RendererAvailable())
		{
			return false;
		}
		resolved = requested;
		return true;
	}

	bool PrepareA8AtlasShape(Font& font, NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount, const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract, UInt64 packetTemplateHash,
		const NiPoint3& geometryOrigin)
	{
		if (!IsA8RendererAvailable()
			|| !ValidateA8Shape(shape, effectConfig, colorContract)
			|| !InitializeA8TriShapeVtable(shape))
		{
			return false;
		}
		auto metadata = std::make_shared<A8ShapeMetadata>();
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		if (NiTriShapeData* data = shape->GetModelData())
		{
			metadata->vertexCount = data->m_usVertices;
			metadata->primitiveCount = data->m_usTriangles;
			metadata->indexCount = data->m_uiTriListLength
				? data->m_uiTriListLength : data->m_usTriangles * 3;
		}
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (effectConfig)
			metadata->effects = *effectConfig;
		CompileA8DrawRanges(*metadata);
		const NativeA8PayloadTemplatePtr payloadTemplate = GetNativeA8PayloadTemplate(
			shape, *metadata, packetTemplateHash, geometryOrigin);
		metadata->nativePayload = payloadTemplate
			? InstantiateNativeA8ShapePayload(font, shape, *metadata,
				payloadTemplate, geometryOrigin)
			: NativeA8ShapePayloadPtr{};
		if (!metadata->nativePayload)
			return false;
		{
			std::lock_guard<std::mutex> lock(State().metadataMutex);
			State().shapeMetadata[shape] = std::move(metadata);
		}
		*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		return true;
	}
}

namespace fonthook
{
	void FinalizeFreeTypeA8Detection()
	{
		vectorfont::FinalizeA8RendererDetection();
	}

	void HandleFreeTypeA8MainLoop()
	{
		vectorfont::HandleA8RendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::HandleA8ShaderLoaderMessage(messageType);
	}
}
