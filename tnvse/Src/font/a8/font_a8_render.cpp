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

		bool IsFiniteBound(const NiBound& bound)
		{
			return std::isfinite(bound.m_kCenter.x)
				&& std::isfinite(bound.m_kCenter.y)
				&& std::isfinite(bound.m_kCenter.z)
				&& std::isfinite(bound.m_fRadius) && bound.m_fRadius >= 0.0f;
		}

		bool IsFiniteVertex(const NativeA8GpuVertex& vertex)
		{
			return std::isfinite(vertex.x) && std::isfinite(vertex.y)
				&& std::isfinite(vertex.z) && std::isfinite(vertex.u)
				&& std::isfinite(vertex.v)
				// Coverage and precomposed ARGB profiles do not carry a distance
				// field, so zero is their canonical spread. Profile-specific
				// validation below still requires a positive spread for MTSDF.
				&& std::isfinite(vertex.sdfSpread) && vertex.sdfSpread >= 0.0f
				&& std::isfinite(vertex.distanceParameterScale)
				&& vertex.distanceParameterScale >= 1.0f
				&& std::isfinite(vertex.layerMask)
				&& vertex.layerMask >= 1.0f && vertex.layerMask <= 15.0f;
		}

		bool RejectA8Shape(const char* reason)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& State().shapeValidationFailureLogCount++
					< kMaximumShapeValidationFailureLogs)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag: rejected shape contract=glyph-params-packet-layer-rgb-v12 reason=%s",
					reason ? reason : "unknown");
			}
			return false;
		}

		bool ValidateA8Shape(NiTriShape* shape,
			const A8EffectShapeConfig* effectConfig,
			const A8ShapeColorContract* colorContract,
			const NativeA8PayloadTemplate* payloadTemplate)
		{
			if (!shape || !colorContract || !payloadTemplate)
				return RejectA8Shape("missing-shape-color-or-text-artifact");
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
			if (!data->m_pkColor)
				return RejectA8Shape("missing-base-vertex-color-stream");
			for (UInt32 index = 0; index < data->m_usVertices; ++index)
			{
				if (!IsFiniteColor(data->m_pkColor[index]))
					return RejectA8Shape("non-finite-base-vertex-color");
			}

			if (!payloadTemplate->quadCount
				|| payloadTemplate->quadCount > kNativeA8MaximumQuads
				|| payloadTemplate->gpuVertices.size()
					< static_cast<size_t>(payloadTemplate->quadCount) * 4u
				|| (payloadTemplate->gpuVertices.size() & 3u)
				|| payloadTemplate->gpuVertices.size() / 4u
					> kNativeA8MaximumQuads
				|| payloadTemplate->packets.empty()
				|| payloadTemplate->pageCount != payloadTemplate->atlasProperties.size()
				|| payloadTemplate->pageCount != payloadTemplate->atlasTextures.size()
				|| !IsFiniteBound(payloadTemplate->bound))
			{
				return RejectA8Shape("invalid-text-artifact");
			}
			if (!std::all_of(payloadTemplate->gpuVertices.begin(),
				payloadTemplate->gpuVertices.end(), IsFiniteVertex))
			{
				return RejectA8Shape("non-finite-text-artifact-vertex");
			}
			for (const NativeA8PacketTemplate& packet : payloadTemplate->packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
					+ packet.vertexCount;
				if (!packet.vertexCount || (packet.firstVertex & 3u)
					|| (packet.vertexCount & 3u)
					|| vertexEnd > payloadTemplate->gpuVertices.size()
					|| packet.layer > 3 || !IsFiniteBound(packet.bound)
					|| !std::all_of(packet.constants.begin(), packet.constants.end(),
						[](float value) { return std::isfinite(value); }))
				{
					return RejectA8Shape("invalid-text-artifact-packet");
				}
			}
			for (const NativeA8PacketTemplate& packet
				: payloadTemplate->compositePackets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
					+ packet.vertexCount;
				if (!packet.vertexCount || (packet.firstVertex & 3u)
					|| (packet.vertexCount & 3u)
					|| vertexEnd > payloadTemplate->gpuVertices.size()
					|| packet.layer > 3 || !IsFiniteBound(packet.bound)
					|| packet.shaderClass != NativeA8ShaderClass::Composite
					|| !std::all_of(packet.constants.begin(), packet.constants.end(),
						[](float value) { return std::isfinite(value); }))
				{
					return RejectA8Shape("invalid-composite-packet");
				}
			}
			if (!effectConfig || !effectConfig->enabled)
				return RejectA8Shape("native-route-requires-enabled-profile");
			const UInt32 profileKinds =
				(effectConfig->shaderEffects ? 1u : 0u)
				+ (effectConfig->bakedCoverage ? 1u : 0u)
				+ (effectConfig->precomposedArgb ? 1u : 0u);
			if (profileKinds != 1u)
				return RejectA8Shape("native-route-profile-kind-conflict");
			const bool distanceFieldProfile = effectConfig->shaderEffects;

			const std::array<float, 12> scalarValues = {
				effectConfig->inverseAtlasWidth,
				effectConfig->inverseAtlasHeight,
				effectConfig->sdfSpreadPixels,
				effectConfig->shadowBlurPixels,
				effectConfig->shadowPower,
				effectConfig->shadowGlowAlpha,
				effectConfig->shadowOutlineAlpha,
				effectConfig->glowInnerPixels,
				effectConfig->glowOuterPixels,
				effectConfig->glowPower,
				effectConfig->outlineWidthPixels,
				effectConfig->outlineSoftnessPixels
			};
			if (!std::all_of(scalarValues.begin(), scalarValues.end(),
				[](float value) { return std::isfinite(value); }))
			{
				return RejectA8Shape("non-finite-effect-configuration");
			}
			if (effectConfig->shadowGlowAlpha < 0.0f
				|| effectConfig->shadowGlowAlpha > 1.0f
				|| effectConfig->shadowOutlineAlpha < 0.0f
				|| effectConfig->shadowOutlineAlpha > 1.0f)
			{
				return RejectA8Shape("invalid-shadow-component-alpha");
			}
			if (static_cast<UInt32>(effectConfig->quality)
				> static_cast<UInt32>(EffectQuality::High))
			{
				return RejectA8Shape("invalid-effect-quality");
			}

			const UInt64 availableVertices = payloadTemplate->gpuVertices.size();
			const UInt64 availableIndices =
				static_cast<UInt64>(payloadTemplate->quadCount) * 6u;
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
			if (effectConfig->atlasProperties.empty()
				|| payloadTemplate->pageCount != effectConfig->atlasTextures.size())
			{
				return RejectA8Shape("text-artifact-page-count-mismatch");
			}
			for (const NativeA8PacketTemplate& packet : payloadTemplate->packets)
			{
				if (packet.atlasPage >= effectConfig->atlasTextures.size())
					return RejectA8Shape("text-artifact-packet-page-out-of-bounds");
			}
			for (const NativeA8PacketTemplate& packet
				: payloadTemplate->compositePackets)
			{
				if (packet.atlasPage >= effectConfig->atlasTextures.size())
					return RejectA8Shape(
						"composite-packet-page-out-of-bounds");
			}
			for (const NiPoint2& inverseSize : effectConfig->atlasInverseSizes)
			{
				if (!std::isfinite(inverseSize.x) || !std::isfinite(inverseSize.y))
					return RejectA8Shape("non-finite-atlas-inverse-size");
			}
			for (const A8DrawRange& range : effectConfig->ranges)
			{
				if (range.layer > 3 || !range.vertexCount || !range.primitiveCount
					|| (range.firstVertex & 3u) || (range.vertexCount & 3u)
					|| (range.startIndex % 6u) || (range.primitiveCount & 1u)
					|| range.vertexCount / 4u != range.primitiveCount / 2u
					|| !IsFiniteColor(range.layerColorModifier)
					|| !std::isfinite(range.sdfSpreadPixels)
					|| (distanceFieldProfile
						&& range.sdfSpreadPixels <= 0.0f)
					|| (!distanceFieldProfile
						&& range.sdfSpreadPixels < 0.0f)
					|| !std::isfinite(range.sourceToLogicalScale)
					|| range.sourceToLogicalScale <= 0.0f
					|| range.sourceToLogicalScale > 1.0f)
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
				if (vertexEnd > availableVertices || indexEnd > availableIndices)
					return RejectA8Shape("draw-range-out-of-bounds");
				if (!firstRange && (layerRank < previousLayerRank
					|| (layerRank == previousLayerRank
						&& (range.firstVertex < previousVertexEnd
							|| range.startIndex < previousIndexEnd))))
				{
					return RejectA8Shape("draw-ranges-not-layer-monotonic");
				}
				if (distanceFieldProfile != range.usesSdf)
					return RejectA8Shape(distanceFieldProfile
						? "distance-field-range-without-sdf"
						: effectConfig->precomposedArgb
							? "argb-range-marked-as-sdf"
							: "coverage-range-marked-as-sdf");
				if (distanceFieldProfile
					&& range.sdfSpreadPixels <= 0.0f)
				{
					return RejectA8Shape(
						"distance-field-range-without-positive-spread");
				}
				haveFill = haveFill || range.layer == 3;
				previousLayerRank = layerRank;
				previousVertexEnd = vertexEnd;
				previousIndexEnd = indexEnd;
				firstRange = false;
			}
			return haveFill || RejectA8Shape("missing-fill-range");
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
		const bool nativeReady =
			accumulatorReady && tileRouteReady && shaderReady;
		SynchronizePersistentFontCacheRoute(ResolveFontAtlasRoute(
			nativeReady, g_bEnableFreeTypeFontAggressivePerformanceMode));
		gLog.FormattedMessage(
			"tnvse_freetype_native: initialization nativeReady=%u accumulator=%u sortedUpload=%u tileRoute=%u shader=%u",
			nativeReady ? 1 : 0,
			accumulatorReady ? 1 : 0,
			State().sortedTileRenderHookInstalled ? 1 : 0,
			tileRouteReady ? 1 : 0,
			shaderReady ? 1 : 0);
	}

	void HandleA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		HandleNativeA8RendererMainLoop();
		HookNativeA8Accumulator();
		HookTileRenderPass();
		EnforceCpuMemoryBudget("main-loop");
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
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		FreeTypePerfScope perf(
			FreeTypePerfPhase::NativeRegistration);
		if (!IsA8RendererAvailable()
			|| !payloadTemplate || payloadTemplate->quadCount != quadCount
			|| !ValidateA8Shape(shape, effectConfig, colorContract,
				payloadTemplate.get())
			|| !InitializeA8TriShapeVtable(shape))
		{
			return false;
		}
		auto metadata = std::make_shared<A8ShapeMetadata>();
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = static_cast<UInt32>(
			payloadTemplate->gpuVertices.size());
		metadata->primitiveCount = payloadTemplate->quadCount * 2u;
		metadata->indexCount = payloadTemplate->quadCount * 6u;
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (!InitializeNativeA8ShapePayload(font, shape, *metadata,
			std::move(payloadTemplate), geometryOrigin, metadata->nativePayload))
			return false;
		metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			sizeof(A8ShapeMetadata)
				+ metadata->nativePayload.packetShaders.capacity()
					* sizeof(TileShader*)
				+ metadata->nativePayload.preflightAtlasTextures.capacity()
					* sizeof(const void*)
				+ sizeof(A8ShapeMetadataPtr) + 6u * sizeof(void*));
		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			state.shapeMetadata[shape] = std::move(metadata);
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
