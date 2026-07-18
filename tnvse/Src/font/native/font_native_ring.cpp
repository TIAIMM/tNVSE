#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "BSShaderProperty.hpp"
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

		struct NativeA8Proxy
		{
			NiTriShapePtr shape;
			NiGeometryBufferData* buffer = nullptr;
			NiVBChip* chip = nullptr;
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
			std::weak_ptr<const NativeA8PayloadTemplate> owner;
			UInt32 submissionCount = 0;
			bool promotionDisabled = false;
		};

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
				NativeA8StaticCandidate> staticCandidates;
			std::atomic<UInt32> resourceSerial = 1;
			bool loggedReady = false;
		};

		NativeA8RingState& RingState()
		{
			// Renderer generations are process-lifetime objects. Keep the equally small
			// proxy pool alive for the same interval so raw geometry/shader links cannot
			// be torn down during late engine shutdown.
			static NativeA8RingState* state = new NativeA8RingState();
			return *state;
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

		void ReleaseRingResourcesLocked(NativeA8RingState& state)
		{
			++state.uploadEpoch;
			if (!state.uploadEpoch)
				++state.uploadEpoch;
			state.uploadedPayloads.clear();
			state.staticPayloads.clear();
			state.staticCandidates.clear();
			state.resourceSerial.fetch_add(1, std::memory_order_release);
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
			NativeA8ShapePayload& payload, UInt32 requiredVertices,
			const char*& operation, HRESULT& result)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			const UInt32 generation = GetNativeA8ShaderGeneration();
			if (!renderer || !device || !generation
				|| generation != payload.preparedGeneration)
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
				ReleaseRingResourcesLocked(state);

			const UInt64 capLimit = static_cast<UInt64>(
				renderer->m_kD3DCaps9.MaxVertexIndex) + 1u;
			UInt64 desired = std::min<UInt64>(kRingTargetVertexCapacity, capLimit);
			desired &= ~static_cast<UInt64>(3u);
			UInt64 staticDesired = std::min<UInt64>(
				kStaticTargetVertexCapacity, capLimit);
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

		bool BindPacketAtlasPage(NiTriShape& shape,
			const A8ShapeMetadata& metadata, UInt16 page)
		{
			if (page >= metadata.effects.atlasProperties.size()
				|| page >= metadata.effects.atlasTextures.size()
				|| !metadata.effects.atlasProperties[page]
				|| !metadata.effects.atlasTextures[page])
			{
				return false;
			}

			NiTexturingProperty* desiredProperty =
				metadata.effects.atlasProperties[page].m_pObject;
			NiTexture* desiredTexture = metadata.effects.atlasTextures[page].m_pObject;
			if (shape.GetTexturingProperty() != desiredProperty)
			{
				shape.RemoveProperty(NiProperty::TEXTURING);
				shape.AddProperty(desiredProperty);
				shape.UpdateProperties();
			}
			TileShaderPropertyView* tile = GetTileProperty(&shape);
			if (!tile)
				return false;
			if (tile->sourceTexture.m_pObject != desiredTexture)
				ThisStdCall(0xBB7A10, tile, desiredTexture);
			return tile->sourceTexture.m_pObject == desiredTexture;
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

		bool SyncProxyState(const NiTriShape& facade, NiTriShape& proxy,
			const NiPoint3& geometryOrigin)
		{
			const TileShaderPropertyView* sourceTile = GetTileProperty(&facade);
			TileShaderPropertyView* proxyTile = GetTileProperty(&proxy);
			if (!sourceTile || !proxyTile || !proxyTile->sourceTexture
				|| !proxy.GetTexturingProperty())
			{
				return false;
			}

			ApplyRelativeOrigin(proxy.m_kLocal, facade.m_kLocal, geometryOrigin);
			ApplyRelativeOrigin(proxy.m_kWorld, facade.m_kWorld, geometryOrigin);
			CopyScissorTail(facade, proxy);

			if (facade.m_pWorldBound)
			{
				if (!proxy.m_pWorldBound)
					proxy.CreateWorldBoundIfMissing();
				if (!proxy.m_pWorldBound)
					return false;
				*proxy.m_pWorldBound = *facade.m_pWorldBound;
			}
			proxy.m_uiFlags = facade.m_uiFlags;
			proxy.m_kProperties.m_spAlphaProperty =
				facade.m_kProperties.m_spAlphaProperty;
			proxy.m_kProperties.m_spCullingProperty =
				facade.m_kProperties.m_spCullingProperty;
			proxy.m_kProperties.m_spMaterialProperty =
				facade.m_kProperties.m_spMaterialProperty;
			proxy.m_kProperties.m_spStencilProperty =
				facade.m_kProperties.m_spStencilProperty;
			proxy.m_kProperties.m_spUnknownProperty =
				facade.m_kProperties.m_spUnknownProperty;
			CopyTileDynamicState(*sourceTile, *proxyTile);
			return true;
		}

		UInt32 AcquireProxyLocked(NativeA8RingState& state)
		{
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (!state.proxies[index].inUse)
				{
					state.proxies[index].inUse = true;
					return index;
				}
			}
			return std::numeric_limits<UInt32>::max();
		}

		bool ResolveStaticPayloadLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			if (!state.staticVertexBuffer)
				return false;
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
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				return true;
			}
			state.staticPayloads.erase(found);
			return false;
		}

		NativeA8StaticCandidate* ResolveStaticCandidateLocked(
			NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			if (!state.staticVertexBuffer || vertexCount > state.staticVertexCapacity)
				return nullptr;
			auto found = state.staticCandidates.find(payloadTemplate.get());
			if (found == state.staticCandidates.end())
			{
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					for (auto current = state.staticCandidates.begin();
						current != state.staticCandidates.end();)
					{
						if (current->second.owner.expired())
							current = state.staticCandidates.erase(current);
						else
							++current;
					}
					if (state.staticCandidates.size() >= kStaticCandidateLimit)
						return nullptr;
				}
				NativeA8StaticCandidate candidate;
				candidate.owner = payloadTemplate;
				found = state.staticCandidates.emplace(
					payloadTemplate.get(), std::move(candidate)).first;
			}
			else
			{
				const std::shared_ptr<const NativeA8PayloadTemplate> owner =
					found->second.owner.lock();
				if (owner.get() != payloadTemplate.get())
				{
					found->second = NativeA8StaticCandidate{};
					found->second.owner = payloadTemplate;
				}
			}
			return &found->second;
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
				candidate->promotionDisabled = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
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
			state.staticCandidates.erase(payloadTemplate.get());
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUploadBytes,
				byteCount);
			return true;
		}

		void ObserveStaticCandidateLocked(NativeA8RingState& state,
			const NativeA8PayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			if (state.nextStaticVertex >= state.staticVertexCapacity)
				return;
			NativeA8StaticCandidate* candidate = ResolveStaticCandidateLocked(
				state, payloadTemplate, vertexCount);
			if (candidate && !candidate->promotionDisabled
				&& candidate->submissionCount
					< std::numeric_limits<UInt32>::max())
			{
				++candidate->submissionCount;
			}
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
			state.proxies[state.proxyCount++] = std::move(proxy);
		}
		return state.proxyCount != 0;
	}

	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload, NativeA8RingSubmission& submission)
	{
		EndNativeA8RingSubmission(submission);
		if (!facade || !payload.buildComplete || !payload.payloadTemplate
			|| payload.packets.empty()
			|| payload.packets.size() != payload.payloadTemplate->packets.size())
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		UInt64 nextVertex = 0;
		for (const NativeA8Packet& packet : payload.packets)
		{
			if (packet.templateIndex >= payload.payloadTemplate->packets.size())
				return NativeA8FallbackReason::PacketBuild;
			const NativeA8PacketTemplate& source =
				payload.payloadTemplate->packets[packet.templateIndex];
			const UInt64 vertexEnd = static_cast<UInt64>(source.firstVertex)
				+ source.vertexCount;
			if (!source.vertexCount || (source.vertexCount & 3u)
				|| source.vertexCount / 4u > kNativeA8MaximumQuads
				|| source.firstVertex != nextVertex
				|| vertexEnd > payload.payloadTemplate->gpuVertices.size())
			{
				return NativeA8FallbackReason::PacketBuild;
			}
			nextVertex = vertexEnd;
		}
		if (!nextVertex || nextVertex != payload.payloadTemplate->gpuVertices.size()
			|| nextVertex > std::numeric_limits<UInt32>::max())
			return NativeA8FallbackReason::PacketBuild;
		const UInt32 totalVertices = static_cast<UInt32>(nextVertex);

		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const UInt32 proxyIndex = AcquireProxyLocked(state);
		if (proxyIndex == std::numeric_limits<UInt32>::max())
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::ProxyUnavailable,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PacketPrepare;
		}

		const char* operation = "ring-resource";
		HRESULT result = D3DERR_DEVICELOST;
		if (!EnsureRingResourcesLocked(state, payload, totalVertices,
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
			else if (operation && (std::strcmp(operation, "CreateIndexBuffer") == 0
				|| std::strcmp(operation, "canonical-index-upload") == 0))
			{
				prepareFailure = NativeA8PacketPrepareFailure::IndexBuffer;
			}
			payload.packetPrepareFailure.store(prepareFailure,
				std::memory_order_relaxed);
			if (prepareFailure != NativeA8PacketPrepareFailure::RingCapacity)
			{
				MarkNativeA8GenerationFault(payload.preparedGeneration,
					operation, result);
			}
			return NativeA8FallbackReason::PacketPrepare;
		}

		UInt32 startVertex = 0;
		bool staticResident = ResolveStaticPayloadLocked(state,
			payload.payloadTemplate, totalVertices, startVertex);
		if (!staticResident)
		{
			staticResident = PromoteStaticPayloadLocked(state,
				payload.payloadTemplate, totalVertices, startVertex);
		}

		if (!staticResident)
		{
			bool reusedUpload = false;
			auto uploaded = state.uploadedPayloads.find(
				payload.payloadTemplate.get());
			if (uploaded != state.uploadedPayloads.end())
			{
				const std::shared_ptr<const NativeA8PayloadTemplate> owner =
					uploaded->second.owner.lock();
				if (owner.get() == payload.payloadTemplate.get()
					&& uploaded->second.epoch == state.uploadEpoch
					&& uploaded->second.vertexCount == totalVertices
					&& uploaded->second.baseVertex <= state.vertexCapacity
					&& totalVertices <= state.vertexCapacity
						- uploaded->second.baseVertex)
				{
					startVertex = uploaded->second.baseVertex;
					reusedUpload = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::DynamicVertexReuse);
				}
				else
				{
					state.uploadedPayloads.erase(uploaded);
				}
			}

			if (!reusedUpload)
			{
				startVertex = state.nextVertex;
				DWORD lockFlags = D3DLOCK_NOOVERWRITE;
				if (!startVertex
					|| totalVertices > state.vertexCapacity - startVertex)
				{
					startVertex = 0;
					lockFlags = D3DLOCK_DISCARD;
					++state.uploadEpoch;
					if (!state.uploadEpoch)
						++state.uploadEpoch;
					state.uploadedPayloads.clear();
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
				state.uploadedPayloads[payload.payloadTemplate.get()] = {
					payload.payloadTemplate, startVertex, totalVertices,
					state.uploadEpoch };
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
			}

			ObserveStaticCandidateLocked(state, payload.payloadTemplate,
				totalVertices);
		}

		NativeA8Proxy& proxy = state.proxies[proxyIndex];
		if (!proxy.shape || !proxy.buffer || !proxy.chip
			|| !SyncProxyState(*facade, *proxy.shape, payload.geometryOrigin))
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
		payload.packetPrepareFailure.store(
			NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		return NativeA8FallbackReason::None;
	}

	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload, NativeA8RingSubmission& submission,
		UInt32 packetIndex, NiTriShape*& proxyShape)
	{
		proxyShape = nullptr;
		if (!submission.active || !facade || !submission.proxyShape
			|| packetIndex != submission.nextPacket
			|| packetIndex >= payload.packets.size()
			|| submission.generation != payload.preparedGeneration
			|| !IsNativeA8ShaderGenerationCurrent(submission.generation)
			|| !payload.payloadTemplate)
		{
			return NativeA8FallbackReason::RuntimeFault;
		}

		NativeA8Packet& packet = payload.packets[packetIndex];
		if (!packet.shader || packet.templateIndex
			>= payload.payloadTemplate->packets.size()
			|| packet.atlasPage >= metadata.effects.atlasTextures.size())
		{
			return NativeA8FallbackReason::AtlasGeneration;
		}
		const NativeA8PacketTemplate& source =
			payload.payloadTemplate->packets[packet.templateIndex];
		const UInt32 vertexCount = source.vertexCount;
		const UInt64 baseVertex = static_cast<UInt64>(
			submission.payloadBaseVertex) + source.firstVertex;
		if (!vertexCount || (vertexCount & 3u)
			|| baseVertex + vertexCount > submission.endVertex)
		{
			return NativeA8FallbackReason::PacketBuild;
		}

		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
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
			|| !BindPacketAtlasPage(*proxy, metadata, packet.atlasPage))
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
		proxy->SetShader(packet.shader);
		if (proxy->GetShader() != packet.shader)
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::ShaderBinding,
				std::memory_order_relaxed);
			return NativeA8FallbackReason::PacketPrepare;
		}

		++submission.nextPacket;
		proxyShape = proxy;
		return NativeA8FallbackReason::None;
	}

	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission)
	{
		if (submission.active)
		{
			NativeA8RingState& state = RingState();
			std::lock_guard<std::mutex> lock(state.mutex);
			if (submission.proxyIndex < state.proxyCount)
				state.proxies[submission.proxyIndex].inUse = false;
		}
		submission = NativeA8RingSubmission{};
	}

	void ReleaseNativeA8RingResources()
	{
		NativeA8RingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		ReleaseRingResourcesLocked(state);
	}
}
