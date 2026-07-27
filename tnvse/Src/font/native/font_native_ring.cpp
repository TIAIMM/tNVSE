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
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kGeometryBufferDataConstructor = 0xE947C0;
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
		inline constexpr UInt32 kStaticPromotionSubmissionCount = 2;
		inline constexpr size_t kStaticCandidateLimit = 4096;
		inline constexpr UInt32 kCanonicalIndexCount =
			kNativeA8MaximumQuads * 6u;
		inline constexpr UInt32 kCanonicalIndexBytes =
			kCanonicalIndexCount * sizeof(UInt16);
		inline constexpr UInt32 kCanonicalArrayCount = 1;

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
		};

		struct NativeA8StaticPayload
		{
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
		};

		struct NativeA8StaticCandidate
		{
			CpuMemoryLease cpuMemory;
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 submissionCount = 0;
			bool promotionDisabled = false;
		};

		struct NativeA8StaticHotEntry
		{
			const NativeA8PayloadTemplate* key = nullptr;
			NativeA8PayloadTemplatePtr owner;
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
			NativeA8PayloadTemplatePtr owner;
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
			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8UploadedPayload> uploadedPayloads;
			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8StaticPayload> staticPayloads;
			std::unordered_map<const NativeA8PayloadTemplate*,
				std::shared_ptr<NativeA8StaticCandidate>> staticCandidates;
			CpuMemoryLease cpuMemory;
			std::atomic<UInt32> resourceSerial = 1;
			std::atomic<UInt32> sortedFrameLeases = 0;
			std::atomic<UInt32> activeSubmissions = 0;
			std::atomic<bool> releasePending = false;
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
			return serial;
		}

		void ReleaseRingResourcesLocked(NativeA8RingState& state)
		{
			state.releasePending.store(false, std::memory_order_release);
			++state.uploadEpoch;
			if (!state.uploadEpoch)
				++state.uploadEpoch;
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
			UInt64 staticDesired = g_bDisableFreeTypeExtendedCaches
				? 0 : std::min<UInt64>(kStaticInitialVertexCapacity, capLimit);
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
					"tnvse_freetype_native: geometry cache ready generation=%u proxies=%u dynamicVertexCapacity=%u staticVertexCapacity=%u staticPromotionSubmissions=%u vertexStride=%u canonicalQuads=%u canonicalIndexBytes=%u",
					generation, state.proxyCount, state.vertexCapacity,
					state.staticVertexCapacity, kStaticPromotionSubmissionCount,
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

		bool TryGrowStaticVertexBufferLocked(NativeA8RingState& state,
			UInt32 requiredVertices, bool& permanentFailure)
		{
			permanentFailure = false;
			if (!state.device || !state.staticVertexBuffer || !requiredVertices)
			{
				permanentFailure = true;
				return false;
			}

			// A different active proxy can still be issuing packets against the old
			// buffer. Defer the optional promotion instead of invalidating that group.
			const UInt32 activeProxies = static_cast<UInt32>(std::count_if(
				state.proxies.begin(), state.proxies.begin() + state.proxyCount,
				[](const NativeA8Proxy& proxy) { return proxy.inUse; }));
			if (activeProxies > 1)
				return false;

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
					permanentFailure = true;
					return false;
				}
				livePayloads.push_back({ std::move(owner),
					static_cast<UInt32>(liveVertexCount) });
				liveVertexCount += livePayloads.back().owner->gpuVertices.size();
			}

			const UInt64 requiredCapacity = liveVertexCount + requiredVertices;
			if (requiredCapacity > kStaticTargetVertexCapacity)
			{
				permanentFailure = true;
				return false;
			}
			UInt64 desiredCapacity = state.staticVertexCapacity;
			while (desiredCapacity < requiredCapacity
				&& desiredCapacity < kStaticTargetVertexCapacity)
			{
				desiredCapacity = std::min<UInt64>(
					desiredCapacity * 2u, kStaticTargetVertexCapacity);
			}
			// Rebuilding at the same size is useful only when expired payloads left
			// holes behind the bump pointer. It compacts the live prefix in one upload.
			if (desiredCapacity == state.staticVertexCapacity
				&& liveVertexCount == state.nextStaticVertex)
			{
				permanentFailure = true;
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
				permanentFailure = true;
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
					permanentFailure = true;
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
					permanentFailure = true;
					return false;
				}
			}

			std::unordered_map<const NativeA8PayloadTemplate*,
				NativeA8StaticPayload> rebuilt;
			rebuilt.reserve(livePayloads.size());
			for (const LiveStaticPayload& payload : livePayloads)
			{
				rebuilt.emplace(payload.owner.get(), NativeA8StaticPayload{
					payload.owner, payload.baseVertex,
					static_cast<UInt32>(payload.owner->gpuVertices.size()) });
			}
			rebuildCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				livePayloads.capacity() * sizeof(LiveStaticPayload)
					+ EstimateUnorderedMapBytes(rebuilt));
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (state.proxies[index].chip)
					state.proxies[index].chip->m_pkVB = state.vertexBuffer;
			}
			state.staticVertexBuffer->Release();
			state.staticVertexBuffer = replacement;
			state.staticVertexCapacity = static_cast<UInt32>(desiredCapacity);
			state.nextStaticVertex = static_cast<UInt32>(liveVertexCount);
			state.staticPayloads = std::move(rebuilt);
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
				gLog.FormattedMessage(
					"tnvse_freetype_native: static vertex buffer rebuilt capacity=%u bytes=%u liveVertices=%u livePayloads=%u requestedVertices=%u",
					state.staticVertexCapacity,
					state.staticVertexCapacity * sizeof(NativeA8GpuVertex),
					state.nextStaticVertex,
					static_cast<UInt32>(state.staticPayloads.size()),
					requiredVertices);
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
			if (!g_bDisableFreeTypeExtendedCaches
				&& thread.preferredProxy < state.proxyCount
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
					if (!g_bDisableFreeTypeExtendedCaches)
						thread.preferredProxy = index;
					return index;
				}
			}
			return std::numeric_limits<UInt32>::max();
		}

		bool ResolveStaticPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			if (g_bDisableFreeTypeExtendedCaches)
				return false;
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
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectStaticResidencyHit);
				return true;
			}
			NativeA8StaticHotEntry& hot = s_ringThread.staticPayload;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial)
			{
				if (hot.owner.get() == payloadTemplate.get()
					&& hot.vertexCount == vertexCount
					&& hot.baseVertex <= state.staticVertexCapacity
					&& vertexCount <= state.staticVertexCapacity - hot.baseVertex)
				{
					baseVertex = hot.baseVertex;
					residency.staticResourceSerial = resourceSerial;
					residency.staticBaseVertex = hot.baseVertex;
					residency.staticVertexCount = hot.vertexCount;
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
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				return true;
			}
			state.staticPayloads.erase(found);
			RefreshRingCpuMemoryLocked(state);
			return false;
		}

		NativeA8StaticCandidate* ResolveStaticCandidateLocked(
			NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			if (g_bDisableFreeTypeExtendedCaches)
				return nullptr;
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
				if (hot.owner.get() == payloadTemplate.get())
					return hot.candidate.get();
				hot = {};
			}
			auto found = state.staticCandidates.find(payloadTemplate.get());
			if (found == state.staticCandidates.end())
			{
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					for (auto current = state.staticCandidates.begin();
						current != state.staticCandidates.end();)
					{
						if (!current->second || current->second->owner.expired())
							current = state.staticCandidates.erase(current);
						else
							++current;
					}
					RefreshRingCpuMemoryLocked(state);
					if (state.staticCandidates.size() >= kStaticCandidateLimit)
						return nullptr;
				}
				auto candidate = CreateStaticCandidate(payloadTemplate);
				found = state.staticCandidates.emplace(
					payloadTemplate.get(), std::move(candidate)).first;
			}
			else
			{
				const std::shared_ptr<const NativeA8PayloadTemplate> owner =
					found->second->owner.lock();
				if (owner.get() != payloadTemplate.get())
				{
					found->second = CreateStaticCandidate(payloadTemplate);
				}
			}
			RefreshRingCpuMemoryLocked(state);
			hot.key = payloadTemplate.get();
			hot.owner = payloadTemplate;
			hot.candidate = found->second;
			hot.resourceSerial = resourceSerial;
			return hot.candidate.get();
		}

		bool PromoteStaticPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			NativeA8StaticCandidate* candidate = ResolveStaticCandidateLocked(
				state, payloadTemplate, vertexCount);
			if (!candidate || candidate->promotionDisabled
				|| candidate->submissionCount
					< kStaticPromotionSubmissionCount)
			{
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
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
					return false;
				}
			}

			baseVertex = state.nextStaticVertex;
			const UINT byteOffset = baseVertex * sizeof(NativeA8GpuVertex);
			const UINT byteCount = vertexCount * sizeof(NativeA8GpuVertex);
			void* destination = nullptr;
			HRESULT result = state.staticVertexBuffer->Lock(byteOffset, byteCount,
				&destination, 0);
			if (FAILED(result) || !destination)
			{
				candidate->promotionDisabled = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}
			std::memcpy(destination, payloadTemplate->gpuVertices.data(), byteCount);
			result = state.staticVertexBuffer->Unlock();
			if (FAILED(result))
			{
				candidate->promotionDisabled = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}

			state.nextStaticVertex = baseVertex + vertexCount;
			state.staticPayloads[payloadTemplate.get()] = {
				payloadTemplate, baseVertex, vertexCount };
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.staticResourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			residency.staticBaseVertex = baseVertex;
			residency.staticVertexCount = vertexCount;
			state.staticCandidates.erase(payloadTemplate.get());
			s_ringThread.staticPayload = {
				payloadTemplate.get(), payloadTemplate, baseVertex, vertexCount,
				state.resourceSerial.load(std::memory_order_relaxed) };
			if (s_ringThread.staticCandidate.key == payloadTemplate.get())
				s_ringThread.staticCandidate = {};
			RefreshRingCpuMemoryLocked(state);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUploadBytes,
				byteCount);
			return true;
		}

		void ObserveStaticCandidateLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			NativeA8StaticCandidate* candidate = ResolveStaticCandidateLocked(
				state, payloadTemplate, vertexCount);
			if (candidate && !candidate->promotionDisabled
				&& candidate->submissionCount
					< std::numeric_limits<UInt32>::max())
			{
				++candidate->submissionCount;
			}
		}

		bool ResolveUploadedPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			if (g_bDisableFreeTypeExtendedCaches)
				return false;
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
			if (g_bDisableFreeTypeExtendedCaches)
				return false;
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
				payloadTemplate, baseVertex, vertexCount, state.uploadEpoch };
			NativeA8PayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.dynamicResourceSerial = resourceSerial;
			residency.dynamicUploadEpoch = state.uploadEpoch;
			residency.dynamicBaseVertex = baseVertex;
			residency.dynamicVertexCount = vertexCount;
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
			const NativeA8PayloadResidencyCache& residency =
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
			UInt32 generation)
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
				if (!staticResident)
					ObserveStaticCandidateLocked(state, payloadTemplate,
						vertexCount);
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
		std::lock_guard<std::mutex> lock(state.mutex);
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
		if (g_bDisableFreeTypeExtendedCaches)
		{
			if (state.activeSubmissions.load(std::memory_order_acquire))
				return;
			state.nextVertex = 0;
			state.uploadedPayloads.clear();
			state.uploadedPayloads.rehash(0);
			state.staticCandidates.clear();
			state.staticPayloads.clear();
			s_ringThread.uploadedPayload = {};
			s_ringThread.staticCandidate = {};
			s_ringThread.staticPayload = {};
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
		auto isBatchPromotionReady = [&state](
			const NativeA8PayloadTemplatePtr& payloadTemplate)
		{
			auto found = state.staticCandidates.find(payloadTemplate.get());
			if (found == state.staticCandidates.end() || !found->second)
				return false;
			NativeA8PayloadTemplatePtr owner = found->second->owner.lock();
			if (owner.get() != payloadTemplate.get())
			{
				state.staticCandidates.erase(found);
				return false;
			}
			return !found->second->promotionDisabled
				&& found->second->submissionCount
					>= kStaticPromotionSubmissionCount;
		};

		if (state.staticVertexBuffer)
		{
			UInt64 requestedVertices = 0;
			for (const NativeA8PayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				UInt32 baseVertex = 0;
				if (ResolveStaticPayloadLocked(state, payloadTemplate,
					vertexCount, baseVertex)
					|| !isBatchPromotionReady(payloadTemplate))
				{
					continue;
				}
				requestedVertices += vertexCount;
			}

			const UInt64 availableVertices = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			if (requestedVertices > availableVertices
				&& requestedVertices <= std::numeric_limits<UInt32>::max())
			{
				bool permanentFailure = false;
				TryGrowStaticVertexBufferLocked(state,
					static_cast<UInt32>(requestedVertices), permanentFailure);
			}

			const UInt32 staticAvailable = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			UInt32 selectedVertices = 0;
			size_t selectedPayloads = 0;
			for (const NativeA8PayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				if (HasDirectStaticPayloadLocked(state, *payloadTemplate,
						vertexCount)
					|| !isBatchPromotionReady(payloadTemplate)
					|| vertexCount > staticAvailable - selectedVertices)
				{
					continue;
				}
				selectedVertices += vertexCount;
				++selectedPayloads;
			}

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
					for (const NativeA8PayloadTemplatePtr& payloadTemplate
						: payloadTemplates)
					{
						if (!isValidPayload(payloadTemplate))
							continue;
						const UInt32 vertexCount = static_cast<UInt32>(
							payloadTemplate->gpuVertices.size());
						if (HasDirectStaticPayloadLocked(state,
								*payloadTemplate, vertexCount)
							|| !isBatchPromotionReady(payloadTemplate)
							|| vertexCount > staticAvailable - copiedVertices)
						{
							continue;
						}
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
						const UInt32 resourceSerial =
							state.resourceSerial.load(
								std::memory_order_relaxed);
						UInt32 mappedVertices = 0;
						for (const NativeA8PayloadTemplatePtr& payloadTemplate
							: payloadTemplates)
						{
							if (!isValidPayload(payloadTemplate))
								continue;
							const UInt32 vertexCount = static_cast<UInt32>(
								payloadTemplate->gpuVertices.size());
							if (HasDirectStaticPayloadLocked(state,
									*payloadTemplate, vertexCount)
								|| !isBatchPromotionReady(payloadTemplate)
								|| vertexCount
									> staticAvailable - mappedVertices)
							{
								continue;
							}
							const UInt32 baseVertex =
								state.nextStaticVertex + mappedVertices;
							state.staticPayloads[payloadTemplate.get()] = {
								payloadTemplate, baseVertex, vertexCount };
							NativeA8PayloadResidencyCache& residency =
								payloadTemplate->residency;
							residency.staticResourceSerial = resourceSerial;
							residency.staticBaseVertex = baseVertex;
							residency.staticVertexCount = vertexCount;
							state.staticCandidates.erase(payloadTemplate.get());
							if (s_ringThread.staticCandidate.key
								== payloadTemplate.get())
							{
								s_ringThread.staticCandidate = {};
							}
							mappedVertices += vertexCount;
						}
						state.nextStaticVertex += selectedVertices;
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
						RecordFreeTypePerf(
							FreeTypePerfCounter::StaticVertexPromotionFailed);
					}
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
				}
			}
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
			if (ResolveStaticPayloadLocked(state, payloadTemplate,
				vertexCount, baseVertex))
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
			++state.uploadEpoch;
			if (!state.uploadEpoch)
				++state.uploadEpoch;
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
				"tnvse_freetype_native: sorted dynamic batch generation=%u payloads=%u vertices=%u bytes=%u discard=%u residentVertices=%u capacity=%u",
				generation, static_cast<UInt32>(uploadPayloads),
				uploadVertices, byteCount, discard ? 1u : 0u,
				state.nextVertex, state.vertexCapacity);
		}
		PublishSortedRingLeaseLocked(state, payloadTemplates,
			generation);
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
		if (!staticResident && !State().sortedTileRenderHookInstalled)
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
					++state.uploadEpoch;
					if (!state.uploadEpoch)
						++state.uploadEpoch;
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

				state.nextVertex = startVertex + totalVertices;
				if (!g_bDisableFreeTypeExtendedCaches)
				{
					PublishUploadedPayloadLocked(state, payload.payloadTemplate,
						startVertex, totalVertices);
				}
				RefreshRingCpuMemoryLocked(state);
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
			}

			ObserveStaticCandidateLocked(state, payload.payloadTemplate,
				totalVertices);
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
			&& (g_bDisableFreeTypeExtendedCaches
				|| state->releasePending.load(std::memory_order_acquire)))
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->sortedFrameLeases.load(std::memory_order_acquire)
				&& !state->activeSubmissions.load(std::memory_order_acquire))
			{
				if (g_bDisableFreeTypeExtendedCaches)
				{
					state->uploadedPayloads.clear();
					state->uploadedPayloads.rehash(0);
					state->nextVertex = 0;
					s_ringThread.uploadedPayload = {};
				}
				if (state->releasePending.load(std::memory_order_acquire))
					ReleaseRingResourcesLocked(*state);
			}
		}
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
