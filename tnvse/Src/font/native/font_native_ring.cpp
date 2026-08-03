#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9ShaderDeclaration.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiMemory.hpp"
#include "NiPoint4.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

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
	namespace implementation::font_native_ring {}
	using namespace implementation::font_native_ring;

	namespace implementation::font_native_ring
	{
		inline constexpr UInt32 kScissorTriShapeSize = 0xD4;
		inline constexpr UInt32 kScissorTailOffset = 0xC4;
		inline constexpr UInt32 kScissorTailSize = 0x10;
		inline constexpr UInt32 kProxyPoolSize = 4;
		inline constexpr UInt32 kRingTargetVertexCapacity =
			kNativeA8MaximumQuads * 4u * 2u;
		inline constexpr UInt32 kStaticTargetVertexCapacity =
			kRingTargetVertexCapacity * 4u;
		inline constexpr UInt32 kStaticInitialVertexBytes = 4u * 1024u * 1024u;
		inline constexpr UInt32 kStaticInitialVertexCapacity =
			(kStaticInitialVertexBytes / sizeof(NativeA8GpuVertex)) & ~3u;
		inline constexpr UInt32 kStaticPromotionMinimumFrameCount = 2;
		inline constexpr UInt32 kStaticPromotionMaximumBaseFrameCount = 16;
		inline constexpr UInt32 kStaticPromotionMaximumOversizeFrameCount = 32;
		inline constexpr UInt32 kStaticPromotionMaximumFrameGap = 2;
		inline constexpr UInt32 kStaticPromotionRetryFrames = 8;
		inline constexpr UInt32 kStaticPromotionBudgetBytes = 1024u * 1024u;
		inline constexpr UInt32 kStaticPromotionPayloadLimit = 128;
		inline constexpr UInt32 kStaticCandidateInactiveFrames = 600;
		inline constexpr UInt32 kStaticCandidateSweepIntervalFrames = 60;
		inline constexpr UInt32 kStaticCompactionCooldownFrames = 2;
		inline constexpr UInt32 kStaticCompactionReserveDivisor = 8;
		inline constexpr size_t kStaticCandidateLimit = 4096;
		inline constexpr UInt32 kCanonicalIndexCount =
			kNativeA8MaximumQuads * 6u;
		inline constexpr UInt32 kCanonicalIndexBytes =
			kCanonicalIndexCount * sizeof(UInt16);
		inline constexpr UInt32 kCanonicalArrayCount = 1;

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
			const NativeA8PayloadTemplate& payloadTemplate)
		{
			if (!g_bEnableFreeTypeFontRenderingLog
				|| payloadTemplate.gpuVertices.empty())
			{
				return 0;
			}
			return HashDiagnosticBytes(payloadTemplate.gpuVertices.data(),
				payloadTemplate.gpuVertices.size()
					* sizeof(NativeA8GpuVertex));
		}

		UInt32 AdvanceDiagnosticSerial(UInt32& serial)
		{
			if (++serial == 0)
				++serial;
			return serial;
		}

		// Font::MakeTriShape returns a BSScissorTriShape and a TileShaderProperty,
		// but this CommonLib snapshot does not expose either concrete definition.
		struct TileShaderPropertyView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			bool byte90 = false;
			bool rotates = false;
			bool hasVertexColors = false;
			bool noTexture = false;
			BSStringT<char> texturePath;
			RECT scissorRect = {};
			bool useScissorTest = false;
		};

		static_assert(sizeof(NiTriShape) == kScissorTailOffset);
		static_assert(kScissorTailOffset + kScissorTailSize
			== kScissorTriShapeSize);
		static_assert(sizeof(TileShaderPropertyView) == 0xB0);
		// Retail TileShader::UpdateConstants reads the live color at +0x68 and
		// tile alpha at +0x78. Keep the proxy view tied to that executable ABI;
		// a layout drift here would silently turn every shader-side color fix into
		// reads from the wrong fields.
		static_assert(offsetof(TileShaderPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(TileShaderPropertyView, tileAlpha) == 0x78);
		static_assert(offsetof(TileShaderPropertyView, textureTransform) == 0x7C);
		static_assert(offsetof(TileShaderPropertyView, clampMode) == 0x8C);
		static_assert(offsetof(TileShaderPropertyView, rotates) == 0x91);
		static_assert(offsetof(TileShaderPropertyView, hasVertexColors) == 0x92);

		struct NativeA8Proxy
		{
			NiTriShapePtr shape;
			NiAlphaPropertyPtr alphaProperty;
			NiGeometryBufferData* buffer = nullptr;
			NiVBChip* chip = nullptr;
			TileShaderPropertyView* tile = nullptr;
			NiTexturingProperty* atlasProperty = nullptr;
			NiTexture* atlasTexture = nullptr;
			BSShader* shader = nullptr;
			bool inUse = false;
		};

		struct NativeA8UploadedPayload
		{
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 epoch = 0;
			UInt32 writeSerial = 0;
			UInt32 discardSerial = 0;
			UInt64 payloadHash = 0;
		};

		struct NativeA8StaticPayload
		{
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 writeSerial = 0;
			UInt64 payloadHash = 0;
		};

		struct NativeA8StaticCandidate
		{
			CpuMemoryLease cpuMemory;
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 firstObservedFrame = 0;
			UInt32 lastObservedFrame = 0;
			UInt32 activeObservedFrames = 0;
			UInt32 dynamicUploadEpochCount = 0;
			UInt32 lastDynamicUploadEpoch = 0;
			UInt32 lastDynamicUploadResourceSerial = 0;
			UInt32 nextRetryFrame = 0;
			bool observationFrameValid = false;
			bool promotionDisabled = false;
		};

		struct NativeA8StaticHotEntry
		{
			const NativeA8PayloadTemplate* key = nullptr;
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 resourceSerial = 0;
		};

		struct NativeA8UploadHotEntry
		{
			const NativeA8PayloadTemplate* key = nullptr;
			NativeA8PayloadTemplatePtr owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 epoch = 0;
			UInt32 resourceSerial = 0;
		};

		struct NativeA8CandidateHotEntry
		{
			const NativeA8PayloadTemplate* key = nullptr;
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			std::shared_ptr<NativeA8StaticCandidate> candidate;
			UInt32 resourceSerial = 0;
		};

		struct NativeA8RingThreadState
		{
			UInt32 preferredProxy = std::numeric_limits<UInt32>::max();
			NativeA8StaticHotEntry staticPayload;
			NativeA8UploadHotEntry uploadedPayload;
			NativeA8CandidateHotEntry staticCandidate;
		};

		thread_local NativeA8RingThreadState s_ringThread;

		struct NativeA8RingState
		{
			std::mutex mutex;
			std::array<NativeA8Proxy, kProxyPoolSize> proxies;
			UInt32 proxyCount = 0;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DVertexBuffer9* staticVertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			UInt32 generation = 0;
			UInt32 vertexCapacity = 0;
			UInt32 nextVertex = 0;
			UInt32 staticVertexCapacity = 0;
			UInt32 nextStaticVertex = 0;
			UInt32 uploadEpoch = 1;
			UInt32 dynamicWriteSerial = 0;
			UInt32 dynamicDiscardSerial = 0;
			UInt32 staticWriteSerial = 0;
			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8UploadedPayload> uploadedPayloads;
			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8StaticPayload> staticPayloads;
			std::unordered_map<const NativeA8PayloadTemplate*,
				std::shared_ptr<NativeA8StaticCandidate>> staticCandidates;
			CpuMemoryLease cpuMemory;
			std::atomic<UInt32> resourceSerial = 1;
			// Proxy shapes live for the process lifetime.  Once the complete pool is
			// published, registration no longer needs the ring mutex; device reset
			// only clears/rebinds their borrowed GPU resources.
			std::atomic<bool> proxyPoolReady = false;
			std::atomic<UInt32> sortedFrameLeases = 0;
			std::atomic<UInt32> activeSubmissions = 0;
			std::atomic<bool> releasePending = false;
			UInt32 lastStaticCompactionFrame = 0;
			UInt32 lastStaticCompactionDeferredLogFrame = 0;
			UInt32 lastStaticCandidateSweepFrame = 0;
			UInt32 staticPromotionBudgetFrame = 0;
			UInt32 staticPromotionGlobalRetryFrame = 0;
			UInt32 staticPromotionBytesThisFrame = 0;
			UInt32 staticPromotionPayloadsThisFrame = 0;
			bool staticCompactionFrameValid = false;
			bool staticCompactionDeferredLogFrameValid = false;
			bool staticCandidateSweepFrameValid = false;
			bool staticPromotionBudgetFrameValid = false;
			bool loggedReady = false;
		};

		struct NativeA8SortedRingLease
		{
			NativeA8RingState* state = nullptr;
			IDirect3DVertexBuffer9* dynamicVertexBuffer = nullptr;
			IDirect3DVertexBuffer9* staticVertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			UInt32 generation = 0;
			UInt32 resourceSerial = 0;
			UInt32 uploadEpoch = 0;
			bool active = false;
		};

		thread_local NativeA8SortedRingLease s_sortedRingLease;

		NativeA8RingState& RingState()
		{
			// Renderer generations are process-lifetime objects. Keep the equally small
			// proxy pool alive for the same interval so raw geometry/shader links cannot
			// be torn down during late engine shutdown.
			static NativeA8RingState* state = new NativeA8RingState();
			return *state;
		}

		template <class Map>
		size_t EstimateUnorderedMapBytes(const Map& map)
		{
			return map.bucket_count() * sizeof(void*)
				+ map.size() * (sizeof(typename Map::value_type)
					+ 3u * sizeof(void*));
		}

		void RefreshRingCpuMemoryLocked(NativeA8RingState& state)
		{
			size_t bytes = EstimateUnorderedMapBytes(state.uploadedPayloads)
				+ EstimateUnorderedMapBytes(state.staticPayloads)
				+ EstimateUnorderedMapBytes(state.staticCandidates);
			state.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		std::shared_ptr<NativeA8StaticCandidate> CreateStaticCandidate(
			const NativeA8PayloadTemplatePtr& payloadTemplate)
		{
			auto candidate = std::make_shared<NativeA8StaticCandidate>();
			candidate->owner = payloadTemplate;
			candidate->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				sizeof(NativeA8StaticCandidate) + 2u * sizeof(void*));
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

		bool AttachProxyBuffer(NativeA8Proxy& proxy)
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

			ThisStdCall<void>(kGeometryBufferDataConstructor, buffer);
			std::memset(chipMemory, 0, sizeof(NiVBChip*) + sizeof(NiVBChip));
			auto** chips = static_cast<NiVBChip**>(chipMemory);
			NiVBChip* chip = reinterpret_cast<NiVBChip*>(
				static_cast<UInt8*>(chipMemory) + sizeof(NiVBChip*));
			chips[0] = chip;
			*stride = sizeof(NativeA8GpuVertex);

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

		void ClearProxyGpuBindings(NativeA8Proxy& proxy)
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

		UInt32 AdvanceResourceSerialLocked(NativeA8RingState& state)
		{
			UInt32 serial = state.resourceSerial.fetch_add(
				1, std::memory_order_release) + 1u;
			if (!serial)
			{
				serial = 1u;
				state.resourceSerial.store(serial, std::memory_order_release);
			}
			NotifyNativeA8CommandExternalMutation(
				NativeA8CommandFallback::Resource);
			return serial;
		}

		void AdvanceUploadEpochLocked(NativeA8RingState& state)
		{
			if (++state.uploadEpoch == 0)
				++state.uploadEpoch;
			NotifyNativeA8CommandExternalMutation(
				NativeA8CommandFallback::Resource);
		}

		void ReleaseRingResourcesLocked(NativeA8RingState& state)
		{
			// Virtual-stock descriptors borrow the ring's COM resources without
			// owning references. Restore every live shape to its stock shell before
			// any buffer or declaration can be released.
			InvalidateAllVirtualStockBindings();
			state.releasePending.store(false, std::memory_order_release);
			AdvanceUploadEpochLocked(state);
			state.uploadedPayloads.clear();
			state.staticPayloads.clear();
			state.staticCandidates.clear();
			RefreshRingCpuMemoryLocked(state);
			AdvanceResourceSerialLocked(state);
			s_ringThread.staticPayload = {};
			s_ringThread.uploadedPayload = {};
			s_ringThread.staticCandidate = {};
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
			for (UInt32 quad = 0; quad < kNativeA8MaximumQuads; ++quad)
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

		bool EnsureRingResourcesLocked(NativeA8RingState& state,
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
			const UInt32 generation = GetNativeA8ShaderGeneration();
			if (!renderer || !device || !generation
				|| generation != preparedGeneration)
			{
				operation = "ring-context";
				result = D3DERR_DEVICELOST;
				return false;
			}
			IDirect3DVertexDeclaration9* declaration =
				GetNativeA8D3DDeclaration(generation);
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
				operation = "ring-capacity";
				result = D3DERR_NOTAVAILABLE;
				return false;
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

			const UInt64 capLimit = static_cast<UInt64>(
				renderer->m_kD3DCaps9.MaxVertexIndex) + 1u;
			UInt64 desired = std::min<UInt64>(kRingTargetVertexCapacity, capLimit);
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
				static_cast<UINT>(desired) * sizeof(NativeA8GpuVertex),
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
					static_cast<UINT>(staticDesired) * sizeof(NativeA8GpuVertex),
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
				NativeA8Proxy& proxy = state.proxies[index];
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
					static_cast<UInt32>(sizeof(NativeA8GpuVertex)),
					kNativeA8MaximumQuads, kCanonicalIndexBytes);
			}
			operation = "none";
			result = D3D_OK;
			return true;
		}

		struct LiveStaticPayload
		{
			NativeA8PayloadTemplatePtr owner;
			UInt32 baseVertex = 0;
		};

		UInt32 GetStaticObservationFrame(const NativeA8RingState& state)
		{
			return state.renderer ? state.renderer->m_uiFrameID : 0;
		}

		struct StaticPromotionPolicy
		{
			UInt32 maximumBytes = 0;
			UInt32 minimumActiveFrames = 0;
			UInt32 minimumDynamicUploadEpochs = 0;
			UInt32 coldResidentFrames = 0;
		};

		inline constexpr std::array<StaticPromotionPolicy, 5>
			kStaticPromotionPolicies = {{
				{ 32u * 1024u, 2, 2, 1800 },
				{ 128u * 1024u, 3, 2, 900 },
				{ 512u * 1024u, 5, 3, 450 },
				{ 2u * 1024u * 1024u, 8, 4, 240 },
				{ std::numeric_limits<UInt32>::max(), 16, 6, 120 },
			}};

		const StaticPromotionPolicy& GetStaticPromotionPolicy(UInt32 vertexCount)
		{
			const UInt64 bytes = static_cast<UInt64>(vertexCount)
				* sizeof(NativeA8GpuVertex);
			for (const StaticPromotionPolicy& policy : kStaticPromotionPolicies)
			{
				if (bytes <= policy.maximumBytes)
					return policy;
			}
			return kStaticPromotionPolicies.back();
		}

		enum class StaticPromotionReadiness : UInt8
		{
			Ready = 0,
			Disabled,
			Lifecycle,
			UploadHistory,
			Retry,
		};

		bool IsFrameBefore(UInt32 current, UInt32 target)
		{
			return static_cast<std::int32_t>(current - target) < 0;
		}

		StaticPromotionReadiness GetStaticPromotionReadiness(
			const NativeA8RingState& state,
			const NativeA8StaticCandidate& candidate, UInt32 vertexCount,
			UInt32 frame, UInt32 maturityMultiplier = 1)
		{
			if (candidate.promotionDisabled)
				return StaticPromotionReadiness::Disabled;
			if ((state.staticPromotionGlobalRetryFrame
					&& IsFrameBefore(frame,
						state.staticPromotionGlobalRetryFrame))
				|| (candidate.nextRetryFrame
					&& IsFrameBefore(frame, candidate.nextRetryFrame)))
			{
				return StaticPromotionReadiness::Retry;
			}
			const StaticPromotionPolicy& policy =
				GetStaticPromotionPolicy(vertexCount);
			const UInt32 requiredFrames = policy.minimumActiveFrames
				* maturityMultiplier;
			if (!candidate.observationFrameValid
				|| candidate.activeObservedFrames < requiredFrames
				|| static_cast<UInt32>(frame
					- candidate.firstObservedFrame) + 1u < requiredFrames
				|| static_cast<UInt32>(frame - candidate.lastObservedFrame)
					> kStaticPromotionMaximumFrameGap)
			{
				return StaticPromotionReadiness::Lifecycle;
			}
			const UInt32 requiredUploads = policy.minimumDynamicUploadEpochs
				* maturityMultiplier;
			if (candidate.dynamicUploadEpochCount < requiredUploads)
				return StaticPromotionReadiness::UploadHistory;
			return StaticPromotionReadiness::Ready;
		}

		void ResetStaticPromotionBudgetFrame(NativeA8RingState& state,
			UInt32 frame)
		{
			if (state.staticPromotionBudgetFrameValid
				&& state.staticPromotionBudgetFrame == frame)
			{
				return;
			}
			state.staticPromotionBudgetFrame = frame;
			state.staticPromotionBytesThisFrame = 0;
			state.staticPromotionPayloadsThisFrame = 0;
			state.staticPromotionBudgetFrameValid = true;
		}

		bool FitsStaticPromotionBudget(NativeA8RingState& state,
			const NativeA8StaticCandidate& candidate, UInt32 vertexCount,
			UInt32 pendingBytes, UInt32 pendingPayloads)
		{
			const UInt32 frame = GetStaticObservationFrame(state);
			ResetStaticPromotionBudgetFrame(state, frame);
			const UInt64 byteCount = static_cast<UInt64>(vertexCount)
				* sizeof(NativeA8GpuVertex);
			const UInt64 usedBytes = static_cast<UInt64>(
				state.staticPromotionBytesThisFrame) + pendingBytes;
			const UInt64 usedPayloads = static_cast<UInt64>(
				state.staticPromotionPayloadsThisFrame) + pendingPayloads;
			if (byteCount > kStaticPromotionBudgetBytes)
			{
				return !usedBytes && !usedPayloads
					&& GetStaticPromotionReadiness(state, candidate, vertexCount,
						frame, 2) == StaticPromotionReadiness::Ready;
			}
			return usedPayloads < kStaticPromotionPayloadLimit
				&& usedBytes + byteCount <= kStaticPromotionBudgetBytes;
		}

		void CommitStaticPromotionBudget(NativeA8RingState& state,
			UInt32 bytes, UInt32 payloads)
		{
			ResetStaticPromotionBudgetFrame(state,
				GetStaticObservationFrame(state));
			state.staticPromotionBytesThisFrame += bytes;
			state.staticPromotionPayloadsThisFrame += payloads;
		}

		void DeferStaticPromotionsLocked(NativeA8RingState& state, UInt32 frame)
		{
			state.staticPromotionGlobalRetryFrame = frame
				+ kStaticPromotionRetryFrames;
			if (!state.staticPromotionGlobalRetryFrame)
				state.staticPromotionGlobalRetryFrame = 1;
		}

		void ClearMatchingStaticResidency(
			const NativeA8RingState& state,
			const NativeA8StaticPayload& payload,
			const NativeA8PayloadTemplatePtr& owner)
		{
			if (!owner)
				return;
			NativeA8PayloadResidencyCache& residency = owner->residency;
			if (residency.staticResourceSerial
					!= state.resourceSerial.load(std::memory_order_relaxed)
				|| residency.staticBaseVertex != payload.baseVertex
				|| residency.staticVertexCount != payload.vertexCount)
			{
				return;
			}
			residency.staticResourceSerial = 0;
			residency.staticBaseVertex = 0;
			residency.staticVertexCount = 0;
			residency.staticLastUsedFrame = 0;
		}

		void ReclaimExpiredAndColdStaticPayloadsLocked(NativeA8RingState& state,
			UInt32 requiredVertices)
		{
			// Hot entries never own payload lifetime. Clear the local location cache
			// before erasing residency so it cannot resurrect a reclaimed tail slot.
			s_ringThread.staticPayload = {};

			const UInt32 previousNextVertex = state.nextStaticVertex;
			const UInt32 currentFrame = GetStaticObservationFrame(state);
			const bool underPressure = state.nextStaticVertex
				> state.staticVertexCapacity
				|| requiredVertices > state.staticVertexCapacity
					- std::min(state.nextStaticVertex,
						state.staticVertexCapacity);
			struct ColdPayload
			{
				const NativeA8PayloadTemplate* key = nullptr;
				UInt32 vertexCount = 0;
				UInt32 ageFrames = 0;
			};
			std::vector<ColdPayload> coldPayloads;
			CpuMemoryLease reclaimCpuMemory;
			if (underPressure)
				coldPayloads.reserve(state.staticPayloads.size());
			UInt64 liveVertexCount = 0;
			UInt32 removedPayloads = 0;
			UInt32 coldRemovedPayloads = 0;
			UInt64 coldRemovedVertices = 0;
			for (auto current = state.staticPayloads.begin();
				current != state.staticPayloads.end();)
			{
				const NativeA8StaticPayload payload = current->second;
				NativeA8PayloadTemplatePtr owner = payload.owner.lock();
				const bool valid = owner && owner.get() == current->first
					&& owner->gpuVertices.size() == payload.vertexCount
					&& payload.baseVertex <= state.staticVertexCapacity
					&& payload.vertexCount <= state.staticVertexCapacity
						- payload.baseVertex
					&& payload.baseVertex <= previousNextVertex
					&& payload.vertexCount <= previousNextVertex
						- payload.baseVertex;
				if (!valid)
				{
					if (owner && owner.get() == current->first)
						ClearMatchingStaticResidency(state, payload, owner);
					current = state.staticPayloads.erase(current);
					++removedPayloads;
					continue;
				}
				liveVertexCount += payload.vertexCount;
				if (underPressure)
				{
					const NativeA8PayloadResidencyCache& residency =
						owner->residency;
					const UInt32 ageFrames = static_cast<UInt32>(
						currentFrame - residency.staticLastUsedFrame);
					const StaticPromotionPolicy& policy =
						GetStaticPromotionPolicy(payload.vertexCount);
					if (ageFrames && ageFrames >= policy.coldResidentFrames)
					{
						coldPayloads.push_back({
							current->first, payload.vertexCount, ageFrames });
					}
				}
				++current;
			}
			reclaimCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				coldPayloads.capacity() * sizeof(ColdPayload));

			if (underPressure
				&& liveVertexCount + requiredVertices
					> state.staticVertexCapacity && !coldPayloads.empty())
			{
				std::sort(coldPayloads.begin(), coldPayloads.end(),
					[](const ColdPayload& left, const ColdPayload& right)
					{
						const UInt64 leftScore = static_cast<UInt64>(
							left.ageFrames) * left.vertexCount;
						const UInt64 rightScore = static_cast<UInt64>(
							right.ageFrames) * right.vertexCount;
						if (leftScore != rightScore)
							return leftScore > rightScore;
						return left.vertexCount > right.vertexCount;
					});
				for (const ColdPayload& cold : coldPayloads)
				{
					if (liveVertexCount + requiredVertices
						<= state.staticVertexCapacity)
					{
						break;
					}
					auto found = state.staticPayloads.find(cold.key);
					if (found == state.staticPayloads.end())
						continue;
					const NativeA8StaticPayload payload = found->second;
					NativeA8PayloadTemplatePtr owner = payload.owner.lock();
					if (owner && owner.get() == found->first)
						ClearMatchingStaticResidency(state, payload, owner);
					liveVertexCount -= std::min<UInt64>(
						liveVertexCount, payload.vertexCount);
					coldRemovedVertices += payload.vertexCount;
					state.staticPayloads.erase(found);
					++coldRemovedPayloads;
					++removedPayloads;
				}
			}

			UInt32 liveEndVertex = 0;
			for (const auto& entry : state.staticPayloads)
			{
				liveEndVertex = std::max(liveEndVertex,
					entry.second.baseVertex + entry.second.vertexCount);
			}

			if (liveEndVertex < state.nextStaticVertex)
				state.nextStaticVertex = liveEndVertex;
			if (coldRemovedPayloads)
			{
				// Cold entries can still have live command/virtual-stock descriptors.
				// Invalidate the shared resource identity before a reclaimed tail range
				// is reused, then republish only the retained static locations.
				InvalidateAllVirtualStockBindings();
				AdvanceUploadEpochLocked(state);
				state.uploadedPayloads.clear();
				const UInt32 resourceSerial =
					AdvanceResourceSerialLocked(state);
				for (const auto& entry : state.staticPayloads)
				{
					NativeA8PayloadTemplatePtr owner = entry.second.owner.lock();
					if (!owner || owner.get() != entry.first)
						continue;
					NativeA8PayloadResidencyCache& residency = owner->residency;
					residency.staticResourceSerial = resourceSerial;
					residency.staticBaseVertex = entry.second.baseVertex;
					residency.staticVertexCount = entry.second.vertexCount;
				}
				s_ringThread.staticPayload = {};
				s_ringThread.uploadedPayload = {};
				s_ringThread.staticCandidate = {};
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticResidentColdEviction,
					coldRemovedPayloads);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticResidentColdEvictionBytes,
					coldRemovedVertices * sizeof(NativeA8GpuVertex));
			}
			if (removedPayloads)
				RefreshRingCpuMemoryLocked(state);

			const UInt32 reclaimedVertices =
				previousNextVertex - state.nextStaticVertex;
			if (g_bEnableFreeTypeFontRenderingLog
				&& (removedPayloads || reclaimedVertices))
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_native: static vertex residency reclaimed removedPayloads=%u coldPayloads=%u coldBytes=%llu reclaimedTailVertices=%u residentVertices=%u capacity=%u requestedVertices=%u",
					removedPayloads, coldRemovedPayloads,
					static_cast<unsigned long long>(coldRemovedVertices
						* sizeof(NativeA8GpuVertex)), reclaimedVertices,
					state.nextStaticVertex, state.staticVertexCapacity,
					requiredVertices);
			}
		}

		bool TryGrowStaticVertexBufferLocked(NativeA8RingState& state,
			UInt32 requiredVertices, bool& permanentFailure)
		{
			permanentFailure = false;
			if (requiredVertices > kStaticTargetVertexCapacity)
			{
				permanentFailure = true;
				return false;
			}
			if (!state.device || !state.staticVertexBuffer || !requiredVertices)
				return false;

			// A published submission can still be issuing packets against the old
			// buffer. The one permitted in-use proxy is only the caller's reserved,
			// not-yet-published proxy; any other proxy or active submission makes
			// replacement unsafe.
			const UInt32 activeProxies = static_cast<UInt32>(std::count_if(
				state.proxies.begin(), state.proxies.begin() + state.proxyCount,
				[](const NativeA8Proxy& proxy) { return proxy.inUse; }));
			if (state.activeSubmissions.load(std::memory_order_acquire)
				|| state.sortedFrameLeases.load(std::memory_order_acquire)
				|| activeProxies > 1)
				return false;

			ReclaimExpiredAndColdStaticPayloadsLocked(state, requiredVertices);
			if (state.nextStaticVertex <= state.staticVertexCapacity
				&& requiredVertices <= state.staticVertexCapacity
					- state.nextStaticVertex)
			{
				return true;
			}

			std::vector<LiveStaticPayload> livePayloads;
			CpuMemoryLease rebuildCpuMemory;
			livePayloads.reserve(state.staticPayloads.size());
			rebuildCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				livePayloads.capacity() * sizeof(LiveStaticPayload));
			UInt64 liveVertexCount = 0;
			for (const auto& entry : state.staticPayloads)
			{
				NativeA8PayloadTemplatePtr owner = entry.second.owner.lock();
				if (!owner || owner.get() != entry.first
					|| owner->gpuVertices.size() != entry.second.vertexCount)
				{
					continue;
				}
				if (liveVertexCount + owner->gpuVertices.size()
					> kStaticTargetVertexCapacity)
				{
					return false;
				}
				livePayloads.push_back({ std::move(owner),
					static_cast<UInt32>(liveVertexCount) });
				liveVertexCount += livePayloads.back().owner->gpuVertices.size();
			}

			const UInt64 requiredCapacity = liveVertexCount + requiredVertices;
			if (requiredCapacity > kStaticTargetVertexCapacity)
				return false;
			const UInt64 currentCapacity = state.staticVertexCapacity;
			const UInt64 reserveVertices = std::max<UInt64>(4,
				currentCapacity / kStaticCompactionReserveDivisor);
			const UInt64 capacityTarget = std::min<UInt64>(
				kStaticTargetVertexCapacity,
				requiredCapacity + reserveVertices);
			UInt64 desiredCapacity = currentCapacity;
			while (desiredCapacity < capacityTarget
				&& desiredCapacity < kStaticTargetVertexCapacity)
			{
				desiredCapacity = std::min<UInt64>(
					desiredCapacity * 2u, kStaticTargetVertexCapacity);
			}
			// Rebuilding at the same size is useful only when expired payloads left
			// holes behind the bump pointer. It compacts the live prefix in one upload.
			if (desiredCapacity == state.staticVertexCapacity
				&& liveVertexCount == state.nextStaticVertex)
				return false;
			const bool sameSizeCompaction =
				desiredCapacity == state.staticVertexCapacity;
			const UInt32 currentFrame = GetStaticObservationFrame(state);
			if (sameSizeCompaction && state.staticCompactionFrameValid
				&& static_cast<UInt32>(currentFrame
					- state.lastStaticCompactionFrame)
					< kStaticCompactionCooldownFrames)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& (!state.staticCompactionDeferredLogFrameValid
						|| state.lastStaticCompactionDeferredLogFrame
							!= currentFrame))
				{
					state.lastStaticCompactionDeferredLogFrame = currentFrame;
					state.staticCompactionDeferredLogFrameValid = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_native: static vertex buffer rebuild deferred reason=compaction-cooldown frame=%u lastCompactionFrame=%u cooldownFrames=%u liveVertices=%u capacity=%u requestedVertices=%u",
						currentFrame, state.lastStaticCompactionFrame,
						kStaticCompactionCooldownFrames,
						static_cast<UInt32>(liveVertexCount),
						state.staticVertexCapacity, requiredVertices);
				}
				return false;
			}

			IDirect3DVertexBuffer9* replacement = nullptr;
			HRESULT result = state.device->CreateVertexBuffer(
				static_cast<UINT>(desiredCapacity) * sizeof(NativeA8GpuVertex),
				D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &replacement, nullptr);
			if (FAILED(result) || !replacement)
			{
				if (replacement)
					replacement->Release();
				return false;
			}

			if (liveVertexCount)
			{
				void* destination = nullptr;
				const UINT liveBytes = static_cast<UINT>(liveVertexCount)
					* sizeof(NativeA8GpuVertex);
				result = replacement->Lock(0, liveBytes, &destination, 0);
				if (FAILED(result) || !destination)
				{
					replacement->Release();
					return false;
				}
				for (const LiveStaticPayload& payload : livePayloads)
				{
					std::memcpy(static_cast<UInt8*>(destination)
						+ payload.baseVertex * sizeof(NativeA8GpuVertex),
						payload.owner->gpuVertices.data(),
						payload.owner->gpuVertices.size()
							* sizeof(NativeA8GpuVertex));
				}
				result = replacement->Unlock();
				if (FAILED(result))
				{
					replacement->Release();
					return false;
				}
			}
			if (liveVertexCount)
				AdvanceDiagnosticSerial(state.staticWriteSerial);

			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8StaticPayload> rebuilt;
			rebuilt.reserve(livePayloads.size());
			for (const LiveStaticPayload& payload : livePayloads)
			{
				rebuilt.emplace(payload.owner.get(), NativeA8StaticPayload{
					payload.owner, payload.baseVertex,
					static_cast<UInt32>(payload.owner->gpuVertices.size()),
					state.staticWriteSerial,
					HashDiagnosticPayload(*payload.owner) });
			}
			rebuildCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				livePayloads.capacity() * sizeof(LiveStaticPayload)
					+ EstimateUnorderedMapBytes(rebuilt));
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (state.proxies[index].chip)
					state.proxies[index].chip->m_pkVB = state.vertexBuffer;
			}
			InvalidateAllVirtualStockBindings();
			state.staticVertexBuffer->Release();
			state.staticVertexBuffer = replacement;
			state.staticVertexCapacity = static_cast<UInt32>(desiredCapacity);
			state.nextStaticVertex = static_cast<UInt32>(liveVertexCount);
			state.staticPayloads = std::move(rebuilt);
			if (sameSizeCompaction)
			{
				state.lastStaticCompactionFrame = currentFrame;
				state.staticCompactionFrameValid = true;
			}
			else
			{
				state.lastStaticCompactionFrame = 0;
				state.staticCompactionFrameValid = false;
			}
			rebuildCpuMemory.Release();
			RefreshRingCpuMemoryLocked(state);
			const UInt32 resourceSerial = AdvanceResourceSerialLocked(state);
			for (const LiveStaticPayload& payload : livePayloads)
			{
				NativeA8PayloadResidencyCache& residency =
					payload.owner->residency;
				residency.staticResourceSerial = resourceSerial;
				residency.staticBaseVertex = payload.baseVertex;
				residency.staticVertexCount = static_cast<UInt32>(
					payload.owner->gpuVertices.size());
			}
			s_ringThread.staticPayload = {};
			s_ringThread.uploadedPayload = {};
			s_ringThread.staticCandidate = {};
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const UInt32 retainedHeadroom = static_cast<UInt32>(
					desiredCapacity - requiredCapacity);
				gLog.FormattedMessage(
					"tnvse_freetype_native: static vertex buffer rebuilt capacity=%u bytes=%u liveVertices=%u livePayloads=%u requestedVertices=%u mode=%s reserveVertices=%u copiedBytes=%u",
					state.staticVertexCapacity,
					state.staticVertexCapacity * sizeof(NativeA8GpuVertex),
					state.nextStaticVertex,
					static_cast<UInt32>(state.staticPayloads.size()),
					requiredVertices,
					sameSizeCompaction ? "compact" : "grow",
					retainedHeadroom,
					state.nextStaticVertex
						* static_cast<UInt32>(sizeof(NativeA8GpuVertex)));
			}
			return true;
		}

		bool BindPacketAtlasPage(NativeA8Proxy& proxy,
			const NativeA8PayloadTemplate& artifact, UInt16 page)
		{
			if (page >= artifact.atlasProperties.size()
				|| page >= artifact.atlasTextures.size()
				|| !artifact.atlasProperties[page]
				|| !artifact.atlasTextures[page])
			{
				return false;
			}

			NiTexturingProperty* desiredProperty =
				artifact.atlasProperties[page].m_pObject;
			NiTexture* desiredTexture = artifact.atlasTextures[page].m_pObject;
			if (!proxy.shape || !proxy.tile)
				return false;
			if (proxy.atlasProperty != desiredProperty)
			{
				proxy.shape->RemoveProperty(NiProperty::TEXTURING);
				proxy.shape->AddProperty(desiredProperty);
				proxy.shape->UpdateProperties();
				proxy.atlasProperty = proxy.shape->GetTexturingProperty();
				if (proxy.atlasProperty != desiredProperty)
					return false;
			}
			if (proxy.atlasTexture != desiredTexture
				|| proxy.tile->sourceTexture.m_pObject != desiredTexture)
			{
				ThisStdCall(0xBB7A10, proxy.tile, desiredTexture);
				proxy.atlasTexture = proxy.tile->sourceTexture.m_pObject;
			}
			return proxy.atlasTexture == desiredTexture;
		}

		void CopyScissorTail(const NiTriShape& source, NiTriShape& destination)
		{
			std::memcpy(reinterpret_cast<UInt8*>(&destination) + kScissorTailOffset,
				reinterpret_cast<const UInt8*>(&source) + kScissorTailOffset,
				kScissorTailSize);
		}

		void CopyTileDynamicState(const TileShaderPropertyView& source,
			TileShaderPropertyView& destination)
		{
			destination.m_usFlags = source.m_usFlags;
			destination.ulFlags[0] = source.ulFlags[0];
			destination.ulFlags[1] = source.ulFlags[1];
			destination.fAlpha = source.fAlpha;
			destination.fFadeAlpha = source.fFadeAlpha;
			destination.fEnvMapScale = source.fEnvMapScale;
			destination.fLODFade = source.fLODFade;
			destination.fDepthBias = source.fDepthBias;
			destination.uiShaderIndex = source.uiShaderIndex;
			if (destination.alphaTexture.m_pObject != source.alphaTexture.m_pObject)
				destination.alphaTexture = source.alphaTexture;
			destination.overlayColor = source.overlayColor;
			destination.tileAlpha = source.tileAlpha;
			destination.textureTransform = source.textureTransform;
			destination.clampMode = source.clampMode;
			destination.byte90 = source.byte90;
			destination.rotates = source.rotates;
			destination.hasVertexColors = true;
			destination.noTexture = false;
			destination.scissorRect = source.scissorRect;
			destination.useScissorTest = source.useScissorTest;
			// sourceTexture and texturePath deliberately remain page-specific.
		}

		void ApplyRelativeOrigin(NiTransform& destination,
			const NiTransform& source, const NiPoint3& origin)
		{
			destination = source;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				destination.m_Translate = source * origin;
		}

		bool SyncProxyState(const NiTriShape& facade, NativeA8Proxy& proxyState,
			const NiPoint3& geometryOrigin)
		{
			const TileShaderPropertyView* sourceTile = GetTileProperty(&facade);
			const NiAlphaProperty* sourceAlpha = facade.GetAlphaProperty();
			NiTriShape* proxy = proxyState.shape.m_pObject;
			NiAlphaProperty* proxyAlpha = proxyState.alphaProperty.m_pObject;
			TileShaderPropertyView* proxyTile = proxyState.tile;
			if (!sourceTile || !proxyTile || !proxyTile->sourceTexture
				|| !sourceAlpha || !proxyAlpha || !proxy
				|| !proxyState.atlasProperty)
			{
				return false;
			}

			ApplyRelativeOrigin(proxy->m_kLocal, facade.m_kLocal, geometryOrigin);
			ApplyRelativeOrigin(proxy->m_kWorld, facade.m_kWorld, geometryOrigin);
			CopyScissorTail(facade, *proxy);

			if (facade.m_pWorldBound)
			{
				if (!proxy->m_pWorldBound)
					proxy->CreateWorldBoundIfMissing();
				if (!proxy->m_pWorldBound)
					return false;
				*proxy->m_pWorldBound = *facade.m_pWorldBound;
			}
			proxy->m_uiFlags = facade.m_uiFlags;
			// Keep the blend/sort contract but clear only alpha testing. Aliasing
			// the facade property lets the stock pass re-enable the threshold.
			proxyAlpha->m_usFlags = sourceAlpha->m_usFlags;
			proxyAlpha->m_ucAlphaTestRef = sourceAlpha->m_ucAlphaTestRef;
			proxyAlpha->SetAlphaTesting(false);
			if (proxy->m_kProperties.m_spAlphaProperty.m_pObject
				!= proxyAlpha)
				proxy->m_kProperties.m_spAlphaProperty = proxyAlpha;
			if (proxy->m_kProperties.m_spCullingProperty.m_pObject
				!= facade.m_kProperties.m_spCullingProperty.m_pObject)
				proxy->m_kProperties.m_spCullingProperty =
					facade.m_kProperties.m_spCullingProperty;
			if (proxy->m_kProperties.m_spMaterialProperty.m_pObject
				!= facade.m_kProperties.m_spMaterialProperty.m_pObject)
				proxy->m_kProperties.m_spMaterialProperty =
					facade.m_kProperties.m_spMaterialProperty;
			if (proxy->m_kProperties.m_spStencilProperty.m_pObject
				!= facade.m_kProperties.m_spStencilProperty.m_pObject)
				proxy->m_kProperties.m_spStencilProperty =
					facade.m_kProperties.m_spStencilProperty;
			if (proxy->m_kProperties.m_spUnknownProperty.m_pObject
				!= facade.m_kProperties.m_spUnknownProperty.m_pObject)
				proxy->m_kProperties.m_spUnknownProperty =
					facade.m_kProperties.m_spUnknownProperty;
			CopyTileDynamicState(*sourceTile, *proxyTile);
			return true;
		}

		UInt32 AcquireProxyLocked(NativeA8RingState& state,
			NativeA8RingThreadState& thread)
		{
			if (thread.preferredProxy < state.proxyCount
				&& !state.proxies[thread.preferredProxy].inUse)
			{
				state.proxies[thread.preferredProxy].inUse = true;
				return thread.preferredProxy;
			}
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (!state.proxies[index].inUse)
				{
					state.proxies[index].inUse = true;
					thread.preferredProxy = index;
					return index;
				}
			}
			return std::numeric_limits<UInt32>::max();
		}

		void MarkStaticPayloadUsedLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate)
		{
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			const UInt32 frame = GetStaticObservationFrame(state);
			if (residency.staticLastUsedFrame != frame)
				residency.staticLastUsedFrame = frame;
		}

		bool ResolveStaticPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			if (!state.staticVertexBuffer)
				return false;
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			if (residency.staticResourceSerial == resourceSerial
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex)
			{
				baseVertex = residency.staticBaseVertex;
				MarkStaticPayloadUsedLocked(state, payloadTemplate);
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectStaticResidencyHit);
				return true;
			}
			NativeA8StaticHotEntry& hot = s_ringThread.staticPayload;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial)
			{
				const NativeA8PayloadTemplatePtr hotOwner = hot.owner.lock();
				if (hotOwner.get() == payloadTemplate.get()
					&& residency.staticResourceSerial == resourceSerial
					&& residency.staticBaseVertex == hot.baseVertex
					&& residency.staticVertexCount == hot.vertexCount
					&& hot.vertexCount == vertexCount
					&& hot.baseVertex <= state.staticVertexCapacity
					&& vertexCount <= state.staticVertexCapacity - hot.baseVertex)
				{
					baseVertex = hot.baseVertex;
					MarkStaticPayloadUsedLocked(state, payloadTemplate);
					RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
					return true;
				}
				hot = {};
			}
			auto found = state.staticPayloads.find(payloadTemplate.get());
			if (found == state.staticPayloads.end())
				return false;
			const std::shared_ptr<const NativeA8PayloadTemplate> owner =
				found->second.owner.lock();
			if (owner.get() == payloadTemplate.get()
				&& found->second.vertexCount == vertexCount
				&& found->second.baseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- found->second.baseVertex)
			{
				baseVertex = found->second.baseVertex;
				hot.key = payloadTemplate.get();
				hot.owner = owner;
				hot.baseVertex = found->second.baseVertex;
				hot.vertexCount = found->second.vertexCount;
				hot.resourceSerial = resourceSerial;
				residency.staticResourceSerial = resourceSerial;
				residency.staticBaseVertex = found->second.baseVertex;
				residency.staticVertexCount = found->second.vertexCount;
				MarkStaticPayloadUsedLocked(state, payloadTemplate);
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				return true;
			}
			state.staticPayloads.erase(found);
			RefreshRingCpuMemoryLocked(state);
			return false;
		}

		bool IsStaticPayloadCurrentLocked(const NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			if (!state.staticVertexBuffer || !payloadTemplate)
				return false;
			const NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			return residency.staticResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex;
		}

		NativeA8StaticCandidate* ResolveStaticCandidateLocked(
			NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, bool allowCreate)
		{
			// Candidate observation must use the strategy's growth ceiling rather
			// than the current allocation. Otherwise a payload larger than the
			// initial 4 MiB buffer can never mature and therefore can never trigger
			// the compaction/growth path that was designed to accommodate it.
			if (!state.staticVertexBuffer || vertexCount > kStaticTargetVertexCapacity)
				return nullptr;
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeA8CandidateHotEntry& hot = s_ringThread.staticCandidate;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial && hot.candidate)
			{
				const NativeA8PayloadTemplatePtr hotOwner = hot.owner.lock();
				if (hotOwner.get() == payloadTemplate.get())
					return hot.candidate.get();
				hot = {};
			}
			bool memoryChanged = false;
			auto found = state.staticCandidates.find(payloadTemplate.get());
			if (found == state.staticCandidates.end())
			{
				if (!allowCreate)
					return nullptr;
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					const UInt32 frame = GetStaticObservationFrame(state);
					if (!state.staticCandidateSweepFrameValid
						|| static_cast<UInt32>(frame
							- state.lastStaticCandidateSweepFrame)
							>= kStaticCandidateSweepIntervalFrames)
					{
						state.lastStaticCandidateSweepFrame = frame;
						state.staticCandidateSweepFrameValid = true;
						s_ringThread.staticCandidate = {};
						for (auto current = state.staticCandidates.begin();
							current != state.staticCandidates.end();)
						{
							const bool inactive = current->second
								&& current->second->observationFrameValid
								&& static_cast<UInt32>(frame
									- current->second->lastObservedFrame)
									> kStaticCandidateInactiveFrames;
							if (!current->second
								|| current->second->owner.expired() || inactive)
							{
								current = state.staticCandidates.erase(current);
								memoryChanged = true;
							}
							else
								++current;
						}
					}
					if (state.staticCandidates.size() >= kStaticCandidateLimit)
					{
						if (memoryChanged)
							RefreshRingCpuMemoryLocked(state);
						return nullptr;
					}
				}
				auto candidate = CreateStaticCandidate(payloadTemplate);
				found = state.staticCandidates.emplace(
					payloadTemplate.get(), std::move(candidate)).first;
				memoryChanged = true;
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					state.lastStaticCandidateSweepFrame =
						GetStaticObservationFrame(state);
					state.staticCandidateSweepFrameValid = true;
				}
			}
			else
			{
				const std::shared_ptr<const NativeA8PayloadTemplate> owner =
					found->second->owner.lock();
				if (owner.get() != payloadTemplate.get())
				{
					found->second = CreateStaticCandidate(payloadTemplate);
					memoryChanged = true;
				}
			}
			if (memoryChanged)
				RefreshRingCpuMemoryLocked(state);
			hot.key = payloadTemplate.get();
			hot.owner = payloadTemplate;
			hot.candidate = found->second;
			hot.resourceSerial = resourceSerial;
			return hot.candidate.get();
		}

		NativeA8StaticCandidate* ObserveStaticCandidateLocked(
			NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, bool allowCreate)
		{
			NativeA8StaticCandidate* candidate = ResolveStaticCandidateLocked(
				state, payloadTemplate, vertexCount, allowCreate);
			if (!candidate || candidate->promotionDisabled)
				return candidate;

			const UInt32 frame = GetStaticObservationFrame(state);
			if (!candidate->observationFrameValid)
			{
				candidate->firstObservedFrame = frame;
				candidate->lastObservedFrame = frame;
				candidate->activeObservedFrames = 1;
				candidate->observationFrameValid = true;
			}
			else if (candidate->lastObservedFrame != frame)
			{
				const UInt32 gap = static_cast<UInt32>(
					frame - candidate->lastObservedFrame);
				if (gap > kStaticPromotionMaximumFrameGap)
				{
					candidate->firstObservedFrame = frame;
					candidate->activeObservedFrames = 1;
					candidate->dynamicUploadEpochCount = 0;
					candidate->lastDynamicUploadEpoch = 0;
					candidate->lastDynamicUploadResourceSerial = 0;
				}
				else if (candidate->activeObservedFrames
					< std::numeric_limits<UInt32>::max())
				{
					++candidate->activeObservedFrames;
				}
				candidate->lastObservedFrame = frame;
			}
			return candidate;
		}

		void NoteStaticCandidateDynamicUploadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			NativeA8StaticCandidate* candidate = ObserveStaticCandidateLocked(
				state, payloadTemplate, vertexCount, true);
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			if (!candidate || candidate->promotionDisabled
				|| (candidate->lastDynamicUploadEpoch == state.uploadEpoch
					&& candidate->lastDynamicUploadResourceSerial
						== resourceSerial))
			{
				return;
			}
			candidate->lastDynamicUploadEpoch = state.uploadEpoch;
			candidate->lastDynamicUploadResourceSerial = resourceSerial;
			if (candidate->dynamicUploadEpochCount
				< std::numeric_limits<UInt32>::max())
			{
				++candidate->dynamicUploadEpochCount;
			}
		}

		void RecordStaticPromotionDeferral(StaticPromotionReadiness readiness,
			UInt64 amount = 1)
		{
			if (!amount)
				return;
			switch (readiness)
			{
			case StaticPromotionReadiness::Lifecycle:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredLifecycle,
					amount);
				break;
			case StaticPromotionReadiness::UploadHistory:
				RecordFreeTypePerf(FreeTypePerfCounter::
					StaticPromotionDeferredUploadHistory, amount);
				break;
			case StaticPromotionReadiness::Retry:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredRetry,
					amount);
				break;
			default:
				break;
			}
		}

		bool PromoteStaticPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			NativeA8StaticCandidate* candidate =
				ObserveStaticCandidateLocked(
					state, payloadTemplate, vertexCount, false);
			if (!candidate)
			{
				return false;
			}
			const UInt32 frame = GetStaticObservationFrame(state);
			const StaticPromotionReadiness readiness =
				GetStaticPromotionReadiness(state, *candidate,
					vertexCount, frame);
			if (readiness != StaticPromotionReadiness::Ready)
			{
				RecordStaticPromotionDeferral(readiness);
				return false;
			}
			const UInt32 byteCount = vertexCount * sizeof(NativeA8GpuVertex);
			if (!FitsStaticPromotionBudget(state, *candidate,
				vertexCount, 0, 0))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredBudget);
				return false;
			}

			if (state.nextStaticVertex > state.staticVertexCapacity
				|| vertexCount > state.staticVertexCapacity
					- state.nextStaticVertex)
			{
				bool permanentFailure = false;
				if (!TryGrowStaticVertexBufferLocked(state, vertexCount,
					permanentFailure))
				{
					candidate->promotionDisabled = permanentFailure;
					if (!permanentFailure)
					{
						candidate->nextRetryFrame = frame
							+ kStaticPromotionRetryFrames;
						DeferStaticPromotionsLocked(state, frame);
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
					return false;
				}
			}

			baseVertex = state.nextStaticVertex;
			const UINT byteOffset = baseVertex * sizeof(NativeA8GpuVertex);
			void* destination = nullptr;
			HRESULT result = state.staticVertexBuffer->Lock(byteOffset, byteCount,
				&destination, 0);
			if (FAILED(result) || !destination)
			{
				candidate->nextRetryFrame = frame
					+ kStaticPromotionRetryFrames;
				DeferStaticPromotionsLocked(state, frame);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}
			std::memcpy(destination, payloadTemplate->gpuVertices.data(), byteCount);
			result = state.staticVertexBuffer->Unlock();
			if (FAILED(result))
			{
				state.nextStaticVertex = baseVertex + vertexCount;
				CommitStaticPromotionBudget(state, byteCount, 1);
				candidate->nextRetryFrame = frame
					+ kStaticPromotionRetryFrames;
				DeferStaticPromotionsLocked(state, frame);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}

			AdvanceDiagnosticSerial(state.staticWriteSerial);
			state.nextStaticVertex = baseVertex + vertexCount;
			state.staticPayloads[payloadTemplate.get()] = {
				payloadTemplate, baseVertex, vertexCount,
				state.staticWriteSerial,
				HashDiagnosticPayload(*payloadTemplate) };
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.staticResourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			residency.staticBaseVertex = baseVertex;
			residency.staticVertexCount = vertexCount;
			residency.staticLastUsedFrame = frame;
			state.staticCandidates.erase(payloadTemplate.get());
			s_ringThread.staticPayload = {
				payloadTemplate.get(), payloadTemplate, baseVertex, vertexCount,
				state.resourceSerial.load(std::memory_order_relaxed) };
			if (s_ringThread.staticCandidate.key == payloadTemplate.get())
				s_ringThread.staticCandidate = {};
			RefreshRingCpuMemoryLocked(state);
			CommitStaticPromotionBudget(state, byteCount, 1);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUploadBytes,
				byteCount);
			return true;
		}

		bool ResolveUploadedPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			if (residency.dynamicResourceSerial == resourceSerial
				&& residency.dynamicUploadEpoch == state.uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex)
			{
				baseVertex = residency.dynamicBaseVertex;
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectDynamicResidencyHit);
				return true;
			}

			NativeA8UploadHotEntry& hot = s_ringThread.uploadedPayload;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial
				&& hot.epoch == state.uploadEpoch)
			{
				if (hot.owner.get() == payloadTemplate.get()
					&& hot.vertexCount == vertexCount
					&& hot.baseVertex <= state.vertexCapacity
					&& vertexCount <= state.vertexCapacity - hot.baseVertex)
				{
					baseVertex = hot.baseVertex;
					residency.dynamicResourceSerial = resourceSerial;
					residency.dynamicUploadEpoch = state.uploadEpoch;
					residency.dynamicBaseVertex = hot.baseVertex;
					residency.dynamicVertexCount = hot.vertexCount;
					RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
					return true;
				}
				hot = {};
			}

			auto uploaded = state.uploadedPayloads.find(payloadTemplate.get());
			if (uploaded == state.uploadedPayloads.end())
				return false;
			const std::shared_ptr<const NativeA8PayloadTemplate> owner =
				uploaded->second.owner.lock();
			if (owner.get() == payloadTemplate.get()
				&& uploaded->second.epoch == state.uploadEpoch
				&& uploaded->second.vertexCount == vertexCount
				&& uploaded->second.baseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- uploaded->second.baseVertex)
			{
				baseVertex = uploaded->second.baseVertex;
				hot = {
					payloadTemplate.get(), owner,
					uploaded->second.baseVertex, uploaded->second.vertexCount,
					uploaded->second.epoch, resourceSerial };
				residency.dynamicResourceSerial = resourceSerial;
				residency.dynamicUploadEpoch = state.uploadEpoch;
				residency.dynamicBaseVertex = uploaded->second.baseVertex;
				residency.dynamicVertexCount = uploaded->second.vertexCount;
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
				return true;
			}
			state.uploadedPayloads.erase(uploaded);
			RefreshRingCpuMemoryLocked(state);
			return false;
		}

		bool HasDirectStaticPayloadLocked(const NativeA8RingState& state,
			const NativeA8PayloadTemplate& payloadTemplate,
			UInt32 vertexCount)
		{
			const NativeA8PayloadResidencyCache& residency =
				payloadTemplate.residency;
			return residency.staticResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex;
		}

		bool HasDirectUploadedPayloadLocked(const NativeA8RingState& state,
			const NativeA8PayloadTemplate& payloadTemplate,
			UInt32 vertexCount)
		{
			const NativeA8PayloadResidencyCache& residency =
				payloadTemplate.residency;
			return residency.dynamicResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.dynamicUploadEpoch == state.uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex;
		}

		void PublishUploadedPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 baseVertex, UInt32 vertexCount)
		{
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			state.uploadedPayloads[payloadTemplate.get()] = {
				payloadTemplate, baseVertex, vertexCount, state.uploadEpoch,
				state.dynamicWriteSerial, state.dynamicDiscardSerial,
				HashDiagnosticPayload(*payloadTemplate) };
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.dynamicResourceSerial = resourceSerial;
			residency.dynamicUploadEpoch = state.uploadEpoch;
			residency.dynamicBaseVertex = baseVertex;
			residency.dynamicVertexCount = vertexCount;
			NoteStaticCandidateDynamicUploadLocked(state, payloadTemplate,
				vertexCount);
			s_ringThread.uploadedPayload = {
				payloadTemplate.get(), payloadTemplate, baseVertex, vertexCount,
				state.uploadEpoch, resourceSerial };
		}

		bool ResolveSortedLeaseResidency(
			const NativeA8RingState& state,
			const NativeA8PayloadTemplate& artifact, UInt32 vertexCount,
			UInt32 resourceSerial, UInt32 uploadEpoch,
			UInt32& baseVertex, bool& staticResident)
		{
			NativeA8PayloadResidencyCache& residency =
				artifact.residency;
			if (state.staticVertexBuffer
				&& residency.staticResourceSerial == resourceSerial
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex)
			{
				baseVertex = residency.staticBaseVertex;
				staticResident = true;
				return true;
			}
			if (state.vertexBuffer
				&& residency.dynamicResourceSerial == resourceSerial
				&& residency.dynamicUploadEpoch == uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex)
			{
				baseVertex = residency.dynamicBaseVertex;
				staticResident = false;
				return true;
			}
			return false;
		}

		bool PublishSortedRingLeaseLocked(NativeA8RingState& state,
			const std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates,
			UInt32 generation, bool residencyAlreadyValidated = false)
		{
			if (s_sortedRingLease.active || !generation
				|| state.generation != generation || !state.vertexBuffer
				|| !state.indexBuffer || !state.declaration
				|| state.releasePending.load(std::memory_order_acquire)
				|| state.sortedFrameLeases.load(std::memory_order_acquire)
				|| state.activeSubmissions.load(std::memory_order_acquire))
			{
				return false;
			}
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_acquire);
			const UInt32 uploadEpoch = state.uploadEpoch;
			if (!residencyAlreadyValidated)
			{
				for (const NativeA8PayloadTemplatePtr& payloadTemplate
					: payloadTemplates)
				{
					if (!payloadTemplate || payloadTemplate->gpuVertices.empty()
						|| payloadTemplate->gpuVertices.size()
							> std::numeric_limits<UInt32>::max())
					{
						return false;
					}
					const UInt32 vertexCount = static_cast<UInt32>(
						payloadTemplate->gpuVertices.size());
					UInt32 baseVertex = 0;
					bool staticResident = false;
					if (!ResolveSortedLeaseResidency(state, *payloadTemplate,
						vertexCount, resourceSerial, uploadEpoch,
						baseVertex, staticResident))
					{
						return false;
					}
				}
			}
			RefreshRingCpuMemoryLocked(state);
			state.sortedFrameLeases.fetch_add(1,
				std::memory_order_release);
			s_sortedRingLease.state = &state;
			s_sortedRingLease.dynamicVertexBuffer = state.vertexBuffer;
			s_sortedRingLease.staticVertexBuffer = state.staticVertexBuffer;
			s_sortedRingLease.indexBuffer = state.indexBuffer;
			s_sortedRingLease.declaration = state.declaration;
			s_sortedRingLease.generation = generation;
			s_sortedRingLease.resourceSerial = resourceSerial;
			s_sortedRingLease.uploadEpoch = uploadEpoch;
			s_sortedRingLease.active = true;
			return true;
		}

		bool TryBeginSortedRingSubmission(NiTriShape* facade,
			NativeA8ShapePayload& payload,
			NativeA8RingSubmission& submission,
			NativeA8FallbackReason& result)
		{
			if (!s_sortedRingLease.active)
				return false;
			result = NativeA8FallbackReason::RuntimeFault;
			NativeA8SortedRingLease& lease = s_sortedRingLease;
			NativeA8RingState* state = lease.state;
			if (!state || !facade || !payload.payloadTemplate
				|| payload.preparedGeneration != lease.generation
				|| !IsNativeA8ShaderGenerationCurrent(lease.generation)
				|| state->generation != lease.generation
				|| state->resourceSerial.load(std::memory_order_acquire)
					!= lease.resourceSerial
				|| state->uploadEpoch != lease.uploadEpoch
				|| state->vertexBuffer != lease.dynamicVertexBuffer
				|| state->staticVertexBuffer != lease.staticVertexBuffer
				|| state->indexBuffer != lease.indexBuffer
				|| state->declaration != lease.declaration)
			{
				return true;
			}
			const UInt64 vertexCount64 =
				payload.payloadTemplate->gpuVertices.size();
			if (!vertexCount64
				|| vertexCount64 > std::numeric_limits<UInt32>::max())
			{
				result = NativeA8FallbackReason::PacketBuild;
				return true;
			}
			const UInt32 vertexCount = static_cast<UInt32>(vertexCount64);
			UInt32 baseVertex = 0;
			bool staticResident = false;
			if (!ResolveSortedLeaseResidency(*state,
				*payload.payloadTemplate, vertexCount,
				lease.resourceSerial, lease.uploadEpoch,
				baseVertex, staticResident))
			{
				result = NativeA8FallbackReason::PacketPrepare;
				return true;
			}

			const UInt32 proxyIndex =
				AcquireProxyLocked(*state, s_ringThread);
			if (proxyIndex == std::numeric_limits<UInt32>::max())
			{
				payload.packetPrepareFailure.store(
					NativeA8PacketPrepareFailure::ProxyUnavailable,
					std::memory_order_relaxed);
				result = NativeA8FallbackReason::PacketPrepare;
				return true;
			}
			NativeA8Proxy& proxy = state->proxies[proxyIndex];
			if (!proxy.shape || !proxy.buffer || !proxy.chip
				|| !SyncProxyState(*facade, proxy,
					payload.geometryOrigin))
			{
				proxy.inUse = false;
				payload.packetPrepareFailure.store(
					NativeA8PacketPrepareFailure::Geometry,
					std::memory_order_relaxed);
				result = NativeA8FallbackReason::PropertySync;
				return true;
			}

			submission.proxyShape = proxy.shape.m_pObject;
			submission.proxyBuffer = proxy.buffer;
			submission.proxyChip = proxy.chip;
			submission.vertexBuffer = staticResident
				? state->staticVertexBuffer : state->vertexBuffer;
			submission.proxyIndex = proxyIndex;
			submission.generation = lease.generation;
			submission.resourceSerial = lease.resourceSerial;
			submission.nextPacket = 0;
			submission.payloadBaseVertex = baseVertex;
			submission.endVertex = baseVertex + vertexCount;
			submission.staticResident = staticResident;
			submission.active = true;
			state->activeSubmissions.fetch_add(1, std::memory_order_release);
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None,
				std::memory_order_relaxed);
			result = NativeA8FallbackReason::None;
			return true;
		}
	}

	bool EnsureNativeA8ProxyPool(Font& font)
	{
		NativeA8RingState& state = RingState();
		if (state.proxyPoolReady.load(std::memory_order_acquire))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeRegistrationProxyFast);
			return true;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::NativeRegistrationProxySlow);
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.proxyPoolReady.load(std::memory_order_relaxed))
			return true;
		const NiColorA white{ 1.0f, 1.0f, 1.0f, 1.0f };
		while (state.proxyCount < kProxyPoolSize)
		{
			NativeA8Proxy proxy;
			proxy.shape = font.MakeTriShape(1, &white, false);
			NiTriShapeData* data = proxy.shape
				? proxy.shape->GetModelData() : nullptr;
			if (!data || !GetTileProperty(proxy.shape.m_pObject)
				|| !InstallProxyVertexColors(*data) || !AttachProxyBuffer(proxy))
			{
				break;
			}
			proxy.shape->UpdateProperties();
			proxy.alphaProperty = proxy.shape->GetAlphaProperty();
			proxy.tile = GetTileProperty(proxy.shape.m_pObject);
			proxy.atlasProperty = proxy.shape->GetTexturingProperty();
			proxy.atlasTexture = proxy.tile
				? proxy.tile->sourceTexture.m_pObject : nullptr;
			proxy.shader = proxy.shape->GetShader();
			if (!proxy.alphaProperty || !proxy.tile || !proxy.atlasProperty
				|| !proxy.atlasTexture)
				break;
			proxy.alphaProperty->SetAlphaTesting(false);
			state.proxies[state.proxyCount++] = std::move(proxy);
		}
		if (state.proxyCount == kProxyPoolSize)
			state.proxyPoolReady.store(true, std::memory_order_release);
		return state.proxyCount != 0;
	}

	void TrimNativeA8CpuCachesForTotalBudget()
	{
		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		state.uploadedPayloads.clear();
		state.staticCandidates.clear();
		for (auto current = state.staticPayloads.begin();
			current != state.staticPayloads.end();)
		{
			if (current->second.owner.expired())
				current = state.staticPayloads.erase(current);
			else
				++current;
		}
		s_ringThread.uploadedPayload = {};
		s_ringThread.staticCandidate = {};
		RefreshRingCpuMemoryLocked(state);
	}

	void PrepareSortedNativeA8Payloads(
		std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates,
		UInt32 generation)
	{
		EndNativeA8SortedRingFrame();
		if (!generation || payloadTemplates.empty()
			|| !IsNativeA8ShaderGenerationCurrent(generation))
		{
			return;
		}

		UInt32 maximumVertices = 0;
		for (const NativeA8PayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!payloadTemplate || payloadTemplate->gpuVertices.empty()
				|| payloadTemplate->gpuVertices.size()
					> std::numeric_limits<UInt32>::max())
			{
				continue;
			}
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			if ((vertexCount & 3u) || vertexCount / 4u > kNativeA8MaximumQuads)
				continue;
			maximumVertices = std::max(maximumVertices, vertexCount);
		}
		if (!maximumVertices)
			return;

		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const char* operation = "sorted-frame-resource";
		HRESULT result = D3DERR_DEVICELOST;
		if (!EnsureRingResourcesLocked(state, generation, maximumVertices,
			operation, result))
		{
			return;
		}
		auto isValidPayload = [](
			const NativeA8PayloadTemplatePtr& payloadTemplate)
		{
			return payloadTemplate
				&& !payloadTemplate->gpuVertices.empty()
				&& payloadTemplate->gpuVertices.size()
					<= std::numeric_limits<UInt32>::max()
				&& !(payloadTemplate->gpuVertices.size() & 3u)
				&& payloadTemplate->gpuVertices.size() / 4u
					<= kNativeA8MaximumQuads;
		};
		size_t validatedStaticPayloads = 0;
		size_t residentStaticPayloads = 0;
		const UInt32 staticScanResourceSerial = state.resourceSerial.load(
			std::memory_order_relaxed);
		if (state.staticVertexBuffer)
		{
			std::vector<NativeA8PayloadTemplatePtr> selected;
			selected.reserve(std::min<size_t>(payloadTemplates.size(),
				kStaticPromotionPayloadLimit));
			CpuMemoryLease selectedCpuMemory;
			selectedCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				selected.capacity() * sizeof(NativeA8PayloadTemplatePtr));
			UInt32 requestedVertices = 0;
			UInt32 requestedBytes = 0;
			UInt32 requestedPayloads = 0;
			UInt64 lifecycleDeferred = 0;
			UInt64 uploadDeferred = 0;
			UInt64 retryDeferred = 0;
			UInt64 budgetDeferred = 0;
			const UInt32 frame = GetStaticObservationFrame(state);
			for (const NativeA8PayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				++validatedStaticPayloads;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				UInt32 baseVertex = 0;
				if (ResolveStaticPayloadLocked(state, payloadTemplate,
					vertexCount, baseVertex))
				{
					++residentStaticPayloads;
					continue;
				}
				NativeA8StaticCandidate* candidate =
					ObserveStaticCandidateLocked(state, payloadTemplate,
						vertexCount, false);
				if (!candidate)
					continue;
				const StaticPromotionReadiness readiness =
					GetStaticPromotionReadiness(state, *candidate,
						vertexCount, frame);
				if (readiness != StaticPromotionReadiness::Ready)
				{
					switch (readiness)
					{
					case StaticPromotionReadiness::Lifecycle:
						++lifecycleDeferred;
						break;
					case StaticPromotionReadiness::UploadHistory:
						++uploadDeferred;
						break;
					case StaticPromotionReadiness::Retry:
						++retryDeferred;
						break;
					default:
						break;
					}
					continue;
				}
				if (!FitsStaticPromotionBudget(state, *candidate,
					vertexCount, requestedBytes, requestedPayloads))
				{
					++budgetDeferred;
					continue;
				}
				requestedVertices += vertexCount;
				requestedBytes += vertexCount * sizeof(NativeA8GpuVertex);
				++requestedPayloads;
				selected.push_back(payloadTemplate);
			}
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::Lifecycle, lifecycleDeferred);
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::UploadHistory, uploadDeferred);
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::Retry, retryDeferred);
			if (budgetDeferred)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredBudget,
					budgetDeferred);
			}

			const UInt32 availableVertices = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			if (requestedVertices > availableVertices
				&& requestedVertices)
			{
				bool permanentFailure = false;
				if (!TryGrowStaticVertexBufferLocked(state,
					requestedVertices, permanentFailure)
					&& !permanentFailure)
				{
					DeferStaticPromotionsLocked(state, frame);
				}
			}

			const UInt32 staticAvailable = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			auto deferSelectedCandidate = [&](
				const NativeA8PayloadTemplatePtr& payloadTemplate)
			{
				auto found = state.staticCandidates.find(payloadTemplate.get());
				if (found != state.staticCandidates.end() && found->second)
				{
					found->second->nextRetryFrame = frame
						+ kStaticPromotionRetryFrames;
				}
			};
			UInt32 selectedVertices = 0;
			size_t selectedPayloads = 0;
			for (size_t index = 0; index < selected.size(); ++index)
			{
				const NativeA8PayloadTemplatePtr& payloadTemplate = selected[index];
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				if (vertexCount > staticAvailable - selectedVertices)
				{
					deferSelectedCandidate(payloadTemplate);
					continue;
				}
				selected[selectedPayloads] = payloadTemplate;
				selectedVertices += vertexCount;
				++selectedPayloads;
			}
			selected.resize(selectedPayloads);

			if (selectedPayloads && selectedVertices)
			{
				state.staticPayloads.reserve(
					state.staticPayloads.size() + selectedPayloads);
				const UINT byteOffset = state.nextStaticVertex
					* sizeof(NativeA8GpuVertex);
				const UINT byteCount =
					selectedVertices * sizeof(NativeA8GpuVertex);
				void* destination = nullptr;
				result = state.staticVertexBuffer->Lock(byteOffset, byteCount,
					&destination, 0);
				if (SUCCEEDED(result) && destination)
				{
					UInt32 copiedVertices = 0;
					for (const NativeA8PayloadTemplatePtr& payloadTemplate : selected)
					{
						const UInt32 vertexCount = static_cast<UInt32>(
							payloadTemplate->gpuVertices.size());
						std::memcpy(static_cast<UInt8*>(destination)
								+ copiedVertices
									* sizeof(NativeA8GpuVertex),
							payloadTemplate->gpuVertices.data(),
							vertexCount * sizeof(NativeA8GpuVertex));
						copiedVertices += vertexCount;
					}
					result = state.staticVertexBuffer->Unlock();
					if (copiedVertices == selectedVertices
						&& SUCCEEDED(result))
					{
						AdvanceDiagnosticSerial(state.staticWriteSerial);
						const UInt32 resourceSerial =
							state.resourceSerial.load(
								std::memory_order_relaxed);
						UInt32 mappedVertices = 0;
						for (const NativeA8PayloadTemplatePtr& payloadTemplate
							: selected)
						{
							const UInt32 vertexCount = static_cast<UInt32>(
								payloadTemplate->gpuVertices.size());
							const UInt32 baseVertex =
								state.nextStaticVertex + mappedVertices;
							state.staticPayloads[payloadTemplate.get()] = {
								payloadTemplate, baseVertex, vertexCount,
								state.staticWriteSerial,
								HashDiagnosticPayload(*payloadTemplate) };
							NativeA8PayloadResidencyCache& residency =
								payloadTemplate->residency;
							residency.staticResourceSerial = resourceSerial;
							residency.staticBaseVertex = baseVertex;
							residency.staticVertexCount = vertexCount;
							residency.staticLastUsedFrame = frame;
							state.staticCandidates.erase(payloadTemplate.get());
							if (s_ringThread.staticCandidate.key
								== payloadTemplate.get())
							{
								s_ringThread.staticCandidate = {};
							}
							mappedVertices += vertexCount;
						}
						state.nextStaticVertex += selectedVertices;
						residentStaticPayloads += selectedPayloads;
						CommitStaticPromotionBudget(state, byteCount,
							static_cast<UInt32>(selectedPayloads));
						RecordFreeTypePerf(
							FreeTypePerfCounter::StaticVertexUpload);
						RecordFreeTypePerf(
							FreeTypePerfCounter::StaticVertexUploadBytes,
							byteCount);
						RecordFreeTypePerf(
							FreeTypePerfCounter::SortedStaticBatch);
						RecordFreeTypePerf(
							FreeTypePerfCounter::SortedStaticPayload,
							static_cast<UInt64>(selectedPayloads));
						RecordFreeTypePerf(
							FreeTypePerfCounter::SortedStaticBytes,
							byteCount);
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							FreeTypeFontDebugLog(
								"tnvse_freetype_native: sorted static batch generation=%u payloads=%u vertices=%u bytes=%u residentVertices=%u capacity=%u",
								generation,
								static_cast<UInt32>(selectedPayloads),
								selectedVertices, byteCount,
								state.nextStaticVertex,
								state.staticVertexCapacity);
						}
					}
					else
					{
						// The interval was written but was not published. Reserve it
						// so no later append can overwrite geometry that might have
						// been partially accepted by the driver.
						state.nextStaticVertex += selectedVertices;
						CommitStaticPromotionBudget(state, byteCount,
							static_cast<UInt32>(selectedPayloads));
						for (const NativeA8PayloadTemplatePtr& payloadTemplate
							: selected)
						{
							deferSelectedCandidate(payloadTemplate);
						}
						DeferStaticPromotionsLocked(state, frame);
						RecordFreeTypePerf(
							FreeTypePerfCounter::StaticVertexPromotionFailed);
					}
				}
				else
				{
					for (const NativeA8PayloadTemplatePtr& payloadTemplate
						: selected)
					{
						deferSelectedCandidate(payloadTemplate);
					}
					DeferStaticPromotionsLocked(state, frame);
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
				}
			}
		}
		if (g_bEnableFreeTypeFontStructuralFastPaths
			&& validatedStaticPayloads == payloadTemplates.size()
			&& residentStaticPayloads == validatedStaticPayloads
			&& state.resourceSerial.load(std::memory_order_relaxed)
				== staticScanResourceSerial
			&& PublishSortedRingLeaseLocked(
				state, payloadTemplates, generation, true))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::SortedAllStaticFastExit);
			RecordFreeTypePerf(FreeTypePerfCounter::
				SortedAllStaticPayloadValidationElided,
				static_cast<UInt64>(payloadTemplates.size()));
			return;
		}

		RefreshRingCpuMemoryLocked(state);

		// Resolve existing locations first. If an append cannot fit, rebatch every
		// currently visible non-static payload under one DISCARD so no earlier
		// location survives an epoch change and no shape performs its own lock.
		UInt64 allDynamicVertices = 0;
		UInt64 missingDynamicVertices = 0;
		size_t allDynamicPayloads = 0;
		size_t missingDynamicPayloads = 0;
		for (const NativeA8PayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!isValidPayload(payloadTemplate))
				continue;
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			UInt32 baseVertex = 0;
			const bool staticResident =
				g_bEnableFreeTypeFontStructuralFastPaths
					? IsStaticPayloadCurrentLocked(
						state, payloadTemplate, vertexCount)
					: ResolveStaticPayloadLocked(state, payloadTemplate,
						vertexCount, baseVertex);
			if (staticResident)
			{
				continue;
			}
			allDynamicVertices += vertexCount;
			++allDynamicPayloads;
			if (!ResolveUploadedPayloadLocked(state, payloadTemplate,
				vertexCount, baseVertex))
			{
				missingDynamicVertices += vertexCount;
				++missingDynamicPayloads;
			}
		}
		if (!missingDynamicVertices)
		{
			PublishSortedRingLeaseLocked(state, payloadTemplates,
				generation);
			return;
		}

		const UInt64 appendCapacity = state.nextVertex <= state.vertexCapacity
			? state.vertexCapacity - state.nextVertex : 0;
		const bool discard = !state.nextVertex
			|| missingDynamicVertices > appendCapacity;
		const UInt64 uploadVertices64 = discard
			? allDynamicVertices : missingDynamicVertices;
		const size_t uploadPayloads = discard
			? allDynamicPayloads : missingDynamicPayloads;
		if (!uploadVertices64 || uploadVertices64 > state.vertexCapacity
			|| uploadVertices64 > std::numeric_limits<UInt32>::max())
		{
			return;
		}
		if (discard
			&& state.activeSubmissions.load(std::memory_order_acquire))
			return;

		UInt32 startVertex = state.nextVertex;
		DWORD lockFlags = D3DLOCK_NOOVERWRITE;
		if (discard)
		{
			startVertex = 0;
			lockFlags = D3DLOCK_DISCARD;
			AdvanceUploadEpochLocked(state);
			AdvanceDiagnosticSerial(state.dynamicDiscardSerial);
			state.uploadedPayloads.clear();
			s_ringThread.uploadedPayload = {};
			state.nextVertex = 0;
			RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexDiscard);
		}

		const UInt32 uploadVertices = static_cast<UInt32>(uploadVertices64);
		state.uploadedPayloads.reserve(
			state.uploadedPayloads.size() + uploadPayloads);
		const UINT byteOffset =
			startVertex * sizeof(NativeA8GpuVertex);
		const UINT byteCount =
			uploadVertices * sizeof(NativeA8GpuVertex);
		void* destination = nullptr;
		result = state.vertexBuffer->Lock(byteOffset, byteCount,
			&destination, lockFlags);
		if (FAILED(result) || !destination)
			return;

		UInt32 copiedVertices = 0;
		for (const NativeA8PayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!isValidPayload(payloadTemplate))
				continue;
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			if (HasDirectStaticPayloadLocked(state, *payloadTemplate,
					vertexCount)
				|| (!discard && HasDirectUploadedPayloadLocked(state,
					*payloadTemplate, vertexCount)))
			{
				continue;
			}
			std::memcpy(static_cast<UInt8*>(destination)
					+ copiedVertices * sizeof(NativeA8GpuVertex),
				payloadTemplate->gpuVertices.data(),
				vertexCount * sizeof(NativeA8GpuVertex));
			copiedVertices += vertexCount;
		}
		result = state.vertexBuffer->Unlock();
		if (copiedVertices != uploadVertices || FAILED(result))
			return;
		AdvanceDiagnosticSerial(state.dynamicWriteSerial);

		UInt32 mappedVertices = 0;
		for (const NativeA8PayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!isValidPayload(payloadTemplate))
				continue;
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			if (HasDirectStaticPayloadLocked(state, *payloadTemplate,
					vertexCount)
				|| (!discard && HasDirectUploadedPayloadLocked(state,
					*payloadTemplate, vertexCount)))
			{
				continue;
			}
			PublishUploadedPayloadLocked(state, payloadTemplate,
				startVertex + mappedVertices, vertexCount);
			mappedVertices += vertexCount;
		}
		if (mappedVertices != uploadVertices)
		{
			// Published ranges are valid, but the complete written interval must
			// remain reserved before falling back to per-shape preparation.
			state.nextVertex = startVertex + uploadVertices;
			RefreshRingCpuMemoryLocked(state);
			PublishSortedRingLeaseLocked(state, payloadTemplates,
				generation);
			return;
		}

		state.nextVertex = startVertex + uploadVertices;
		RefreshRingCpuMemoryLocked(state);
		RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
		RecordFreeTypePerf(
			FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
		RecordFreeTypePerf(FreeTypePerfCounter::SortedDynamicBatch);
		RecordFreeTypePerf(FreeTypePerfCounter::SortedDynamicPayload,
			static_cast<UInt64>(uploadPayloads));
		RecordFreeTypePerf(
			FreeTypePerfCounter::SortedDynamicBytes, byteCount);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_native: sorted dynamic batch generation=%u payloads=%u vertices=%u bytes=%u discard=%u startVertex=%u endVertex=%u capacity=%u uploadEpoch=%u writeSerial=%u discardSerial=%u",
				generation, static_cast<UInt32>(uploadPayloads),
				uploadVertices, byteCount, discard ? 1u : 0u,
				startVertex, state.nextVertex, state.vertexCapacity,
				state.uploadEpoch, state.dynamicWriteSerial,
				state.dynamicDiscardSerial);
		}
		PublishSortedRingLeaseLocked(state, payloadTemplates,
			generation);
	}

	void EndNativeA8DirectShapeSubmission(
		NativeA8DirectShapeSubmission& submission)
	{
		if (submission.active)
		{
			NativeA8RingState& state = RingState();
			if (s_sortedRingLease.active
				&& s_sortedRingLease.state == &state)
			{
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
			}
			else
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
				if (!state.activeSubmissions.load(std::memory_order_acquire)
					&& state.releasePending.load(
						std::memory_order_acquire))
				{
					ReleaseRingResourcesLocked(state);
				}
			}
		}
		submission = NativeA8DirectShapeSubmission{};
	}

	NativeA8FallbackReason BeginNativeA8DirectShapeSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8DirectShapeSubmission& submission)
	{
		EndNativeA8DirectShapeSubmission(submission);
		if (!facade || !payload.buildComplete || !payload.payloadTemplate
			|| payload.packetShaders.size() != 1)
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact, payload.useCompositePackets);
		if (artifact.pageCount != 1 || artifact.atlasProperties.size() != 1
			|| artifact.atlasTextures.size() != 1 || packets.size() != 1
			|| !payload.packetShaders[0])
		{
			return NativeA8FallbackReason::PacketBuild;
		}
		const NativeA8PacketTemplate& packet = packets[0];
		if (packet.atlasPage != 0 || packet.firstVertex != 0
			|| !packet.vertexCount
			|| packet.vertexCount != artifact.gpuVertices.size()
			|| (packet.vertexCount & 3u))
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		// The direct shape already owns the first physical atlas property from
		// construction. Requiring exact wrapper and source-texture identity keeps
		// this path mutation-free; page/property changes fall back to the proxy
		// route, which retains its complete synchronization contract.
		const TileShaderPropertyView* tile = GetTileProperty(facade);
		if (!tile || facade->GetTexturingProperty()
				!= artifact.atlasProperties[0].m_pObject
			|| tile->sourceTexture.m_pObject
				!= artifact.atlasTextures[0].m_pObject)
		{
			return NativeA8FallbackReason::PropertySync;
		}

		if (!s_sortedRingLease.active)
			return NativeA8FallbackReason::PacketPrepare;
		NativeA8SortedRingLease& lease = s_sortedRingLease;
		NativeA8RingState* state = lease.state;
		if (!state || payload.preparedGeneration != lease.generation
			|| !IsNativeA8ShaderGenerationCurrent(lease.generation)
			|| state->generation != lease.generation
			|| state->resourceSerial.load(std::memory_order_acquire)
				!= lease.resourceSerial
			|| state->uploadEpoch != lease.uploadEpoch
			|| state->vertexBuffer != lease.dynamicVertexBuffer
			|| state->staticVertexBuffer != lease.staticVertexBuffer
			|| state->indexBuffer != lease.indexBuffer
			|| state->declaration != lease.declaration
			|| !lease.indexBuffer || !lease.declaration)
		{
			return NativeA8FallbackReason::PacketPrepare;
		}

		UInt32 baseVertex = 0;
		bool staticResident = false;
		if (!ResolveSortedLeaseResidency(*state, artifact,
			packet.vertexCount, lease.resourceSerial, lease.uploadEpoch,
			baseVertex, staticResident))
		{
			return NativeA8FallbackReason::PacketPrepare;
		}
		IDirect3DVertexBuffer9* vertexBuffer = staticResident
			? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		if (!vertexBuffer)
			return NativeA8FallbackReason::PacketPrepare;

		submission.vertexBuffer = vertexBuffer;
		submission.indexBuffer = lease.indexBuffer;
		submission.declaration = lease.declaration;
		submission.baseVertex = baseVertex;
		submission.vertexCount = packet.vertexCount;
		submission.indexBytes = kCanonicalIndexBytes;
		submission.generation = lease.generation;
		submission.resourceSerial = lease.resourceSerial;
		submission.staticResident = staticResident;
		submission.active = true;
		state->activeSubmissions.fetch_add(1, std::memory_order_release);
		payload.packetPrepareFailure.store(
			NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		return NativeA8FallbackReason::None;
	}

	NativeA8FallbackReason ResolveNativeA8VirtualStockPacketBinding(
		NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8VirtualStockPacketBinding& binding)
	{
		binding = {};
		if (!payload.buildComplete || !payload.payloadTemplate
			|| payload.preparedGeneration == 0)
		{
			return NativeA8FallbackReason::PacketBuild;
		}
		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size()
			|| packetIndex >= payload.packetShaders.size()
			|| !payload.packetShaders[packetIndex]
			|| artifact.gpuVertices.empty()
			|| artifact.gpuVertices.size()
				> std::numeric_limits<UInt32>::max())
		{
			return NativeA8FallbackReason::PacketBuild;
		}
		const NativeA8PacketTemplate& packet = packets[packetIndex];
		const UInt32 artifactVertexCount = static_cast<UInt32>(
			artifact.gpuVertices.size());
		const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.firstVertex & 3u)
			|| (packet.vertexCount & 3u)
			|| vertexEnd > artifactVertexCount)
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		if (!IsNativeA8ShaderGenerationCurrent(
				payload.preparedGeneration))
		{
			return NativeA8FallbackReason::PacketPrepare;
		}
		NativeA8FramePacketBinding frameBinding;
		if (!ResolveNativeA8FramePacketBinding(
				payload, packetIndex, frameBinding))
		{
			return NativeA8FallbackReason::PacketPrepare;
		}

		binding.vertexBuffer = frameBinding.vertexBuffer;
		binding.indexBuffer = frameBinding.indexBuffer;
		binding.declaration = frameBinding.declaration;
		binding.baseVertex = frameBinding.baseVertex;
		binding.vertexCount = frameBinding.vertexCount;
		binding.indexBytes = frameBinding.indexBytes;
		binding.generation = frameBinding.generation;
		binding.resourceSerial = frameBinding.resourceSerial;
		binding.uploadEpoch = frameBinding.uploadEpoch;
		binding.atlasTextureEpoch = payload.preflightAtlasTextureEpoch;
		binding.staticResident = frameBinding.staticResident;
		binding.active = true;
		return NativeA8FallbackReason::None;
	}

	bool IsNativeA8VirtualStockPacketBindingCurrent(
		const NativeA8VirtualStockPacketBinding& binding)
	{
		if (!binding.active || !s_sortedRingLease.active)
			return false;
		NativeA8FramePacketBinding frameBinding;
		frameBinding.vertexBuffer = binding.vertexBuffer;
		frameBinding.indexBuffer = binding.indexBuffer;
		frameBinding.declaration = binding.declaration;
		frameBinding.baseVertex = binding.baseVertex;
		frameBinding.vertexCount = binding.vertexCount;
		frameBinding.indexBytes = binding.indexBytes;
		frameBinding.generation = binding.generation;
		frameBinding.resourceSerial = binding.resourceSerial;
		frameBinding.uploadEpoch = binding.uploadEpoch;
		frameBinding.staticResident = binding.staticResident;
		frameBinding.active = binding.active;
		return binding.atlasTextureEpoch
				== GetNativeA8AtlasTextureEpoch()
			&& IsNativeA8FramePacketBindingCurrent(frameBinding);
	}

	bool IsNativeA8VirtualStockPacketAtlasCurrent(
		const NiTriShape* shape, const NativeA8ShapePayload& payload,
		UInt32 packetIndex)
	{
		if (!shape || !payload.buildComplete
			|| !payload.payloadTemplate)
		{
			return false;
		}
		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size())
			return false;
		const UInt16 page = packets[packetIndex].atlasPage;
		if (page >= artifact.atlasProperties.size()
			|| page >= artifact.atlasTextures.size()
			|| !artifact.atlasProperties[page]
			|| !artifact.atlasTextures[page])
		{
			return false;
		}
		const TileShaderPropertyView* tile = GetTileProperty(shape);
		return tile
			&& shape->GetTexturingProperty()
				== artifact.atlasProperties[page].m_pObject
			&& tile->sourceTexture.m_pObject
				== artifact.atlasTextures[page].m_pObject;
	}

	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission)
	{
		EndNativeA8RingSubmission(submission);
		if (!facade || !payload.buildComplete || !payload.payloadTemplate
			|| payload.packetShaders.empty())
		{
			return NativeA8FallbackReason::PacketBuild;
		}
		const std::vector<NativeA8PacketTemplate>& activePackets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (payload.packetShaders.size() != activePackets.size())
			return NativeA8FallbackReason::PacketBuild;
		NativeA8FallbackReason leaseResult =
			NativeA8FallbackReason::RuntimeFault;
		if (TryBeginSortedRingSubmission(facade, payload, submission,
			leaseResult))
		{
			if (leaseResult == NativeA8FallbackReason::None)
				return leaseResult;
			// A generation/resource/range mismatch invalidates the complete frame
			// snapshot. Drop its lease, then run the existing locked per-facade
			// path so the affected text still has a correctness-preserving fallback.
			// A recursive draw can still own a proxy from this lease; in that case
			// retaining the lease is mandatory because proxy inUse state is
			// render-thread confined until the final lockless submission ends.
			if (s_sortedRingLease.state
				&& s_sortedRingLease.state->activeSubmissions.load(
					std::memory_order_acquire))
			{
				return leaseResult;
			}
			EndNativeA8SortedRingFrame();
		}

		const UInt64 totalVertexCount = payload.payloadTemplate->gpuVertices.size();
		if (!totalVertexCount || totalVertexCount > std::numeric_limits<UInt32>::max())
			return NativeA8FallbackReason::PacketBuild;
		// InitializeNativeA8ShapePayload and the generation preflight validate every
		// immutable packet span. Rewalking all spans here made the steady sorted path
		// pay the same O(packet count) validation twice for every facade.
		const UInt32 totalVertices = static_cast<UInt32>(totalVertexCount);

		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sortedFrameLeases.load(std::memory_order_acquire))
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::ProxyUnavailable,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PacketPrepare;
		}
		const UInt32 proxyIndex = AcquireProxyLocked(state, s_ringThread);
		if (proxyIndex == std::numeric_limits<UInt32>::max())
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::ProxyUnavailable,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PacketPrepare;
		}

		const char* operation = "ring-resource";
		HRESULT result = D3DERR_DEVICELOST;
		if (!EnsureRingResourcesLocked(state, payload.preparedGeneration,
			totalVertices,
			operation, result))
		{
			state.proxies[proxyIndex].inUse = false;
			NativeA8PacketPrepareFailure prepareFailure =
				NativeA8PacketPrepareFailure::VertexBuffer;
			if (operation && std::strcmp(operation, "ring-context") == 0)
				prepareFailure = NativeA8PacketPrepareFailure::Generation;
			else if (operation && std::strcmp(operation, "ring-declaration") == 0)
				prepareFailure = NativeA8PacketPrepareFailure::Declaration;
			else if (operation && std::strcmp(operation, "ring-capacity") == 0)
				prepareFailure = NativeA8PacketPrepareFailure::RingCapacity;
			else if (operation && std::strcmp(operation, "ring-busy") == 0)
				prepareFailure = NativeA8PacketPrepareFailure::ProxyUnavailable;
			else if (operation && (std::strcmp(operation, "CreateIndexBuffer") == 0
				|| std::strcmp(operation, "canonical-index-upload") == 0))
			{
				prepareFailure = NativeA8PacketPrepareFailure::IndexBuffer;
			}
			payload.packetPrepareFailure.store(prepareFailure,
				std::memory_order_relaxed);
			if (prepareFailure != NativeA8PacketPrepareFailure::RingCapacity
				&& prepareFailure
					!= NativeA8PacketPrepareFailure::ProxyUnavailable)
			{
				MarkNativeA8GenerationFault(payload.preparedGeneration,
					operation, result);
			}
			return NativeA8FallbackReason::PacketPrepare;
		}

		UInt32 startVertex = 0;
		bool staticResident = ResolveStaticPayloadLocked(state,
			payload.payloadTemplate, totalVertices, startVertex);
		if (!staticResident && !State().renderAlphaGeometryHookInstalled)
		{
			staticResident = PromoteStaticPayloadLocked(state,
				payload.payloadTemplate, totalVertices, startVertex);
		}

		if (!staticResident)
		{
			const bool reusedUpload = ResolveUploadedPayloadLocked(state,
				payload.payloadTemplate, totalVertices, startVertex);
			if (!reusedUpload)
			{
				startVertex = state.nextVertex;
				DWORD lockFlags = D3DLOCK_NOOVERWRITE;
				if (!startVertex || startVertex > state.vertexCapacity
					|| totalVertices > state.vertexCapacity - startVertex)
				{
					if (state.activeSubmissions.load(
						std::memory_order_acquire))
					{
						state.proxies[proxyIndex].inUse = false;
						payload.packetPrepareFailure.store(
							NativeA8PacketPrepareFailure::ProxyUnavailable,
							std::memory_order_relaxed);
						return NativeA8FallbackReason::PacketPrepare;
					}
					startVertex = 0;
					lockFlags = D3DLOCK_DISCARD;
					AdvanceUploadEpochLocked(state);
					AdvanceDiagnosticSerial(state.dynamicDiscardSerial);
					state.uploadedPayloads.clear();
					RefreshRingCpuMemoryLocked(state);
					s_ringThread.uploadedPayload = {};
					RecordFreeTypePerf(
						FreeTypePerfCounter::DynamicVertexDiscard);
				}
				void* destination = nullptr;
				const UINT byteOffset = startVertex
					* sizeof(NativeA8GpuVertex);
				const UINT byteCount = totalVertices
					* sizeof(NativeA8GpuVertex);
				result = state.vertexBuffer->Lock(byteOffset, byteCount,
					&destination, lockFlags);
				if (FAILED(result) || !destination)
				{
					if (SUCCEEDED(result))
						result = E_FAIL;
					state.proxies[proxyIndex].inUse = false;
					payload.packetPrepareFailure.store(
						NativeA8PacketPrepareFailure::VertexBuffer,
						std::memory_order_relaxed);
					MarkNativeA8GenerationFault(payload.preparedGeneration,
						"dynamic-vb-lock", result);
					return NativeA8FallbackReason::PacketPrepare;
				}

				std::memcpy(destination,
					payload.payloadTemplate->gpuVertices.data(), byteCount);
				result = state.vertexBuffer->Unlock();
				if (FAILED(result))
				{
					state.proxies[proxyIndex].inUse = false;
					payload.packetPrepareFailure.store(
						NativeA8PacketPrepareFailure::VertexBuffer,
						std::memory_order_relaxed);
					MarkNativeA8GenerationFault(payload.preparedGeneration,
						"dynamic-vb-unlock", result);
					return NativeA8FallbackReason::PacketPrepare;
				}
				AdvanceDiagnosticSerial(state.dynamicWriteSerial);

				state.nextVertex = startVertex + totalVertices;
				PublishUploadedPayloadLocked(state, payload.payloadTemplate,
					startVertex, totalVertices);
				RefreshRingCpuMemoryLocked(state);
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
			}

			ObserveStaticCandidateLocked(state, payload.payloadTemplate,
				totalVertices, false);
		}

		NativeA8Proxy& proxy = state.proxies[proxyIndex];
		if (!proxy.shape || !proxy.buffer || !proxy.chip
			|| !SyncProxyState(*facade, proxy, payload.geometryOrigin))
		{
			proxy.inUse = false;
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::Geometry,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PropertySync;
		}

		submission.proxyShape = proxy.shape.m_pObject;
		submission.proxyBuffer = proxy.buffer;
		submission.proxyChip = proxy.chip;
		submission.vertexBuffer = staticResident
			? state.staticVertexBuffer : state.vertexBuffer;
		submission.proxyIndex = proxyIndex;
		submission.generation = state.generation;
		submission.resourceSerial = state.resourceSerial.load(
			std::memory_order_acquire);
		submission.nextPacket = 0;
		submission.payloadBaseVertex = startVertex;
		submission.endVertex = startVertex + totalVertices;
		submission.staticResident = staticResident;
		submission.active = true;
		state.activeSubmissions.fetch_add(1, std::memory_order_release);
		payload.packetPrepareFailure.store(
			NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		return NativeA8FallbackReason::None;
	}

	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape)
	{
		proxyShape = nullptr;
		if (!submission.active || !facade || !submission.proxyShape
			|| packetIndex != submission.nextPacket
			|| packetIndex >= payload.packetShaders.size()
			|| submission.generation != payload.preparedGeneration
			|| !IsNativeA8ShaderGenerationCurrent(submission.generation)
			|| !payload.payloadTemplate)
		{
			return NativeA8FallbackReason::RuntimeFault;
		}

		const std::vector<NativeA8PacketTemplate>& activePackets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (packetIndex >= activePackets.size())
			return NativeA8FallbackReason::PacketBuild;
		TileShader* shader = payload.packetShaders[packetIndex];
		const NativeA8PacketTemplate& source =
			activePackets[packetIndex];
		if (!shader || source.atlasPage
			>= payload.payloadTemplate->atlasTextures.size())
		{
			return NativeA8FallbackReason::AtlasGeneration;
		}
		const UInt32 vertexCount = source.vertexCount;
		const UInt64 baseVertex = static_cast<UInt64>(
			submission.payloadBaseVertex) + source.firstVertex;
		if (!vertexCount || (vertexCount & 3u)
			|| baseVertex + vertexCount > submission.endVertex)
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		NativeA8RingState& state = RingState();
		// Begin reserved this proxy under either the ring mutex or the validated
		// sorted-frame lease, then incremented activeSubmissions. Resource
		// replacement/release is deferred until End, so packet-local property and
		// buffer updates need no global lock.
		IDirect3DVertexBuffer9* expectedVertexBuffer =
			submission.staticResident
			? state.staticVertexBuffer : state.vertexBuffer;
		if (state.resourceSerial.load(std::memory_order_acquire)
			!= submission.resourceSerial
			|| submission.proxyIndex >= state.proxyCount
			|| !state.proxies[submission.proxyIndex].inUse
			|| expectedVertexBuffer != submission.vertexBuffer
			|| state.generation != submission.generation)
		{
			return NativeA8FallbackReason::RuntimeFault;
		}
		NativeA8Proxy& reservedProxy = state.proxies[submission.proxyIndex];
		NiTriShape* proxy = reservedProxy.shape.m_pObject;
		NiGeometryBufferData* buffer = reservedProxy.buffer;
		NiVBChip* chip = reservedProxy.chip;
		if (!proxy || !buffer || !chip
			|| proxy != submission.proxyShape
			|| buffer != submission.proxyBuffer
			|| chip != submission.proxyChip
			|| !BindPacketAtlasPage(reservedProxy, *payload.payloadTemplate,
				source.atlasPage))
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::Geometry,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PropertySync;
		}

		const UInt32 quadCount = vertexCount / 4u;
		chip->m_pkVB = submission.vertexBuffer;
		chip->m_uiOffset = 0;
		chip->m_uiLockFlags = 0;
		chip->m_uiSize = vertexCount * sizeof(NativeA8GpuVertex);
		buffer->m_uiVertCount = vertexCount;
		buffer->m_uiMaxVertCount = vertexCount;
		buffer->m_uiIndexCount = quadCount * 6u;
		buffer->m_uiBaseVertexIndex = static_cast<UInt32>(baseVertex);
		buffer->m_eType = D3DPT_TRIANGLELIST;
		buffer->m_uiTriCount = quadCount * 2u;
		buffer->m_uiMaxTriCount = quadCount * 2u;
		// One contiguous indexed array is required even when m_pusArrayLengths is
		// null; stock then uses m_uiTriCount for this single draw.
		buffer->m_uiNumArrays = kCanonicalArrayCount;
		proxy->GetModelData()->m_kBound = source.bound;
		if (reservedProxy.shader != shader)
		{
			proxy->SetShader(shader);
			reservedProxy.shader = proxy->GetShader();
			if (reservedProxy.shader != shader)
			{
				payload.packetPrepareFailure.store(
					NativeA8PacketPrepareFailure::ShaderBinding,
					std::memory_order_relaxed);
				return NativeA8FallbackReason::PacketPrepare;
			}
		}

		++submission.nextPacket;
		proxyShape = proxy;
		RecordFreeTypePerf(FreeTypePerfCounter::LocklessPacketPrepare);
		return NativeA8FallbackReason::None;
	}

	NativeA8FallbackReason SkipNativeA8RingPacket(
		NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex)
	{
		if (!submission.active || packetIndex != submission.nextPacket
			|| packetIndex >= payload.packetShaders.size()
			|| submission.generation != payload.preparedGeneration
			|| !IsNativeA8ShaderGenerationCurrent(submission.generation)
			|| !payload.payloadTemplate)
		{
			return NativeA8FallbackReason::RuntimeFault;
		}
		const std::vector<NativeA8PacketTemplate>& activePackets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (packetIndex >= activePackets.size())
			return NativeA8FallbackReason::PacketBuild;
		const NativeA8PacketTemplate& packet = activePackets[packetIndex];
		const UInt64 end = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.vertexCount & 3u)
			|| end > payload.payloadTemplate->gpuVertices.size())
		{
			return NativeA8FallbackReason::PacketBuild;
		}
		++submission.nextPacket;
		return NativeA8FallbackReason::None;
	}

	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission)
	{
		if (submission.active)
		{
			NativeA8RingState& state = RingState();
			if (s_sortedRingLease.active
				&& s_sortedRingLease.state == &state)
			{
				if (submission.proxyIndex < state.proxyCount)
					state.proxies[submission.proxyIndex].inUse = false;
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
			}
			else
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				if (submission.proxyIndex < state.proxyCount)
					state.proxies[submission.proxyIndex].inUse = false;
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
				if (!state.activeSubmissions.load(std::memory_order_acquire)
					&& state.releasePending.load(
						std::memory_order_acquire))
				{
					ReleaseRingResourcesLocked(state);
				}
			}
		}
		submission = NativeA8RingSubmission{};
	}

	void EndNativeA8SortedRingFrame()
	{
		if (!s_sortedRingLease.active)
			return;
		NativeA8RingState* state = s_sortedRingLease.state;
		if (state && state->activeSubmissions.load(std::memory_order_acquire))
			return;
		s_sortedRingLease = {};
		if (!state)
			return;
		const UInt32 previous = state->sortedFrameLeases.fetch_sub(
			1, std::memory_order_acq_rel);
		if (previous <= 1
			&& state->releasePending.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->sortedFrameLeases.load(std::memory_order_acquire)
				&& !state->activeSubmissions.load(std::memory_order_acquire))
			{
				if (state->releasePending.load(std::memory_order_acquire))
					ReleaseRingResourcesLocked(*state);
			}
		}
	}

	bool ResolveNativeA8FramePayloadBinding(
		const NativeA8ShapePayload& payload,
		NativeA8FramePayloadBinding& binding)
	{
		binding = {};
		if (!s_sortedRingLease.active || !payload.payloadTemplate
			|| payload.preparedGeneration != s_sortedRingLease.generation)
		{
			return false;
		}
		NativeA8SortedRingLease& lease = s_sortedRingLease;
		NativeA8RingState* state = lease.state;
		if (!state || state->generation != lease.generation
			|| state->resourceSerial.load(std::memory_order_acquire)
				!= lease.resourceSerial
			|| state->uploadEpoch != lease.uploadEpoch
			|| state->indexBuffer != lease.indexBuffer
			|| state->declaration != lease.declaration)
		{
			return false;
		}
		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		if (artifact.gpuVertices.empty()
			|| artifact.gpuVertices.size()
				> std::numeric_limits<UInt32>::max())
		{
			return false;
		}
		const UInt32 artifactVertices = static_cast<UInt32>(
			artifact.gpuVertices.size());
		UInt32 payloadBaseVertex = 0;
		bool staticResident = false;
		if (!ResolveSortedLeaseResidency(*state, artifact,
			artifactVertices, lease.resourceSerial, lease.uploadEpoch,
			payloadBaseVertex, staticResident))
		{
			return false;
		}

		binding.vertexBuffer = staticResident
			? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		binding.indexBuffer = lease.indexBuffer;
		binding.declaration = lease.declaration;
		binding.payloadBaseVertex = payloadBaseVertex;
		binding.payloadVertexCount = artifactVertices;
		binding.indexBytes = kCanonicalIndexBytes;
		binding.generation = lease.generation;
		binding.resourceSerial = lease.resourceSerial;
		binding.uploadEpoch = lease.uploadEpoch;
		binding.staticResident = staticResident;
		binding.active = binding.vertexBuffer && binding.indexBuffer
			&& binding.declaration;
		return binding.active;
	}

	bool ResolveNativeA8FramePacketBinding(
		const NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8FramePacketBinding& binding)
	{
		binding = {};
		if (!payload.payloadTemplate)
			return false;
		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size())
			return false;
		const NativeA8PacketTemplate& packet = packets[packetIndex];
		const UInt64 packetEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.vertexCount & 3u)
			|| packetEnd > artifact.gpuVertices.size())
		{
			return false;
		}
		NativeA8FramePayloadBinding payloadBinding;
		if (!ResolveNativeA8FramePayloadBinding(
				payload, payloadBinding))
			return false;
		const UInt64 baseVertex = static_cast<UInt64>(
			payloadBinding.payloadBaseVertex)
			+ packet.firstVertex;
		if (baseVertex > std::numeric_limits<UInt32>::max())
			return false;

		binding.vertexBuffer = payloadBinding.vertexBuffer;
		binding.indexBuffer = payloadBinding.indexBuffer;
		binding.declaration = payloadBinding.declaration;
		binding.baseVertex = static_cast<UInt32>(baseVertex);
		binding.vertexCount = packet.vertexCount;
		binding.indexBytes = payloadBinding.indexBytes;
		binding.generation = payloadBinding.generation;
		binding.resourceSerial = payloadBinding.resourceSerial;
		binding.uploadEpoch = payloadBinding.uploadEpoch;
		binding.staticResident = payloadBinding.staticResident;
		binding.active = payloadBinding.active;
		return binding.active;
	}

	bool IsNativeA8FramePacketBindingCurrent(
		const NativeA8FramePacketBinding& binding)
	{
		if (!binding.active || !s_sortedRingLease.active)
			return false;
		const NativeA8SortedRingLease& lease = s_sortedRingLease;
		const NativeA8RingState* state = lease.state;
		const IDirect3DVertexBuffer9* expectedVertexBuffer =
			binding.staticResident
				? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		return state && lease.active
			&& binding.generation == lease.generation
			&& binding.resourceSerial == lease.resourceSerial
			&& binding.uploadEpoch == lease.uploadEpoch
			&& binding.vertexBuffer == expectedVertexBuffer
			&& binding.indexBuffer == lease.indexBuffer
			&& binding.declaration == lease.declaration
			&& state->generation == lease.generation
			&& state->resourceSerial.load(std::memory_order_acquire)
				== lease.resourceSerial
			&& state->uploadEpoch == lease.uploadEpoch;
	}

	bool InspectNativeA8RingPacketForDiagnostic(
		const NativeA8DrawCommand& command,
		NativeA8RingPacketDiagnostic& diagnostic)
	{
		diagnostic = {};
		if (!command.payload || !command.payload->payloadTemplate
			|| !command.packet)
		{
			return false;
		}

		const NativeA8PayloadTemplatePtr& owner =
			command.payload->payloadTemplate;
		const NativeA8PayloadTemplate& artifact = *owner;
		const NativeA8PacketTemplate& packet = *command.packet;
		diagnostic.artifact = &artifact;
		diagnostic.packet = &packet;
		diagnostic.packetFirstVertex = packet.firstVertex;
		diagnostic.packetVertexCount = packet.vertexCount;
		diagnostic.staticResident = command.binding.staticResident;
		if (artifact.gpuVertices.empty()
			|| artifact.gpuVertices.size()
				> std::numeric_limits<UInt32>::max())
		{
			return true;
		}

		diagnostic.payloadVertexCount = static_cast<UInt32>(
			artifact.gpuVertices.size());
		diagnostic.cpuPayloadHash = HashDiagnosticBytes(
			artifact.gpuVertices.data(), artifact.gpuVertices.size()
				* sizeof(NativeA8GpuVertex));
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact,
				command.payload->useCompositePackets);
		diagnostic.packetIdentityMatch = command.packetIndex < packets.size()
			&& &packets[command.packetIndex] == command.packet;
		const UInt64 packetEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		diagnostic.packetRangeMatch = packet.vertexCount
			&& !(packet.vertexCount & 3u)
			&& packetEnd <= artifact.gpuVertices.size()
			&& command.binding.vertexCount == packet.vertexCount;
		if (diagnostic.packetRangeMatch)
		{
			diagnostic.cpuPacketHash = HashDiagnosticBytes(
				artifact.gpuVertices.data() + packet.firstVertex,
				static_cast<size_t>(packet.vertexCount)
					* sizeof(NativeA8GpuVertex));
		}
		const UInt64 submittedEnd = static_cast<UInt64>(
			command.binding.baseVertex) + command.binding.vertexCount;
		if (command.binding.baseVertex >= packet.firstVertex)
		{
			diagnostic.expectedPayloadBaseVertex =
				command.binding.baseVertex - packet.firstVertex;
		}
		if (submittedEnd <= std::numeric_limits<UInt32>::max())
		{
			diagnostic.expectedPacketEndVertex =
				static_cast<UInt32>(submittedEnd);
		}
		const UInt64 indexBytes = static_cast<UInt64>(
			packet.vertexCount / 4u) * 6u * sizeof(UInt16);
		diagnostic.canonicalIndexRangeReady =
			diagnostic.packetRangeMatch
			&& packet.vertexCount / 4u <= kNativeA8MaximumQuads
			&& indexBytes <= command.binding.indexBytes;

		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		diagnostic.stateGeneration = state.generation;
		diagnostic.stateResourceSerial = state.resourceSerial.load(
			std::memory_order_acquire);
		diagnostic.stateUploadEpoch = state.uploadEpoch;
		diagnostic.stateWriteSerial = command.binding.staticResident
			? state.staticWriteSerial : state.dynamicWriteSerial;
		diagnostic.stateDiscardSerial = command.binding.staticResident
			? 0u : state.dynamicDiscardSerial;
		diagnostic.stateNextVertex = command.binding.staticResident
			? state.nextStaticVertex : state.nextVertex;
		diagnostic.stateVertexCapacity = command.binding.staticResident
			? state.staticVertexCapacity : state.vertexCapacity;

		const NativeA8SortedRingLease& lease = s_sortedRingLease;
		diagnostic.leaseActive = lease.active && lease.state == &state;
		diagnostic.leaseGeneration = lease.generation;
		diagnostic.leaseResourceSerial = lease.resourceSerial;
		diagnostic.leaseUploadEpoch = lease.uploadEpoch;
		const IDirect3DVertexBuffer9* expectedVertexBuffer =
			command.binding.staticResident
				? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		diagnostic.bindingCurrent = command.binding.active
			&& diagnostic.leaseActive
			&& command.binding.generation == lease.generation
			&& command.binding.resourceSerial == lease.resourceSerial
			&& command.binding.uploadEpoch == lease.uploadEpoch
			&& command.binding.vertexBuffer == expectedVertexBuffer
			&& command.binding.indexBuffer == lease.indexBuffer
			&& command.binding.declaration == lease.declaration
			&& state.generation == lease.generation
			&& diagnostic.stateResourceSerial == lease.resourceSerial
			&& state.uploadEpoch == lease.uploadEpoch;

		if (command.binding.staticResident)
		{
			const auto found = state.staticPayloads.find(&artifact);
			if (found != state.staticPayloads.end())
			{
				diagnostic.residencyFound = true;
				const NativeA8StaticPayload& record = found->second;
				const NativeA8PayloadTemplatePtr recordOwner =
					record.owner.lock();
				diagnostic.ownerMatch = recordOwner.get() == &artifact;
				diagnostic.recordBaseVertex = record.baseVertex;
				diagnostic.recordVertexCount = record.vertexCount;
				diagnostic.recordWriteSerial = record.writeSerial;
				diagnostic.recordedPayloadHash = record.payloadHash;
			}
		}
		else
		{
			const auto found = state.uploadedPayloads.find(&artifact);
			if (found != state.uploadedPayloads.end())
			{
				diagnostic.residencyFound = true;
				const NativeA8UploadedPayload& record = found->second;
				const NativeA8PayloadTemplatePtr recordOwner =
					record.owner.lock();
				diagnostic.ownerMatch = recordOwner.get() == &artifact;
				diagnostic.recordBaseVertex = record.baseVertex;
				diagnostic.recordVertexCount = record.vertexCount;
				diagnostic.recordUploadEpoch = record.epoch;
				diagnostic.recordWriteSerial = record.writeSerial;
				diagnostic.recordDiscardSerial = record.discardSerial;
				diagnostic.recordedPayloadHash = record.payloadHash;
			}
		}

		const UInt64 recordEnd = static_cast<UInt64>(
			diagnostic.recordBaseVertex) + diagnostic.recordVertexCount;
		diagnostic.recordRangeMatch = diagnostic.residencyFound
			&& diagnostic.ownerMatch
			&& diagnostic.recordBaseVertex
				== diagnostic.expectedPayloadBaseVertex
			&& diagnostic.recordVertexCount
				== diagnostic.payloadVertexCount
			&& command.binding.baseVertex
				== diagnostic.recordBaseVertex + packet.firstVertex
			&& command.binding.vertexCount == packet.vertexCount;
		diagnostic.rangePublished = diagnostic.residencyFound
			&& recordEnd <= diagnostic.stateNextVertex
			&& recordEnd <= diagnostic.stateVertexCapacity
			&& submittedEnd <= diagnostic.stateNextVertex
			&& submittedEnd <= diagnostic.stateVertexCapacity;
		diagnostic.hashRecorded =
			diagnostic.recordedPayloadHash != 0;
		diagnostic.hashMatch = diagnostic.hashRecorded
			&& diagnostic.recordedPayloadHash
				== diagnostic.cpuPayloadHash;
		return true;
	}

	bool IsNativeA8FrameResourceStampCurrent(
		UInt32 generation, UInt32 resourceSerial, UInt32 uploadEpoch)
	{
		if (!generation || !resourceSerial
			|| !s_sortedRingLease.active)
		{
			return false;
		}
		const NativeA8SortedRingLease& lease = s_sortedRingLease;
		const NativeA8RingState* state = lease.state;
		return state && lease.active
			&& generation == lease.generation
			&& resourceSerial == lease.resourceSerial
			&& uploadEpoch == lease.uploadEpoch
			&& lease.indexBuffer && lease.declaration
			&& (lease.staticVertexBuffer || lease.dynamicVertexBuffer)
			&& state->generation == lease.generation
			&& state->resourceSerial.load(std::memory_order_acquire)
				== lease.resourceSerial
			&& state->uploadEpoch == lease.uploadEpoch
			&& state->indexBuffer == lease.indexBuffer
			&& state->declaration == lease.declaration;
	}

	void ReleaseNativeA8RingResources()
	{
		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sortedFrameLeases.load(std::memory_order_acquire)
			|| state.activeSubmissions.load(std::memory_order_acquire))
		{
			if (!state.releasePending.exchange(true,
				std::memory_order_acq_rel))
				AdvanceResourceSerialLocked(state);
			return;
		}
		ReleaseRingResourcesLocked(state);
	}
}
