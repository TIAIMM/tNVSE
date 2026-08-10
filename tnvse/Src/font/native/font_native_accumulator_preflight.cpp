#include "font_native_accumulator_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_accumulator {}
	using namespace implementation::font_native_accumulator;

	namespace implementation::font_native_accumulator
	{
		void ClearNativePacketFailure(NativeFontShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
		}

		void InvalidateNativePreflight(NativeFontShapePayload& payload)
		{
			payload.preparedGeneration = 0;
			payload.preflightAtlasTextureEpoch = 0;
			payload.vanillaLikeBitmapPackets = false;
			// Full preflight often refreshes only atlas/resource stamps. Keep
			// the Tile/program dispatch until retained rebuild can compare its
			// geometry, program, and generation identities.
			InvalidateNativeFontTileRetainedText(payload, true);
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
			std::fill(payload.packetPrograms.begin(),
				payload.packetPrograms.end(), nullptr);
		}

		bool IsNativePreflightCacheCurrent(const NativeFontShapePayload& payload,
			UInt32 generation,
			UInt32 atlasTextureEpoch, bool scaledFillSampling,
			bool alphaBlending, const bool* forcedCompositeTopology)
		{
			if (!payload.payloadTemplate)
				return false;
			const NativeFontPayloadTemplate& artifact = *payload.payloadTemplate;
			const bool compositeDesired = forcedCompositeTopology
				? *forcedCompositeTopology
				: g_bEnableFreeTypeFontCompositePass
					&& !artifact.compositePackets.empty()
					&& !(payload.compositeUnavailable
						&& payload.compositeAttemptGeneration == generation);
			const std::vector<NativeFontPacketTemplate>& packets =
				GetNativeFontPackets(artifact, payload.useCompositePackets);
			if (payload.preparedGeneration != generation
				|| payload.preflightAtlasTextureEpoch != atlasTextureEpoch
				|| payload.preflightScaledFillSampling != scaledFillSampling
				|| payload.preflightAlphaBlending != alphaBlending
				|| payload.useCompositePackets != compositeDesired
				|| payload.preflightAtlasTextures.size()
					!= artifact.atlasTextures.size()
				|| payload.packetShaders.size() != packets.size()
				|| payload.packetPrograms.size() != packets.size())
			{
				return false;
			}
			return true;
		}

		NativeFontFallbackReason PreflightNativeFacadeImpl(NiTriShape* facade,
			const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload,
			const NativePreflightFrameContext* frameContext,
			const bool* forcedCompositeTopology)
		{
			if (!facade || !payload.buildComplete || !payload.payloadTemplate
				|| payload.payloadTemplate->packets.empty())
			{
				return NativeFontFallbackReason::PacketBuild;
			}
			const NativeFontPayloadTemplate& artifact = *payload.payloadTemplate;
			NativePreflightFrameContext structuralContext;
			const NativePreflightFrameContext* currentContext = frameContext;
			if (!currentContext)
			{
				NativeFontRuntimeReadinessView readiness;
				const bool ready =
					GetNativeFontRuntimeReadinessCurrent(readiness);
				structuralContext.accumulatorCurrent = ready;
				structuralContext.immediateRouteCurrent = ready;
				structuralContext.rendererAvailable = ready;
				structuralContext.generation = readiness.generation;
				structuralContext.atlasTextureEpoch =
					readiness.atlasTextureEpoch;
				currentContext = &structuralContext;
			}
			if (!currentContext->accumulatorCurrent)
				return NativeFontFallbackReason::AccumulatorConflict;
			if (!currentContext->immediateRouteCurrent)
				return NativeFontFallbackReason::TileRouteConflict;
			if (!currentContext->rendererAvailable)
				return NativeFontFallbackReason::ShaderGeneration;

			const UInt32 generation = currentContext->generation;
			if (!generation)
				return NativeFontFallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			const UInt32 atlasTextureEpoch = currentContext->atlasTextureEpoch;
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& artifact.compositePackets.empty())
			{
				return NativeFontFallbackReason::PacketBuild;
			}
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& payload.compositeUnavailable
				&& payload.compositeAttemptGeneration == generation)
			{
				return NativeFontFallbackReason::ShaderGeneration;
			}
			if (IsNativePreflightCacheCurrent(payload, generation,
					atlasTextureEpoch, scaledFillSampling, alphaBlending,
					forcedCompositeTopology))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::PreflightFastHit);
				ClearNativePacketFailure(payload);
				return NativeFontFallbackReason::None;
			}

			RecordFreeTypePerf(FreeTypePerfCounter::PreflightFullValidation);
			InvalidateNativePreflight(payload);
			payload.preflightScaledFillSampling = scaledFillSampling;
			payload.preflightAlphaBlending = alphaBlending;
			const bool attemptComposite = forcedCompositeTopology
				? *forcedCompositeTopology
				: g_bEnableFreeTypeFontCompositePass
					&& !artifact.compositePackets.empty()
					&& !(payload.compositeUnavailable
						&& payload.compositeAttemptGeneration == generation);
			payload.useCompositePackets = attemptComposite;
			const std::vector<NativeFontPacketTemplate>* packets =
				&GetNativeFontPackets(artifact, payload.useCompositePackets);
			payload.vanillaLikeBitmapPackets =
				UsesOnlyVanillaLikeBitmapPackets(*packets);
			payload.packetShaders.assign(packets->size(), nullptr);
			payload.packetPrograms.assign(packets->size(), nullptr);
			if (payload.preflightAtlasTextures.size() != artifact.atlasTextures.size())
				return NativeFontFallbackReason::PacketBuild;
			for (const NativeFontPacketTemplate& packetTemplate : *packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(
					packetTemplate.firstVertex) + packetTemplate.vertexCount;
				if (!packetTemplate.vertexCount
					|| (packetTemplate.vertexCount & 3u)
					|| vertexEnd > artifact.gpuVertices.size()
					|| packetTemplate.atlasPage >= artifact.atlasTextures.size())
				{
					return NativeFontFallbackReason::PacketBuild;
				}
				const size_t page = packetTemplate.atlasPage;
				if (payload.preflightAtlasTextures[page])
					continue;
				NiTexture* texture = artifact.atlasTextures[page].m_pObject;
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				const void* d3dTexture = textureData
					? textureData->GetD3DTexture() : nullptr;
				if (!d3dTexture)
					return NativeFontFallbackReason::PageTexture;
				payload.preflightAtlasTextures[page] = d3dTexture;
			}

			bool shaderSetReady = true;
			for (size_t index = 0; index < packets->size(); ++index)
			{
				payload.packetShaders[index] = ResolveNativeFontPacketShader(
					(*packets)[index],
					facade, scaledFillSampling);
				if (!payload.packetShaders[index])
				{
					shaderSetReady = false;
					break;
				}
			}
			if (!shaderSetReady && attemptComposite
				&& !forcedCompositeTopology)
			{
				// Composite shaders are optional generation members.  Reject only
				// this optimization for the current generation and immediately
				// resolve the ordinary quality-equivalent packet set.
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeShaderFallback);
				payload.useCompositePackets = false;
				packets = &artifact.packets;
				payload.vanillaLikeBitmapPackets =
					UsesOnlyVanillaLikeBitmapPackets(*packets);
				payload.packetShaders.assign(packets->size(), nullptr);
				payload.packetPrograms.assign(packets->size(), nullptr);
				shaderSetReady = true;
				for (size_t index = 0; index < packets->size(); ++index)
				{
					payload.packetShaders[index] = ResolveNativeFontPacketShader(
						(*packets)[index], facade, scaledFillSampling);
					if (!payload.packetShaders[index])
					{
						shaderSetReady = false;
						break;
					}
				}
			}
			else if (!shaderSetReady && attemptComposite)
			{
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = true;
			}
			if (!shaderSetReady)
				return NativeFontFallbackReason::ShaderGeneration;
			if (g_bEnableFreeTypeFontCommandBuffer)
			{
				for (size_t index = 0; index < packets->size(); ++index)
				{
					ResolveNativeFontRetainedPacketProgram(
						(*packets)[index],
						payload.packetShaders[index], generation,
						payload.packetPrograms[index]);
				}
			}
			if (attemptComposite && payload.useCompositePackets)
			{
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = false;
			}
			if (GetNativeFontAtlasTextureEpoch() != atlasTextureEpoch)
			{
				InvalidateNativePreflight(payload);
				return NativeFontFallbackReason::AtlasGeneration;
			}
			if (payload.topologyObserved
				&& payload.lastTopologyComposite
					!= payload.useCompositePackets)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeTopologySwitch);
			}
			payload.topologyObserved = true;
			payload.lastTopologyComposite = payload.useCompositePackets;

			payload.preparedGeneration = generation;
			payload.preflightAtlasTextureEpoch = atlasTextureEpoch;
			if (g_bEnableFreeTypeFontCommandBuffer)
			{
				BuildNativeFontTileRetainedText(facade, payload,
					generation, atlasTextureEpoch);
			}
			ClearNativePacketFailure(payload);
			return NativeFontFallbackReason::None;
		}

	}

	NativeFontFallbackReason PrepareNativeFontFacade(NiTriShape* facade,
		const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload)
	{
		return PreflightNativeFacadeImpl(facade, metadata, payload);
	}

}
