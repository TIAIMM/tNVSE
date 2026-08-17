#include "font_native_ring_detail.h"

#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	using namespace implementation::font_native_ring;

	namespace implementation::font_native_ring
	{
		UInt64 HashDiagnosticBytes(const void* data, size_t size)
		{
			if (!data || !size)
				return 0;
			const UInt8* bytes = static_cast<const UInt8*>(data);
			UInt64 value = 14695981039346656037ull;
			for (size_t index = 0; index < size; ++index)
			{
				value ^= bytes[index];
				value *= 1099511628211ull;
			}
			return value;
		}

		UInt64 HashDiagnosticPayload(
			const NativeFontPayloadTemplate& payloadTemplate)
		{
			if (!g_bEnableFreeTypeFontRenderingLog
				|| payloadTemplate.gpuVertices.empty())
			{
				return 0;
			}
			return HashDiagnosticBytes(payloadTemplate.gpuVertices.data(),
				payloadTemplate.gpuVertices.size()
					* sizeof(NativeFontGpuVertex));
		}

		UInt32 AdvanceDiagnosticSerial(UInt32& serial)
		{
			if (++serial == 0)
				++serial;
			return serial;
		}
		void RefreshRingCpuMemoryLocked(NativeFontRingState& state)
		{
			size_t bytes = EstimateUnorderedMapBytes(state.uploadedPayloads)
				+ EstimateUnorderedMapBytes(state.staticPayloads)
				+ EstimateUnorderedMapBytes(state.staticCandidates);
			state.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		std::shared_ptr<NativeFontStaticCandidate> CreateStaticCandidate(
			const NativeFontPayloadTemplatePtr& payloadTemplate)
		{
			auto candidate = std::make_shared<NativeFontStaticCandidate>();
			candidate->owner = payloadTemplate;
			candidate->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				sizeof(NativeFontStaticCandidate) + 2u * sizeof(void*));
			return candidate;
		}

		TileShaderPropertyView* GetTileProperty(NiTriShape* shape)
		{
			NiShadeProperty* property = shape ? shape->GetShadeProperty() : nullptr;
			return property && property->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<TileShaderPropertyView*>(property) : nullptr;
		}

		const TileShaderPropertyView* GetTileProperty(const NiTriShape* shape)
		{
			return GetTileProperty(const_cast<NiTriShape*>(shape));
		}

		bool InstallProxyVertexColors(NiTriShapeData& data)
		{
			if (!data.m_usVertices || !data.m_pkTexture || data.m_pkBuffData)
				return false;
			if (data.m_pkColor)
				return true;
			NiColorA* colors = NiAlloc<NiColorA>(data.m_usVertices);
			if (!colors)
				return false;
			std::fill_n(colors, data.m_usVertices,
				NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f });
			data.m_pkColor = colors;
			return true;
		}

		bool AttachProxyBuffer(NativeFontProxy& proxy)
		{
			NiTriShapeData* data = proxy.shape
				? proxy.shape->GetModelData() : nullptr;
			if (!data || data->m_pkBuffData)
				return false;

			NiGeometryBufferData* buffer = NiNew<NiGeometryBufferData>();
			UInt32* stride = NiAlloc<UInt32>(1);
			void* chipMemory = NiAlloc(sizeof(NiVBChip*) + sizeof(NiVBChip));
			if (!buffer || !stride || !chipMemory)
			{
				if (buffer)
					NiDelete(buffer, sizeof(NiGeometryBufferData));
				if (stride)
					NiFree(stride);
				if (chipMemory)
					NiFree(chipMemory);
				return false;
			}

			ThisStdCall<void>(kNiGeometryBufferDataConstructor, buffer);
			std::memset(chipMemory, 0, sizeof(NiVBChip*) + sizeof(NiVBChip));
			auto** chips = static_cast<NiVBChip**>(chipMemory);
			NiVBChip* chip = reinterpret_cast<NiVBChip*>(
				static_cast<UInt8*>(chipMemory) + sizeof(NiVBChip*));
			chips[0] = chip;
			*stride = sizeof(NativeFontGpuVertex);

			buffer->m_uiStreamCount = 1;
			buffer->m_puiVertexStride = stride;
			buffer->m_ppkVBChip = chips;
			buffer->m_eType = D3DPT_TRIANGLELIST;
			// NiTriShape::RenderImmediate only enters its indexed draw loop when
			// m_uiNumArrays is nonzero. The shared canonical IB is one contiguous
			// triangle-list array; zero here silently binds every resource but emits
			// no DrawIndexedPrimitive call.
			buffer->m_uiNumArrays = kCanonicalArrayCount;
			buffer->m_pusArrayLengths = nullptr;
			buffer->m_pusIndexArray = nullptr;
			chip->m_uiIndex = 0;
			data->m_pkBuffData = buffer;
			proxy.buffer = buffer;
			proxy.chip = chip;
			return true;
		}

		void ClearProxyGpuBindings(NativeFontProxy& proxy)
		{
			proxy.shader = nullptr;
			if (proxy.chip)
				proxy.chip->m_pkVB = nullptr;
			if (!proxy.buffer)
				return;
			if (proxy.buffer->m_pkIB)
			{
				proxy.buffer->m_pkIB->Release();
				proxy.buffer->m_pkIB = nullptr;
			}
			if (proxy.buffer->m_hDeclaration)
			{
				auto* declaration = static_cast<IDirect3DVertexDeclaration9*>(
					proxy.buffer->m_hDeclaration);
				declaration->Release();
				proxy.buffer->m_hDeclaration = nullptr;
			}
			proxy.buffer->m_uiVertCount = 0;
			proxy.buffer->m_uiMaxVertCount = 0;
			proxy.buffer->m_uiIndexCount = 0;
			proxy.buffer->m_uiBaseVertexIndex = 0;
			proxy.buffer->m_uiTriCount = 0;
			proxy.buffer->m_uiMaxTriCount = 0;
		}

		UInt32 AdvanceResourceSerialLocked(NativeFontRingState& state)
		{
			UInt32 serial = state.resourceSerial.fetch_add(
				1, std::memory_order_release) + 1u;
			if (!serial)
			{
				serial = 1u;
				state.resourceSerial.store(serial, std::memory_order_release);
			}
			NotifyNativeFontCommandExternalMutation(
				NativeFontCommandFallback::Resource);
			return serial;
		}

		void AdvanceUploadEpochLocked(NativeFontRingState& state)
		{
			if (++state.uploadEpoch == 0)
				++state.uploadEpoch;
			NotifyNativeFontCommandExternalMutation(
				NativeFontCommandFallback::Resource);
		}

		void ReleaseRingResourcesLocked(NativeFontRingState& state)
		{
			// Singleton-facade descriptors borrow the ring's COM resources without
			// owning references. Restore every live shape to its vanilla shell before
			// any buffer or declaration can be released.
			InvalidateAllSingletonFacadeBindings();
			state.releasePending.store(false, std::memory_order_release);
			AdvanceUploadEpochLocked(state);
			state.uploadedPayloads.clear();
			state.staticPayloads.clear();
			state.staticCandidates.clear();
			RefreshRingCpuMemoryLocked(state);
			AdvanceResourceSerialLocked(state);
			RingThread().staticPayload = {};
			RingThread().uploadedPayload = {};
			RingThread().staticCandidate = {};
			for (UInt32 index = 0; index < state.proxyCount; ++index)
				ClearProxyGpuBindings(state.proxies[index]);
			if (state.indexBuffer)
				state.indexBuffer->Release();
			if (state.vertexBuffer)
				state.vertexBuffer->Release();
			if (state.staticVertexBuffer)
				state.staticVertexBuffer->Release();
			state.renderer = nullptr;
			state.device = nullptr;
			state.vertexBuffer = nullptr;
			state.staticVertexBuffer = nullptr;
			state.indexBuffer = nullptr;
			state.declaration = nullptr;
			state.generation = 0;
			state.vertexCapacity = 0;
			state.nextVertex = 0;
			state.staticVertexCapacity = 0;
			state.nextStaticVertex = 0;
			state.lastStaticCompactionFrame = 0;
			state.lastStaticCompactionDeferredLogFrame = 0;
			state.lastStaticCandidateSweepFrame = 0;
			state.staticPromotionBudgetFrame = 0;
			state.staticPromotionGlobalRetryFrame = 0;
			state.staticPromotionBytesThisFrame = 0;
			state.staticPromotionPayloadsThisFrame = 0;
			state.staticCompactionFrameValid = false;
			state.staticCompactionDeferredLogFrameValid = false;
			state.staticCandidateSweepFrameValid = false;
			state.staticPromotionBudgetFrameValid = false;
		}

		bool PopulateCanonicalIndexBuffer(IDirect3DIndexBuffer9* indexBuffer,
			HRESULT& result)
		{
			if (!indexBuffer)
			{
				result = E_POINTER;
				return false;
			}
			void* memory = nullptr;
			result = indexBuffer->Lock(0, kCanonicalIndexBytes,
				&memory, 0);
			if (FAILED(result) || !memory)
			{
				if (SUCCEEDED(result))
					result = E_FAIL;
				return false;
			}
			auto* indices = static_cast<UInt16*>(memory);
			for (UInt32 quad = 0; quad < kNativeFontMaximumQuads; ++quad)
			{
				const UInt16 base = static_cast<UInt16>(quad * 4u);
				const UInt32 output = quad * 6u;
				indices[output + 0] = static_cast<UInt16>(base + 0u);
				indices[output + 1] = static_cast<UInt16>(base + 2u);
				indices[output + 2] = static_cast<UInt16>(base + 1u);
				indices[output + 3] = static_cast<UInt16>(base + 0u);
				indices[output + 4] = static_cast<UInt16>(base + 3u);
				indices[output + 5] = static_cast<UInt16>(base + 2u);
			}
			result = indexBuffer->Unlock();
			return SUCCEEDED(result);
		}

		bool EnsureRingResourcesLocked(NativeFontRingState& state,
			UInt32 preparedGeneration, UInt32 requiredVertices,
			const char*& operation, HRESULT& result)
		{
			if (state.releasePending.load(std::memory_order_acquire))
			{
				if (state.sortedFrameLeases.load(std::memory_order_acquire)
					|| state.activeSubmissions.load(std::memory_order_acquire))
				{
					operation = "ring-busy";
					result = D3DERR_WASSTILLDRAWING;
					return false;
				}
				ReleaseRingResourcesLocked(state);
			}
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			const UInt32 generation = GetNativeFontShaderGeneration();
			if (!renderer || !device || !generation
				|| generation != preparedGeneration)
			{
				operation = "ring-context";
				result = D3DERR_DEVICELOST;
				return false;
			}
			IDirect3DVertexDeclaration9* declaration =
				GetNativeFontD3DDeclaration(generation);
			if (!declaration)
			{
				operation = "ring-declaration";
				result = E_FAIL;
				return false;
			}

			if (state.device != device || state.renderer != renderer
				|| state.generation != generation || state.declaration != declaration)
			{
				if (state.sortedFrameLeases.load(std::memory_order_acquire)
					|| state.activeSubmissions.load(std::memory_order_acquire))
				{
					operation = "ring-busy";
					result = D3DERR_WASSTILLDRAWING;
					return false;
				}
				ReleaseRingResourcesLocked(state);
			}
			if (state.vertexBuffer && state.indexBuffer)
			{
				if (requiredVertices <= state.vertexCapacity)
					return true;
				if (state.sortedFrameLeases.load(std::memory_order_acquire)
					|| state.activeSubmissions.load(std::memory_order_acquire))
				{
					operation = "ring-busy";
					result = D3DERR_WASSTILLDRAWING;
					return false;
				}
				ReleaseRingResourcesLocked(state);
			}
			if (state.vertexBuffer || state.staticVertexBuffer || state.indexBuffer)
			{
				if (state.sortedFrameLeases.load(std::memory_order_acquire)
					|| state.activeSubmissions.load(std::memory_order_acquire))
				{
					operation = "ring-busy";
					result = D3DERR_WASSTILLDRAWING;
					return false;
				}
				ReleaseRingResourcesLocked(state);
			}

			const UInt64 indexCap = static_cast<UInt64>(
				renderer->m_kD3DCaps9.MaxVertexIndex) + 1u;
			const UInt64 byteCap = std::numeric_limits<UINT>::max()
				/ sizeof(NativeFontGpuVertex);
			const UInt64 capLimit = std::min<UInt64>(indexCap,
				std::min<UInt64>(byteCap, std::numeric_limits<UInt32>::max()));
			UInt64 desired = std::min<UInt64>(
				std::max<UInt64>(kRingTargetVertexCapacity, requiredVertices),
				capLimit);
			desired &= ~static_cast<UInt64>(3u);
			UInt64 staticDesired = std::min<UInt64>(
				kStaticInitialVertexCapacity, capLimit);
			staticDesired &= ~static_cast<UInt64>(3u);
			if (desired < requiredVertices || desired > std::numeric_limits<UInt32>::max())
			{
				operation = "ring-capacity";
				result = D3DERR_NOTAVAILABLE;
				return false;
			}

			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DVertexBuffer9* staticVertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			operation = "CreateVertexBuffer";
			result = device->CreateVertexBuffer(
				static_cast<UINT>(desired) * sizeof(NativeFontGpuVertex),
				D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
				&vertexBuffer, nullptr);
			if (FAILED(result) || !vertexBuffer)
			{
				if (SUCCEEDED(result))
					result = E_FAIL;
				return false;
			}
			if (staticDesired)
			{
				const HRESULT staticResult = device->CreateVertexBuffer(
					static_cast<UINT>(staticDesired) * sizeof(NativeFontGpuVertex),
					D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
					&staticVertexBuffer, nullptr);
				if (FAILED(staticResult) || !staticVertexBuffer)
				{
					if (staticVertexBuffer)
						staticVertexBuffer->Release();
					staticVertexBuffer = nullptr;
					staticDesired = 0;
				}
			}

			operation = "CreateIndexBuffer";
			result = device->CreateIndexBuffer(kCanonicalIndexBytes,
				D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
				&indexBuffer, nullptr);
			if (FAILED(result) || !indexBuffer)
			{
				if (SUCCEEDED(result))
					result = E_FAIL;
				if (staticVertexBuffer)
					staticVertexBuffer->Release();
				vertexBuffer->Release();
				return false;
			}
			operation = "canonical-index-upload";
			if (!PopulateCanonicalIndexBuffer(indexBuffer, result))
			{
				indexBuffer->Release();
				if (staticVertexBuffer)
					staticVertexBuffer->Release();
				vertexBuffer->Release();
				return false;
			}

			state.renderer = renderer;
			state.device = device;
			state.vertexBuffer = vertexBuffer;
			state.staticVertexBuffer = staticVertexBuffer;
			state.indexBuffer = indexBuffer;
			state.declaration = declaration;
			state.generation = generation;
			state.vertexCapacity = static_cast<UInt32>(desired);
			state.nextVertex = 0;
			state.staticVertexCapacity = static_cast<UInt32>(staticDesired);
			state.nextStaticVertex = 0;
			state.lastStaticCompactionFrame = 0;
			state.lastStaticCompactionDeferredLogFrame = 0;
			state.lastStaticCandidateSweepFrame = 0;
			state.staticPromotionBudgetFrame = 0;
			state.staticPromotionGlobalRetryFrame = 0;
			state.staticPromotionBytesThisFrame = 0;
			state.staticPromotionPayloadsThisFrame = 0;
			state.staticCompactionFrameValid = false;
			state.staticCompactionDeferredLogFrameValid = false;
			state.staticCandidateSweepFrameValid = false;
			state.staticPromotionBudgetFrameValid = false;
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				NativeFontProxy& proxy = state.proxies[index];
				if (!proxy.buffer || !proxy.chip)
					continue;
				proxy.chip->m_pkVB = vertexBuffer;
				proxy.buffer->m_hDeclaration = declaration;
				declaration->AddRef();
				proxy.buffer->m_pkIB = indexBuffer;
				indexBuffer->AddRef();
				proxy.buffer->m_uiIBSize = kCanonicalIndexBytes;
			}
			if (g_bEnableFreeTypeFontRenderingLog && !state.loggedReady)
			{
				state.loggedReady = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: geometry cache ready generation=%u proxies=%u dynamicVertexCapacity=%u staticVertexCapacity=%u staticPromotionBaseFrames=%u-%u staticPromotionOversizeMaxFrames=%u staticPromotionBudgetBytes=%u staticPromotionPayloadLimit=%u staticCandidateLimit=%u staticCandidateSweepFrames=%u staticCandidateInactiveFrames=%u vertexStride=%u canonicalQuads=%u canonicalIndexBytes=%u",
					generation, state.proxyCount, state.vertexCapacity,
					state.staticVertexCapacity,
					kStaticPromotionMinimumFrameCount,
					kStaticPromotionMaximumBaseFrameCount,
					kStaticPromotionMaximumOversizeFrameCount,
					kStaticPromotionBudgetBytes,
					kStaticPromotionPayloadLimit,
					static_cast<UInt32>(kStaticCandidateLimit),
					kStaticCandidateSweepIntervalFrames,
					kStaticCandidateInactiveFrames,
					static_cast<UInt32>(sizeof(NativeFontGpuVertex)),
					kNativeFontMaximumQuads, kCanonicalIndexBytes);
			}
			operation = "none";
			result = D3D_OK;
			return true;
		}
	}
}
