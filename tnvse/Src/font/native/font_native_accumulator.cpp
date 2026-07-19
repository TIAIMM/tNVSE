#include "font_a8_internal.h"
#include "font_native_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kBSShaderAccumulatorVtable = 0x10ADFF8;
		inline constexpr UInt32 kRegisterObjectVtableSlot = 38;
		inline constexpr UInt32 kRegisterObjectVtableEntry =
			kBSShaderAccumulatorVtable + kRegisterObjectVtableSlot * sizeof(void*);
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;

		using RegisterObjectFn = bool(__thiscall*)(BSShaderAccumulator*, NiGeometry*);

		RegisterObjectFn s_originalRegisterObject = nullptr;
		bool s_hookAttempted = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;

		struct SortedPayloadScratch
		{
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			CpuMemoryLease cpuMemory;
		};

		thread_local SortedPayloadScratch s_sortedPayloadScratch;

		SortedTileRenderFn ReadSortedTileRenderCallTarget()
		{
			const UInt8* call = reinterpret_cast<const UInt8*>(
				kSortedTileRenderCallSite);
			if (!call || call[0] != 0xE8)
				return nullptr;
			SInt32 displacement = 0;
			std::memcpy(&displacement, call + 1, sizeof(displacement));
			return reinterpret_cast<SortedTileRenderFn>(
				kSortedTileRenderCallSite + 5 + displacement);
		}

		bool IsFreeTypeFacade(const NiGeometry* geometry)
		{
			if (!geometry || !State().originalTriShapeVtable)
				return false;
			void* const* vtable = *reinterpret_cast<void* const* const*>(geometry);
			return vtable == &State().triShapeVtable[1];
		}

		void ClearNativePacketFailure(NativeA8ShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		}

		void InvalidateNativePreflight(NativeA8ShapePayload& payload)
		{
			payload.preparedGeneration = 0;
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
		}

		bool IsNativePreflightCacheCurrent(const NativeA8ShapePayload& payload,
			UInt32 generation,
			bool scaledFillSampling, bool alphaBlending)
		{
			if (!payload.payloadTemplate)
				return false;
			const NativeA8PayloadTemplate& artifact = *payload.payloadTemplate;
			if (payload.preparedGeneration != generation
				|| payload.preflightScaledFillSampling != scaledFillSampling
				|| payload.preflightAlphaBlending != alphaBlending
				|| payload.preflightAtlasTextures.size()
					!= artifact.atlasTextures.size()
				|| payload.packetShaders.size() != artifact.packets.size())
			{
				return false;
			}

			// Device reset changes the native generation. This page-level identity
			// check additionally catches an atlas wrapper being rebuilt or replaced
			// without paying the old per-packet validation and shader lookup cost.
			for (size_t page = 0; page < payload.preflightAtlasTextures.size(); ++page)
			{
				const void* expected = payload.preflightAtlasTextures[page];
				if (!expected)
					continue;
				NiTexture* texture = artifact.atlasTextures[page].m_pObject;
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				if (!textureData || textureData->GetD3DTexture() != expected)
					return false;
			}
			return true;
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
		{
			if (!facade || !payload.buildComplete || !payload.payloadTemplate
				|| payload.packetShaders.empty()
				|| payload.packetShaders.size()
					!= payload.payloadTemplate->packets.size())
			{
				return NativeA8FallbackReason::PacketBuild;
			}
			const NativeA8PayloadTemplate& artifact = *payload.payloadTemplate;
			if (!IsNativeA8AccumulatorHookCurrent())
				return NativeA8FallbackReason::AccumulatorConflict;
			if (!IsA8TileRenderPassHookCurrent())
				return NativeA8FallbackReason::TileRouteConflict;
			if (!IsNativeA8RendererAvailable())
				return NativeA8FallbackReason::ShaderGeneration;

			const UInt32 generation = GetNativeA8ShaderGeneration();
			if (!generation)
				return NativeA8FallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			if (IsNativePreflightCacheCurrent(payload, generation,
				scaledFillSampling, alphaBlending))
			{
				ClearNativePacketFailure(payload);
				return NativeA8FallbackReason::None;
			}

			InvalidateNativePreflight(payload);
			payload.preflightScaledFillSampling = scaledFillSampling;
			payload.preflightAlphaBlending = alphaBlending;
			if (payload.preflightAtlasTextures.size() != artifact.atlasTextures.size())
				return NativeA8FallbackReason::PacketBuild;
			for (const NativeA8PacketTemplate& packetTemplate : artifact.packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(
					packetTemplate.firstVertex) + packetTemplate.vertexCount;
				if (!packetTemplate.vertexCount
					|| (packetTemplate.vertexCount & 3u)
					|| vertexEnd > artifact.gpuVertices.size()
					|| packetTemplate.atlasPage >= artifact.atlasTextures.size())
				{
					return NativeA8FallbackReason::PacketBuild;
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
					return NativeA8FallbackReason::PageTexture;
				payload.preflightAtlasTextures[page] = d3dTexture;
			}

			for (size_t index = 0; index < artifact.packets.size(); ++index)
			{
				payload.packetShaders[index] = ResolveNativeA8PacketShader(
					artifact.packets[index],
					facade, scaledFillSampling);
				if (!payload.packetShaders[index])
					return NativeA8FallbackReason::ShaderGeneration;
			}

			payload.preparedGeneration = generation;
			ClearNativePacketFailure(payload);
			return NativeA8FallbackReason::None;
		}

		bool SuppressNativeGroup(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
			const char* phase)
		{
			RecordNativeA8Suppression(facade, metadata, reason, phase);
			// Match stock's accepted/skipped result while preventing a marked facade
			// from entering a renderer path that cannot interpret native packet data.
			return true;
		}

		bool __fastcall NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			void*, NiGeometry* geometry)
		{
			if (!s_originalRegisterObject || !accumulator || !geometry
				|| accumulator->eRenderMode != BSShaderManager::BSSM_RENDER_TILES
				|| !IsFreeTypeFacade(geometry))
			{
				return s_originalRegisterObject
					? s_originalRegisterObject(accumulator, geometry) : false;
			}

			NiTriShape* facade = static_cast<NiTriShape*>(geometry);
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(facade);
			if (!metadata || !metadata->nativePayload.buildComplete)
			{
				if (metadata)
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::PacketBuild, "register-object");
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					const UInt32 ordinal = s_missingMetadataLogCount.fetch_add(
						1, std::memory_order_relaxed);
					if (ordinal < kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=register-object shape=%p thread=%u",
							facade, GetCurrentThreadId());
					}
					else if (ordinal == kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: metadata-missing registration logs capped at %u entries",
							kMaximumMissingMetadataLogs);
					}
				}
				return true;
			}

			// Keep one facade in the stock Tile alpha list. Equal-depth entries are
			// quicksorted unstably, so individually registered packets cannot retain
			// Glow/Shadow/Outline/Fill order. Expand only after stock UI sorting.
			if (!IsA8TileRenderPassHookCurrent())
				return SuppressNativeGroup(facade, *metadata,
					NativeA8FallbackReason::TileRouteConflict, "register-object");
			return s_originalRegisterObject(accumulator, facade);
		}

		int __fastcall NativeA8RenderSorted(BSShaderAccumulator* accumulator, void*)
		{
			A8State& state = State();
			if (!state.originalSortedTileRender)
				return 0;

			if (accumulator
				&& accumulator->eRenderMode == BSShaderManager::BSSM_RENDER_TILES
				&& accumulator->m_iNumItems > 0 && accumulator->m_ppkItems)
			{
				std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates =
					s_sortedPayloadScratch.payloadTemplates;
				payloadTemplates.clear();
				if (payloadTemplates.capacity()
					< static_cast<size_t>(accumulator->m_iNumItems))
				{
					payloadTemplates.reserve(static_cast<size_t>(
						accumulator->m_iNumItems));
				}
				UInt32 generation = GetNativeA8ShaderGeneration();
				for (SInt32 index = accumulator->m_iNumItems - 1;
					index >= 0; --index)
				{
					NiGeometry* geometry = accumulator->m_ppkItems[index];
					if (!IsFreeTypeFacade(geometry))
						continue;
					NiTriShape* facade = static_cast<NiTriShape*>(geometry);
					const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(facade);
					if (!metadata || !metadata->nativePayload.buildComplete)
						continue;
					NativeA8ShapePayload& payload = metadata->nativePayload;
					if (PreflightNativeGroupImpl(facade, *metadata, payload)
						!= NativeA8FallbackReason::None
						|| !payload.payloadTemplate
						|| payload.preparedGeneration != generation)
					{
						continue;
					}
					payloadTemplates.push_back(payload.payloadTemplate);
				}
				std::sort(payloadTemplates.begin(), payloadTemplates.end(),
					[](const NativeA8PayloadTemplatePtr& left,
						const NativeA8PayloadTemplatePtr& right)
					{
						return std::less<const NativeA8PayloadTemplate*>{}(
							left.get(), right.get());
					});
				payloadTemplates.erase(std::unique(payloadTemplates.begin(),
					payloadTemplates.end(),
					[](const NativeA8PayloadTemplatePtr& left,
						const NativeA8PayloadTemplatePtr& right)
					{
						return left.get() == right.get();
					}), payloadTemplates.end());
				PrepareSortedNativeA8Payloads(payloadTemplates, generation);
				payloadTemplates.clear();
				if (payloadTemplates.capacity() > 8192)
					std::vector<NativeA8PayloadTemplatePtr>().swap(payloadTemplates);
				s_sortedPayloadScratch.cpuMemory.Reset(
					CpuMemoryCategory::RuntimeMetadata,
					payloadTemplates.capacity()
						* sizeof(NativeA8PayloadTemplatePtr));
				if (IsCpuMemoryBudgetExceeded())
				{
					std::vector<NativeA8PayloadTemplatePtr>().swap(payloadTemplates);
					s_sortedPayloadScratch.cpuMemory.Release();
				}
			}

			return state.originalSortedTileRender(accumulator);
		}

		bool HookSortedTileRender()
		{
			A8State& state = State();
			const SortedTileRenderFn hook = reinterpret_cast<SortedTileRenderFn>(
				&NativeA8RenderSorted);
			const SortedTileRenderFn current = ReadSortedTileRenderCallTarget();
			if (current == hook)
			{
				state.sortedTileRenderHookInstalled =
					state.originalSortedTileRender != nullptr;
				return state.sortedTileRenderHookInstalled;
			}
			if (!current)
			{
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile render call site is not CALL rel32; frame upload batching disabled");
				}
				return false;
			}
			if (state.sortedTileRenderHookInstalled)
			{
				state.sortedTileRenderHookInstalled = false;
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile frame upload hook was replaced; per-shape upload fallback remains active");
				}
				return false;
			}
			if (reinterpret_cast<UInt32>(current) != kStockSortedTileRender)
			{
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile render call site already has a non-stock target=%p; frame upload batching left disabled",
						current);
				}
				return false;
			}

			state.originalSortedTileRender = current;
			WriteRelCall(kSortedTileRenderCallSite, hook);
			state.sortedTileRenderHookInstalled =
				ReadSortedTileRenderCallTarget() == hook;
			if (!state.sortedTileRenderHookInstalled)
			{
				state.originalSortedTileRender = nullptr;
				return false;
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed sorted Tile frame upload route original=%p stock=1",
					current);
			}
			return true;
		}
	}

	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
	{
		return PreflightNativeGroupImpl(facade, metadata, payload);
	}

	bool HookNativeA8Accumulator()
	{
		void* current = *reinterpret_cast<void**>(kRegisterObjectVtableEntry);
		if (current == reinterpret_cast<void*>(&NativeA8RegisterObject))
		{
			HookSortedTileRender();
			return s_originalRegisterObject != nullptr;
		}
		if (s_hookAttempted)
			return false;
		s_hookAttempted = true;
		if (!current)
			return false;
		s_originalRegisterObject = reinterpret_cast<RegisterObjectFn>(current);
		SafeWrite32(kRegisterObjectVtableEntry,
			reinterpret_cast<UInt32>(&NativeA8RegisterObject));
		const bool accumulatorReady = IsNativeA8AccumulatorHookCurrent();
		if (accumulatorReady)
			HookSortedTileRender();
		return accumulatorReady;
	}

	bool IsNativeA8AccumulatorHookCurrent()
	{
		return *reinterpret_cast<void**>(kRegisterObjectVtableEntry)
			== reinterpret_cast<void*>(&NativeA8RegisterObject)
			&& s_originalRegisterObject != nullptr;
	}
}
