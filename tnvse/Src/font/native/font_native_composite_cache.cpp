#include "font_atlas_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiMaterialProperty.hpp"
#include "NiTexturingProperty.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kStableObservationCount = 3;
		inline constexpr UInt32 kMaximumGenerationsPerFrame = 2;
		inline constexpr UInt64 kMaximumGeneratedTexelsPerFrame = 2ull * 1024ull
			* 1024ull;
		inline constexpr UInt64 kMaximumEntryTexels = 2ull * 1024ull * 1024ull;
		inline constexpr UInt64 kFailedGenerationCooldownFrames = 60;
		inline constexpr float kMinimumClipW = 1.0e-5f;
		inline constexpr size_t kMaximumPendingValidations = 8;
		inline constexpr UInt64 kValidationTimeoutFrames = 120;

		struct CompositeCacheKey
		{
			const NativeA8PayloadTemplate* artifact = nullptr;
			std::array<UInt8, 256> state = {};
			UInt16 size = 0;

			bool operator==(const CompositeCacheKey& other) const
			{
				return artifact == other.artifact && size == other.size
					&& std::memcmp(state.data(), other.state.data(), size) == 0;
			}
		};

		struct CompositeCacheKeyHash
		{
			size_t operator()(const CompositeCacheKey& key) const
			{
				size_t hash = 2166136261u;
				auto mixByte = [&](UInt8 value)
				{
					hash ^= value;
					hash *= 16777619u;
				};
				const uintptr_t artifact =
					reinterpret_cast<uintptr_t>(key.artifact);
				for (size_t index = 0; index < sizeof(artifact); ++index)
					mixByte(static_cast<UInt8>(artifact >> (index * 8u)));
				for (UInt16 index = 0; index < key.size; ++index)
					mixByte(key.state[index]);
				return hash;
			}
		};

		class CompositeCacheKeyBuilder
		{
		public:
			explicit CompositeCacheKeyBuilder(
				const NativeA8PayloadTemplate* artifact)
			{
				m_key.artifact = artifact;
			}

			template <class T>
			void Add(const T& value)
			{
				AddBytes(&value, sizeof(value));
			}

			void AddBytes(const void* source, size_t size)
			{
				if (!m_valid || !source
					|| size > m_key.state.size() - m_key.size)
				{
					m_valid = false;
					return;
				}
				std::memcpy(m_key.state.data() + m_key.size, source, size);
				m_key.size = static_cast<UInt16>(m_key.size + size);
			}

			bool Finish(CompositeCacheKey& result) const
			{
				if (!m_valid || !m_key.artifact || !m_key.size)
					return false;
				result = m_key;
				return true;
			}

		private:
			CompositeCacheKey m_key;
			bool m_valid = true;
		};

		struct CompositeObservation
		{
			CompositeCacheKey key;
			IDirect3DDevice9* device = nullptr;
			IDirect3DSurface9* renderTargetIdentity = nullptr;
			D3DVIEWPORT9 viewport = {};
			D3DSURFACE_DESC targetDescription = {};
			UInt32 generation = 0;
			UInt32 atlasEpoch = 0;
		};

		struct NativeA8CompositeCacheEntry
		{
			CompositeCacheKey key;
			NativeA8PayloadTemplatePtr sourceOwner;
			NativeA8PayloadTemplatePtr cachedArtifact;
			NativeA8ShapePayload cachedPayload;
			UInt64 bytes = 0;
			UInt64 lastUse = 0;
			bool valid = false;
		};

		struct PendingCompositeValidation
		{
			NativeA8PayloadTemplatePtr sourceOwner;
			IDirect3DQuery9* query = nullptr;
			UInt32 generation = 0;
			UInt64 submittedFrame = 0;
		};

		struct CompositeCacheState
		{
			std::mutex mutex;
			std::unordered_map<CompositeCacheKey,
				std::shared_ptr<NativeA8CompositeCacheEntry>,
				CompositeCacheKeyHash> entries;
			std::vector<PendingCompositeValidation> pendingValidations;
			UInt64 bytes = 0;
			UInt64 frameSerial = 0;
			UInt64 useSerial = 0;
			UInt64 generatedTexelsThisFrame = 0;
			UInt32 generatedItemsThisFrame = 0;
			bool frameActive = false;
		};

		CompositeCacheState& CacheState()
		{
			static CompositeCacheState* state = new CompositeCacheState();
			return *state;
		}

		void ClearPendingValidation(PendingCompositeValidation& pending)
		{
			if (pending.sourceOwner)
			{
				UInt32 expected = pending.generation;
				pending.sourceOwner->compositeValidationPendingGeneration
					.compare_exchange_strong(expected, 0,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
			}
			if (pending.query)
			{
				pending.query->Release();
				pending.query = nullptr;
			}
			pending.sourceOwner.reset();
		}

		void CompletePendingValidation(PendingCompositeValidation& pending,
			bool rejected, bool inconclusive)
		{
			if (pending.sourceOwner)
			{
				if (rejected)
				pending.sourceOwner->compositeRejectedGeneration.store(
					pending.generation, std::memory_order_release);
				else
					pending.sourceOwner->compositeValidatedGeneration.store(
						pending.generation, std::memory_order_release);
			}
			RecordFreeTypePerf(rejected
				? (inconclusive
					? FreeTypePerfCounter::CompositeVisualInconclusive
					: FreeTypePerfCounter::CompositeVisualRejected)
				: FreeTypePerfCounter::CompositeVisualValidated);
			ClearPendingValidation(pending);
		}

		void PollPendingValidationsLocked(CompositeCacheState& state)
		{
			const UInt32 currentGeneration = GetNativeA8ShaderGeneration();
			for (auto pending = state.pendingValidations.begin();
				pending != state.pendingValidations.end();)
			{
				if (!pending->sourceOwner || !pending->query
					|| pending->generation != currentGeneration)
				{
					ClearPendingValidation(*pending);
					pending = state.pendingValidations.erase(pending);
					continue;
				}

				DWORD visiblePixels = 0;
				// A zero flag is essential here. D3DGETDATA_FLUSH can synchronously
				// wait for the GPU/driver and was capable of freezing a menu-opening
				// frame while many new text artifacts became stable together.
				const HRESULT result = pending->query->GetData(
					&visiblePixels, sizeof(visiblePixels), 0);
				if (result == S_FALSE
					&& state.frameSerial - pending->submittedFrame
						< kValidationTimeoutFrames)
				{
					++pending;
					continue;
				}

				if (result == S_OK)
					CompletePendingValidation(
						*pending, visiblePixels != 0, false);
				else
					CompletePendingValidation(*pending, true, true);
				pending = state.pendingValidations.erase(pending);
			}
		}

		void ClearPendingValidationsLocked(CompositeCacheState& state)
		{
			for (PendingCompositeValidation& pending :
				state.pendingValidations)
				ClearPendingValidation(pending);
			state.pendingValidations.clear();
		}

		struct BakeWvpState
		{
			std::array<float, 16> wvp = {};
			UInt32 depth = 0;
		};

		thread_local BakeWvpState s_bakeWvp;

		UInt64 CacheBudgetBytes()
		{
			return static_cast<UInt64>(
				std::min<UInt32>(g_uiFreeTypeFontCompositeCacheMB, 512u))
				* 1024ull * 1024ull;
		}

		void InvalidateEntryLocked(
			const std::shared_ptr<NativeA8CompositeCacheEntry>& entry,
			CompositeCacheState& state)
		{
			if (!entry || !entry->valid)
				return;
			state.bytes -= std::min(state.bytes, entry->bytes);
			entry->valid = false;
			entry->bytes = 0;
			entry->cachedPayload.buildComplete = false;
			entry->cachedPayload.packetShaders.clear();
			entry->cachedPayload.preflightAtlasTextures.clear();
			entry->cachedPayload.payloadTemplate.reset();
			entry->cachedArtifact.reset();
			entry->sourceOwner.reset();
		}

		void TrimCacheLocked(CompositeCacheState& state, UInt64 incomingBytes,
			const CompositeCacheKey* protectedKey = nullptr)
		{
			const UInt64 budget = CacheBudgetBytes();
			while (!state.entries.empty()
				&& (state.bytes > budget
					|| incomingBytes > budget - std::min(budget, state.bytes)))
			{
				auto victim = state.entries.end();
				for (auto current = state.entries.begin();
					current != state.entries.end(); ++current)
				{
					if (protectedKey && current->first == *protectedKey)
						continue;
					if (victim == state.entries.end()
						|| current->second->lastUse
							< victim->second->lastUse)
					{
						victim = current;
					}
				}
				if (victim == state.entries.end())
					break;
				InvalidateEntryLocked(victim->second, state);
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeCacheEvicted);
				state.entries.erase(victim);
			}
		}

		bool BuildObservation(NiTriShape* facade,
			NativeA8ShapePayload& sourcePayload,
			CompositeObservation& observation)
		{
			observation = {};
			if (!facade || !sourcePayload.payloadTemplate
				|| !sourcePayload.buildComplete
				|| !g_bEnableFreeTypeFontCompositePass)
			{
				return false;
			}
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!renderer || !device
				|| !(renderer->m_kD3DCaps9.PrimitiveMiscCaps
					& D3DPMISCCAPS_SEPARATEALPHABLEND))
			{
				return false;
			}
			const UInt32 generation = GetNativeA8ShaderGeneration();
			const UInt32 atlasEpoch = GetNativeA8AtlasTextureEpoch();
			if (!generation || !atlasEpoch
				|| generation != sourcePayload.preparedGeneration)
			{
				return false;
			}

			IDirect3DSurface9* renderTarget = nullptr;
			if (FAILED(device->GetRenderTarget(0, &renderTarget))
				|| !renderTarget)
				return false;
			D3DSURFACE_DESC description = {};
			D3DVIEWPORT9 viewport = {};
			const HRESULT descriptionResult =
				renderTarget->GetDesc(&description);
			const HRESULT viewportResult = device->GetViewport(&viewport);
			IDirect3DSurface9* const renderTargetIdentity = renderTarget;
			renderTarget->Release();
			if (FAILED(descriptionResult) || FAILED(viewportResult)
				|| !viewport.Width || !viewport.Height
				|| description.MultiSampleType != D3DMULTISAMPLE_NONE
				|| (description.Format != D3DFMT_A8R8G8B8
					&& description.Format != D3DFMT_X8R8G8B8))
			{
				return false;
			}

			NiShadeProperty* shade = facade->GetShadeProperty();
			if (!shade || shade->m_eShaderType != NiShadeProperty::PROP_Tile)
				return false;
			const BSShaderProperty* tile =
				static_cast<const BSShaderProperty*>(shade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const NiMaterialProperty* material = facade->GetMaterialProperty();
			if (!alpha || !alpha->GetAlphaBlending()
				|| alpha->m_usFlags.GetField(
					NiAlphaProperty::SRC_BLEND_MASK,
					NiAlphaProperty::SRC_BLEND_POS)
					!= NiAlphaProperty::ALPHA_SRCALPHA
				|| alpha->m_usFlags.GetField(
					NiAlphaProperty::DEST_BLEND_MASK,
					NiAlphaProperty::DEST_BLEND_POS)
					!= NiAlphaProperty::ALPHA_INVSRCALPHA)
			{
				// The RTT stores the result as premultiplied RGBA and the cache-hit
				// profile necessarily uses ONE/INVSRCALPHA.  Other source blend
				// contracts cannot be folded into that representation losslessly.
				return false;
			}

			CompositeCacheKeyBuilder builder(
				sourcePayload.payloadTemplate.get());
			builder.Add(device);
			builder.Add(renderTargetIdentity);
			builder.Add(generation);
			builder.Add(atlasEpoch);
			builder.Add(sourcePayload.useCompositePackets);
			builder.Add(sourcePayload.geometryOrigin);
			builder.Add(facade->m_kWorld);
			builder.Add(viewport);
			builder.Add(description.Width);
			builder.Add(description.Height);
			builder.Add(description.Format);
			builder.Add(description.MultiSampleType);
			builder.Add(description.MultiSampleQuality);
			builder.Add(tile->ulFlags);
			builder.Add(tile->fAlpha);
			builder.Add(tile->fFadeAlpha);
			builder.Add(tile->fDepthBias);
			// Retail Tile constants and UV state occupy this fixed tail of the
			// concrete property.  Scissor starts later and is intentionally omitted.
			builder.AddBytes(reinterpret_cast<const UInt8*>(tile) + 0x68,
				0x94 - 0x68);
			if (alpha)
			{
				builder.Add(alpha->m_usFlags);
				builder.Add(alpha->m_ucAlphaTestRef);
			}
			else
			{
				const UInt32 noAlphaProperty = 0;
				builder.Add(noAlphaProperty);
			}
			const float materialAlpha = material ? material->m_fAlpha : 1.0f;
			builder.Add(materialAlpha);
			if (!builder.Finish(observation.key))
				return false;

			observation.device = device;
			observation.renderTargetIdentity = renderTargetIdentity;
			observation.viewport = viewport;
			observation.targetDescription = description;
			observation.generation = generation;
			observation.atlasEpoch = atlasEpoch;
			return true;
		}

		bool SameObservation(const CompositeObservation& observation,
			const NativeA8CompositeCacheTracker& tracker);

		void BeginBakeWvp(const std::array<float, 16>& wvp)
		{
			if (!s_bakeWvp.depth++)
				s_bakeWvp.wvp = wvp;
		}

		void EndBakeWvp()
		{
			if (!s_bakeWvp.depth)
				return;
			if (!--s_bakeWvp.depth)
				s_bakeWvp.wvp = {};
		}

		class CompositeBakeStateGuard
		{
		public:
			explicit CompositeBakeStateGuard(IDirect3DDevice9* device)
				: m_device(device)
			{
				if (!m_device
					|| FAILED(m_device->GetRenderTarget(0, &m_renderTarget))
					|| !m_renderTarget
					|| FAILED(m_device->GetViewport(&m_viewport))
					|| FAILED(m_device->GetScissorRect(&m_scissor))
					|| FAILED(m_device->GetVertexShaderConstantF(
						0, m_vertexConstants.data(), 4))
					|| FAILED(m_device->GetPixelShaderConstantF(
						0, m_pixelConstants.data(), 9))
					|| FAILED(m_device->CreateStateBlock(
						D3DSBT_ALL, &m_stateBlock))
					|| !m_stateBlock
					|| FAILED(m_stateBlock->Capture()))
				{
					return;
				}
				const HRESULT depthResult =
					m_device->GetDepthStencilSurface(&m_depthSurface);
				if (FAILED(depthResult) && depthResult != D3DERR_NOTFOUND)
					return;
				m_captured = true;
			}

			~CompositeBakeStateGuard()
			{
				Restore();
				if (m_stateBlock)
					m_stateBlock->Release();
				if (m_depthSurface)
					m_depthSurface->Release();
				if (m_renderTarget)
					m_renderTarget->Release();
			}

			bool Captured() const
			{
				return m_captured;
			}

			IDirect3DSurface9* RenderTarget() const
			{
				return m_renderTarget;
			}

			const D3DVIEWPORT9& Viewport() const
			{
				return m_viewport;
			}

			const std::array<float, 16>& VertexConstants() const
			{
				return m_vertexConstants;
			}

			bool Restore()
			{
				if (m_restored)
					return m_restoreSucceeded;
				m_restored = true;
				if (!m_captured || !m_device)
					return false;
				bool succeeded = SUCCEEDED(m_stateBlock->Apply());
				succeeded = SUCCEEDED(m_device->SetRenderTarget(
					0, m_renderTarget)) && succeeded;
				succeeded = SUCCEEDED(m_device->SetDepthStencilSurface(
					m_depthSurface)) && succeeded;
				succeeded = SUCCEEDED(m_device->SetViewport(&m_viewport))
					&& succeeded;
				succeeded = SUCCEEDED(m_device->SetScissorRect(&m_scissor))
					&& succeeded;
				succeeded = SUCCEEDED(m_device->SetVertexShaderConstantF(
					0, m_vertexConstants.data(), 4)) && succeeded;
				succeeded = SUCCEEDED(m_device->SetPixelShaderConstantF(
					0, m_pixelConstants.data(), 9)) && succeeded;
				m_restoreSucceeded = succeeded;
				return succeeded;
			}

		private:
			IDirect3DDevice9* m_device = nullptr;
			IDirect3DStateBlock9* m_stateBlock = nullptr;
			IDirect3DSurface9* m_renderTarget = nullptr;
			IDirect3DSurface9* m_depthSurface = nullptr;
			D3DVIEWPORT9 m_viewport = {};
			RECT m_scissor = {};
			std::array<float, 16> m_vertexConstants = {};
			std::array<float, 36> m_pixelConstants = {};
			bool m_captured = false;
			bool m_restored = false;
			bool m_restoreSucceeded = false;
		};

		bool ProjectArtifactBounds(const NativeA8PayloadTemplate& artifact,
			const std::array<float, 16>& wvp, const D3DVIEWPORT9& viewport,
			const D3DSURFACE_DESC& target, RECT& bounds)
		{
			float minimumX = std::numeric_limits<float>::max();
			float minimumY = std::numeric_limits<float>::max();
			float maximumX = std::numeric_limits<float>::lowest();
			float maximumY = std::numeric_limits<float>::lowest();
			for (const NativeA8GpuVertex& vertex : artifact.gpuVertices)
			{
				const float x = vertex.x;
				const float y = vertex.y;
				const float z = vertex.z;
				const float clipX = x * wvp[0] + y * wvp[1]
					+ z * wvp[2] + wvp[3];
				const float clipY = x * wvp[4] + y * wvp[5]
					+ z * wvp[6] + wvp[7];
				const float clipW = x * wvp[12] + y * wvp[13]
					+ z * wvp[14] + wvp[15];
				if (!std::isfinite(clipX) || !std::isfinite(clipY)
					|| !std::isfinite(clipW) || clipW <= kMinimumClipW)
				{
					return false;
				}
				const float screenX = static_cast<float>(viewport.X)
					+ (clipX / clipW + 1.0f)
						* static_cast<float>(viewport.Width) * 0.5f;
				const float screenY = static_cast<float>(viewport.Y)
					+ (1.0f - clipY / clipW)
						* static_cast<float>(viewport.Height) * 0.5f;
				minimumX = std::min(minimumX, screenX);
				minimumY = std::min(minimumY, screenY);
				maximumX = std::max(maximumX, screenX);
				maximumY = std::max(maximumY, screenY);
			}
			if (!std::isfinite(minimumX) || !std::isfinite(minimumY)
				|| !std::isfinite(maximumX) || !std::isfinite(maximumY))
				return false;

			const LONG viewportRight = static_cast<LONG>(
				std::min<UInt64>(target.Width,
					static_cast<UInt64>(viewport.X) + viewport.Width));
			const LONG viewportBottom = static_cast<LONG>(
				std::min<UInt64>(target.Height,
					static_cast<UInt64>(viewport.Y) + viewport.Height));
			bounds.left = std::max<LONG>(static_cast<LONG>(viewport.X),
				static_cast<LONG>(std::floor(minimumX)) - 1);
			bounds.top = std::max<LONG>(static_cast<LONG>(viewport.Y),
				static_cast<LONG>(std::floor(minimumY)) - 1);
			bounds.right = std::min<LONG>(viewportRight,
				static_cast<LONG>(std::ceil(maximumX)) + 1);
			bounds.bottom = std::min<LONG>(viewportBottom,
				static_cast<LONG>(std::ceil(maximumY)) + 1);
			return bounds.right > bounds.left && bounds.bottom > bounds.top;
		}

		std::array<float, 16> BuildBakeWvp(
			const std::array<float, 16>& source,
			const D3DVIEWPORT9& viewport, const RECT& bounds)
		{
			const float width = static_cast<float>(bounds.right - bounds.left);
			const float height = static_cast<float>(bounds.bottom - bounds.top);
			const float scaleX = static_cast<float>(viewport.Width) / width;
			const float offsetX =
				(2.0f * (static_cast<float>(viewport.X) - bounds.left)
					+ static_cast<float>(viewport.Width)) / width - 1.0f;
			const float scaleY = static_cast<float>(viewport.Height) / height;
			const float offsetY = 1.0f -
				(2.0f * (static_cast<float>(viewport.Y) - bounds.top)
					+ static_cast<float>(viewport.Height)) / height;
			std::array<float, 16> result = source;
			for (size_t component = 0; component < 4; ++component)
			{
				result[component] = source[component] * scaleX
					+ source[12 + component] * offsetX;
				result[4 + component] = source[4 + component] * scaleY
					+ source[12 + component] * offsetY;
			}
			return result;
		}

		bool RenderCompositeTexture(IDirect3DDevice9* device,
			UInt32 width, UInt32 height,
			const std::array<float, 16>& bakeWvp,
			NativeA8CompositeBakeDrawFn draw, void* context, bool fallback,
			IDirect3DTexture9*& texture)
		{
			texture = nullptr;
			if (!device || !width || !height || !draw)
				return false;
			IDirect3DSurface9* surface = nullptr;
			bool rendered = SUCCEEDED(device->CreateTexture(
				width, height, 1, D3DUSAGE_RENDERTARGET,
				D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr))
				&& texture
				&& SUCCEEDED(texture->GetSurfaceLevel(0, &surface))
				&& surface;
			if (rendered)
			{
				D3DVIEWPORT9 bakeViewport = {};
				bakeViewport.Width = width;
				bakeViewport.Height = height;
				bakeViewport.MinZ = 0.0f;
				bakeViewport.MaxZ = 1.0f;
				InvalidateNativeA8SortedShaderState();
				rendered = SUCCEEDED(device->SetRenderTarget(0, surface))
					&& SUCCEEDED(device->SetDepthStencilSurface(nullptr))
					&& SUCCEEDED(device->SetViewport(&bakeViewport))
					&& SUCCEEDED(device->SetRenderState(
						D3DRS_SCISSORTESTENABLE, FALSE))
					&& SUCCEEDED(device->SetRenderState(
						D3DRS_SRGBWRITEENABLE, FALSE))
					&& SUCCEEDED(device->Clear(
						0, nullptr, D3DCLEAR_TARGET, 0x00000000u, 1.0f, 0));
			}
			if (rendered)
			{
				BeginBakeWvp(bakeWvp);
				rendered = draw(context, fallback);
				EndBakeWvp();
			}
			if (surface)
				surface->Release();
			return rendered;
		}

		bool IssueCompositeTextureValidation(
			IDirect3DDevice9* device, UInt32 generation,
			IDirect3DSurface9* comparisonTarget, UInt32 width, UInt32 height,
			IDirect3DTexture9* optimized, IDirect3DTexture9* reference,
			IDirect3DQuery9*& pendingQuery)
		{
			pendingQuery = nullptr;
			IDirect3DVertexDeclaration9* declaration =
				GetNativeA8D3DDeclaration(generation);
			IDirect3DVertexShader9* vertexShader =
				GetNativeA8CacheD3DVertexShader(generation);
			IDirect3DPixelShader9* pixelShader =
				GetNativeA8CompositeValidationD3DPixelShader(generation);
			if (!device || !comparisonTarget || !width || !height
				|| !optimized || !reference || !declaration
				|| !vertexShader || !pixelShader)
			{
				return false;
			}

			IDirect3DQuery9* query = nullptr;
			if (FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query))
				|| !query)
			{
				return false;
			}

			const float inverseWidth = 1.0f / static_cast<float>(width);
			const float inverseHeight = 1.0f / static_cast<float>(height);
			const float left = -1.0f - inverseWidth;
			const float right = 1.0f - inverseWidth;
			const float top = 1.0f + inverseHeight;
			const float bottom = -1.0f + inverseHeight;
			const std::array<NativeA8GpuVertex, 4> vertices = {{
				{ left, 0.0f, top, 0.0f, 0.0f, 0xFFFFFFFFu,
					1.0f, 1.0f, 8.0f },
				{ right, 0.0f, top, 1.0f, 0.0f, 0xFFFFFFFFu,
					1.0f, 1.0f, 8.0f },
				{ left, 0.0f, bottom, 0.0f, 1.0f, 0xFFFFFFFFu,
					1.0f, 1.0f, 8.0f },
				{ right, 0.0f, bottom, 1.0f, 1.0f, 0xFFFFFFFFu,
					1.0f, 1.0f, 8.0f }
			}};
			D3DVIEWPORT9 viewport = {};
			viewport.Width = width;
			viewport.Height = height;
			viewport.MinZ = 0.0f;
			viewport.MaxZ = 1.0f;
			const RECT scissor = {
				0, 0, static_cast<LONG>(width), static_cast<LONG>(height)
			};

			bool configured =
				SUCCEEDED(device->SetRenderTarget(0, comparisonTarget))
				&& SUCCEEDED(device->SetDepthStencilSurface(nullptr))
				&& SUCCEEDED(device->SetViewport(&viewport))
				&& SUCCEEDED(device->SetScissorRect(&scissor))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_SCISSORTESTENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_ZWRITEENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_ALPHATESTENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_ALPHABLENDENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_SEPARATEALPHABLENDENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_STENCILENABLE, FALSE))
				&& SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,
					D3DCULL_NONE))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_COLORWRITEENABLE, 0))
				&& SUCCEEDED(device->SetRenderState(
					D3DRS_SRGBWRITEENABLE, FALSE))
				&& SUCCEEDED(device->SetVertexDeclaration(declaration))
				&& SUCCEEDED(device->SetVertexShader(vertexShader))
				&& SUCCEEDED(device->SetPixelShader(pixelShader))
				&& SUCCEEDED(device->SetTexture(0, optimized))
				&& SUCCEEDED(device->SetTexture(1, reference));
			for (DWORD sampler = 0; configured && sampler < 2; ++sampler)
			{
				configured =
					SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP))
					&& SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP))
					&& SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_MINFILTER, D3DTEXF_POINT))
					&& SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_MAGFILTER, D3DTEXF_POINT))
					&& SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_MIPFILTER, D3DTEXF_NONE))
					&& SUCCEEDED(device->SetSamplerState(sampler,
						D3DSAMP_SRGBTEXTURE, FALSE));
			}

			const bool issued = configured
				&& SUCCEEDED(query->Issue(D3DISSUE_BEGIN))
				&& SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,
					2, vertices.data(), sizeof(NativeA8GpuVertex)))
				&& SUCCEEDED(query->Issue(D3DISSUE_END));
			device->SetTexture(0, nullptr);
			device->SetTexture(1, nullptr);
			if (!issued)
			{
				query->Release();
				return false;
			}
			pendingQuery = query;
			return true;
		}

		NativeA8PayloadTemplatePtr BuildCachedArtifact(
			IDirect3DTexture9*& texture, const RECT& bounds,
			const CompositeObservation& observation,
			const NativeA8PayloadTemplatePtr& sourceOwner)
		{
			if (!texture || !sourceOwner || sourceOwner->packets.empty())
				return {};
			NiTexturingProperty* property =
				CreateDefaultTextureProperty(texture, AtlasPixelMode::Argb32);
			if (!property || !property->m_kMaps.GetSize()
				|| !property->m_kMaps[0]
				|| !property->m_kMaps[0]->m_spTexture)
			{
				return {};
			}
			NiTexture* niTexture = property->m_kMaps[0]->m_spTexture;
			auto artifact = std::make_shared<NativeA8PayloadTemplate>();
			artifact->pageCount = 1;
			artifact->quadCount = 1;
			artifact->sourceRangeCount = 1;
			artifact->atlasProperties.push_back(property);
			artifact->atlasTextures.push_back(niTexture);
			artifact->gpuVertices.resize(4);

			const D3DVIEWPORT9& viewport = observation.viewport;
			// D3D9 pixel centers are integer-valued.  Put the quad edges half a
			// pixel outside the cached texel centers so the one-sample hit path
			// reproduces the exact RTT footprint without a half-pixel shift.
			const float left = (static_cast<float>(bounds.left) - 0.5f
				- viewport.X) * 2.0f / viewport.Width - 1.0f;
			const float right = (static_cast<float>(bounds.right) - 0.5f
				- viewport.X) * 2.0f / viewport.Width - 1.0f;
			const float top = 1.0f - (static_cast<float>(bounds.top) - 0.5f
				- viewport.Y) * 2.0f / viewport.Height;
			const float bottom = 1.0f - (static_cast<float>(bounds.bottom) - 0.5f
				- viewport.Y) * 2.0f / viewport.Height;
			const std::array<NiPoint3, 4> positions = {{
				NiPoint3(left, 0.0f, top), NiPoint3(right, 0.0f, top),
				NiPoint3(right, 0.0f, bottom), NiPoint3(left, 0.0f, bottom)
			}};
			const std::array<NiPoint2, 4> uvs = {{
				NiPoint2(0.0f, 0.0f), NiPoint2(1.0f, 0.0f),
				NiPoint2(1.0f, 1.0f), NiPoint2(0.0f, 1.0f)
			}};
			for (size_t index = 0; index < artifact->gpuVertices.size(); ++index)
			{
				artifact->gpuVertices[index] = {
					positions[index].x, positions[index].y, positions[index].z,
					uvs[index].x, uvs[index].y, 0xFFFFFFFFu,
					1.0f, 1.0f, 8.0f
				};
			}
			artifact->bound.m_kCenter = NiPoint3(
				(left + right) * 0.5f, 0.0f, (top + bottom) * 0.5f);
			const float halfWidth = (right - left) * 0.5f;
			const float halfHeight = (top - bottom) * 0.5f;
			artifact->bound.m_fRadius = std::sqrt(
				halfWidth * halfWidth + halfHeight * halfHeight);

			NativeA8PacketTemplate packet;
			packet.firstVertex = 0;
			packet.vertexCount = 4;
			packet.bound = artifact->bound;
			packet.shaderClass = NativeA8ShaderClass::CachedImage;
			packet.sampling = NativeA8Sampling::Point;
			packet.quality = EffectQuality::Fast;
			packet.distanceFieldMethod =
				sourceOwner->packets.front().distanceFieldMethod;
			packet.layer = 3;
			packet.atlasPage = 0;
			packet.staticSmoothSampling = false;
			packet.usesLiveTileRgb = false;
			artifact->packets.push_back(packet);
			artifact->cpuMemory.Reset(CpuMemoryCategory::TextArtifact,
				GetNativeA8PayloadTemplateBytes(*artifact));
			return artifact;
		}
	}

	struct NativeA8CompositeCacheTracker
	{
		CompositeCacheKey key;
		std::shared_ptr<NativeA8CompositeCacheEntry> entry;
		UInt64 lastObservedFrame = 0;
		UInt64 cooldownUntilFrame = 0;
		UInt32 stableFrames = 0;
		bool hasKey = false;
	};

	namespace
	{
		bool SameObservation(const CompositeObservation& observation,
			const NativeA8CompositeCacheTracker& tracker)
		{
			return tracker.hasKey && tracker.key == observation.key;
		}
	}

	bool GetNativeA8BakeWvp(std::array<float, 16>& wvp)
	{
		if (!s_bakeWvp.depth)
			return false;
		wvp = s_bakeWvp.wvp;
		return true;
	}

	void BeginNativeA8CompositeCacheFrame()
	{
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		++state.frameSerial;
		if (!state.frameSerial)
			++state.frameSerial;
		state.generatedItemsThisFrame = 0;
		state.generatedTexelsThisFrame = 0;
		state.frameActive = true;
		if (!g_bEnableFreeTypeFontCompositePass)
			ClearPendingValidationsLocked(state);
		else
			PollPendingValidationsLocked(state);
		if (!g_bEnableFreeTypeFontCompositePass
			|| !g_uiFreeTypeFontCompositeCacheMB)
		{
			for (auto& item : state.entries)
				InvalidateEntryLocked(item.second, state);
			state.entries.clear();
			state.bytes = 0;
		}
		else
		{
			TrimCacheLocked(state, 0);
		}
	}

	void EndNativeA8CompositeCacheFrame()
	{
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		state.frameActive = false;
	}

	NativeA8ShapePayload* ProbeNativeA8CompositeCache(
		NiTriShape* facade, const A8ShapeMetadata&,
		NativeA8ShapePayload& sourcePayload)
	{
		if (!sourcePayload.payloadTemplate)
			return nullptr;
		const UInt32 currentGeneration = GetNativeA8ShaderGeneration();
		const NativeA8PayloadTemplate& sourceArtifact =
			*sourcePayload.payloadTemplate;
		const bool needsVisualValidation =
			sourcePayload.useCompositePackets && currentGeneration
			&& sourceArtifact.compositeValidatedGeneration.load(
				std::memory_order_acquire) != currentGeneration
			&& sourceArtifact.compositeRejectedGeneration.load(
				std::memory_order_acquire) != currentGeneration;
		if (!CacheBudgetBytes() && !needsVisualValidation)
		{
			sourcePayload.compositeCacheTracker.reset();
			return nullptr;
		}
		CompositeObservation observation;
		if (!BuildObservation(facade, sourcePayload, observation))
		{
			sourcePayload.compositeCacheTracker.reset();
			return nullptr;
		}
		if (!sourcePayload.compositeCacheTracker)
		{
			sourcePayload.compositeCacheTracker =
				std::make_shared<NativeA8CompositeCacheTracker>();
		}
		NativeA8CompositeCacheTracker& tracker =
			*sourcePayload.compositeCacheTracker;
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.frameActive)
			return nullptr;

		if (!SameObservation(observation, tracker))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheStateChange);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheMiss);
			tracker.key = observation.key;
			tracker.hasKey = true;
			tracker.stableFrames = 1;
			tracker.lastObservedFrame = state.frameSerial;
			tracker.entry.reset();
			return nullptr;
		}
		if (tracker.lastObservedFrame != state.frameSerial)
		{
			tracker.lastObservedFrame = state.frameSerial;
			if (tracker.stableFrames < std::numeric_limits<UInt32>::max())
				++tracker.stableFrames;
		}
		if (tracker.entry && tracker.entry->valid
			&& tracker.entry->key == observation.key)
		{
			tracker.entry->lastUse = ++state.useSerial;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheHit);
			return &tracker.entry->cachedPayload;
		}
		tracker.entry.reset();
		if (tracker.stableFrames < kStableObservationCount
			|| state.frameSerial < tracker.cooldownUntilFrame)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheMiss);
			return nullptr;
		}
		auto found = state.entries.find(observation.key);
		if (found == state.entries.end() || !found->second
			|| !found->second->valid)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheMiss);
			return nullptr;
		}
		tracker.entry = found->second;
		tracker.entry->lastUse = ++state.useSerial;
		RecordFreeTypePerf(
			FreeTypePerfCounter::CompositeCacheHit);
		return &tracker.entry->cachedPayload;
	}

	bool TryGenerateNativeA8CompositeCache(
		NiTriShape* facade, const A8ShapeMetadata&,
		NativeA8ShapePayload& sourcePayload,
		NativeA8CompositeBakeDrawFn draw, void* context)
	{
		if (!draw || !sourcePayload.compositeCacheTracker)
			return false;
		CompositeObservation observation;
		if (!BuildObservation(facade, sourcePayload, observation))
			return false;
		const NativeA8PayloadTemplate& artifact =
			*sourcePayload.payloadTemplate;
		const UInt32 rejectedGeneration =
			artifact.compositeRejectedGeneration.load(
				std::memory_order_acquire);
		if (rejectedGeneration == observation.generation)
			return false;
		const bool needsVisualValidation =
			sourcePayload.useCompositePackets
			&& artifact.compositeValidatedGeneration.load(
				std::memory_order_acquire) != observation.generation;
		if (needsVisualValidation
			&& artifact.compositeValidationPendingGeneration.load(
				std::memory_order_acquire) == observation.generation)
		{
			return false;
		}
		const UInt64 cacheBudget = CacheBudgetBytes();
		const bool cacheEnabled = cacheBudget != 0;
		if (!cacheEnabled && !needsVisualValidation)
			return false;

		NativeA8CompositeCacheTracker& tracker =
			*sourcePayload.compositeCacheTracker;
		if (!SameObservation(observation, tracker)
			|| tracker.stableFrames < kStableObservationCount)
		{
			return false;
		}

		CompositeCacheState& state = CacheState();
		UInt64 generationFrame = 0;
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			if (!state.frameActive
				|| state.frameSerial < tracker.cooldownUntilFrame
				|| state.generatedItemsThisFrame
					>= kMaximumGenerationsPerFrame
				|| (needsVisualValidation
					&& state.pendingValidations.size()
						>= kMaximumPendingValidations))
			{
				return false;
			}
			generationFrame = state.frameSerial;
			auto existing = state.entries.find(observation.key);
			if (existing != state.entries.end() && existing->second
				&& existing->second->valid)
			{
				tracker.entry = existing->second;
				return true;
			}
		}

		auto rejectComposite = [&](bool inconclusive)
		{
			artifact.compositeRejectedGeneration.store(
				observation.generation, std::memory_order_release);
			sourcePayload.compositeAttemptGeneration =
				observation.generation;
			sourcePayload.compositeUnavailable = true;
			sourcePayload.preparedGeneration = 0;
			sourcePayload.preflightAtlasTextureEpoch = 0;
			RecordFreeTypePerf(inconclusive
				? FreeTypePerfCounter::CompositeVisualInconclusive
				: FreeTypePerfCounter::CompositeVisualRejected);
		};

		CompositeBakeStateGuard guard(observation.device);
		if (!guard.Captured()
			|| guard.RenderTarget() != observation.renderTargetIdentity
			|| guard.Viewport().X != observation.viewport.X
			|| guard.Viewport().Y != observation.viewport.Y
			|| guard.Viewport().Width != observation.viewport.Width
			|| guard.Viewport().Height != observation.viewport.Height)
		{
			if (needsVisualValidation)
				rejectComposite(true);
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		RECT bounds = {};
		if (!ProjectArtifactBounds(*sourcePayload.payloadTemplate,
			guard.VertexConstants(), observation.viewport,
			observation.targetDescription, bounds))
		{
			if (needsVisualValidation)
				rejectComposite(true);
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		const UInt32 width = static_cast<UInt32>(bounds.right - bounds.left);
		const UInt32 height = static_cast<UInt32>(bounds.bottom - bounds.top);
		const UInt64 texels = static_cast<UInt64>(width) * height;
		const UInt64 bytes = texels * 4ull;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!width || !height || texels > kMaximumEntryTexels
			|| !renderer
			|| width > renderer->m_kD3DCaps9.MaxTextureWidth
			|| height > renderer->m_kD3DCaps9.MaxTextureHeight)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheBudgetReject);
			if (needsVisualValidation)
				rejectComposite(true);
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		const bool cacheEligible = cacheEnabled && bytes <= cacheBudget;
		if (cacheEnabled && !cacheEligible)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheBudgetReject);
		}
		if (!cacheEligible && !needsVisualValidation)
		{
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		const UInt64 requiredTexels = texels
			* (needsVisualValidation ? 2ull : 1ull);
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			if (state.generatedTexelsThisFrame + requiredTexels
				> kMaximumGeneratedTexelsPerFrame)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeCacheBudgetReject);
				return false;
			}
			++state.generatedItemsThisFrame;
			state.generatedTexelsThisFrame += requiredTexels;
		}

		const std::array<float, 16> bakeWvp = BuildBakeWvp(
			guard.VertexConstants(), observation.viewport, bounds);
		IDirect3DTexture9* referenceTexture = nullptr;
		const bool referenceRendered = !needsVisualValidation
			|| RenderCompositeTexture(observation.device, width, height,
				bakeWvp, draw, context, true, referenceTexture);
		IDirect3DTexture9* texture = nullptr;
		const bool generated = RenderCompositeTexture(observation.device,
			width, height, bakeWvp, draw, context, false, texture);
		IDirect3DQuery9* validationQuery = nullptr;
		const bool validationIssued = !needsVisualValidation
			|| (referenceRendered && generated
				&& IssueCompositeTextureValidation(observation.device,
					observation.generation, guard.RenderTarget(), width, height,
					texture, referenceTexture, validationQuery));
		const bool restored = guard.Restore();
		InvalidateNativeA8SortedShaderState();
		if (referenceTexture)
			referenceTexture->Release();
		if (!generated || !referenceRendered || !validationIssued || !restored)
		{
			if (!generated || !referenceRendered || !validationIssued)
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeCacheRttFailure);
			if (!restored)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeCacheRestoreFailure);
				MarkNativeA8GenerationFault(observation.generation,
					"composite-cache-state-restore", E_FAIL);
			}
			if (texture)
				texture->Release();
			if (validationQuery)
				validationQuery->Release();
			if (needsVisualValidation)
				rejectComposite(true);
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		if (needsVisualValidation)
		{
			bool queued = false;
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				if (state.frameActive
					&& state.frameSerial == generationFrame
					&& state.pendingValidations.size()
						< kMaximumPendingValidations
					&& IsNativeA8ShaderGenerationCurrent(
						observation.generation)
					&& artifact.compositeValidationPendingGeneration.load(
						std::memory_order_acquire)
						!= observation.generation)
				{
					artifact.compositeValidationPendingGeneration.store(
						observation.generation, std::memory_order_release);
					PendingCompositeValidation pending;
					pending.sourceOwner = sourcePayload.payloadTemplate;
					pending.query = validationQuery;
					pending.generation = observation.generation;
					pending.submittedFrame = generationFrame;
					state.pendingValidations.push_back(std::move(pending));
					validationQuery = nullptr;
					queued = true;
				}
			}
			if (validationQuery)
				validationQuery->Release();
			if (texture)
				texture->Release();
			if (!queued)
			{
				rejectComposite(true);
				tracker.cooldownUntilFrame =
					generationFrame + kFailedGenerationCooldownFrames;
				return false;
			}
			// Validation completion is polled at future frame boundaries without
			// D3DGETDATA_FLUSH.  A validated artifact can build its cache entry on
			// the next eligible frame; this frame never waits for the driver.
			return true;
		}
		if (!cacheEligible)
		{
			if (texture)
				texture->Release();
			return false;
		}

		NativeA8PayloadTemplatePtr cachedArtifact = BuildCachedArtifact(
			texture, bounds, observation, sourcePayload.payloadTemplate);
		if (texture)
			texture->Release();
		if (!cachedArtifact)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CompositeCacheRttFailure);
			tracker.cooldownUntilFrame =
				generationFrame + kFailedGenerationCooldownFrames;
			return false;
		}
		auto entry = std::make_shared<NativeA8CompositeCacheEntry>();
		entry->key = observation.key;
		entry->sourceOwner = sourcePayload.payloadTemplate;
		entry->cachedArtifact = cachedArtifact;
		entry->cachedPayload.payloadTemplate = cachedArtifact;
		entry->cachedPayload.geometryOrigin = NiPoint3();
		entry->cachedPayload.packetShaders.assign(1, nullptr);
		entry->cachedPayload.preflightAtlasTextures.assign(1, nullptr);
		entry->cachedPayload.buildComplete = true;
		entry->bytes = bytes;
		entry->valid = true;

		{
			std::lock_guard<std::mutex> lock(state.mutex);
			if (!state.frameActive || state.frameSerial != generationFrame
				|| !IsNativeA8ShaderGenerationCurrent(observation.generation)
				|| !SameObservation(observation, tracker))
			{
				InvalidateEntryLocked(entry, state);
				tracker.cooldownUntilFrame =
					state.frameSerial + kFailedGenerationCooldownFrames;
				return false;
			}
			TrimCacheLocked(state, bytes);
			const UInt64 currentBudget = CacheBudgetBytes();
			if (bytes > currentBudget
				|| state.bytes > currentBudget - bytes)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeCacheBudgetReject);
				InvalidateEntryLocked(entry, state);
				tracker.cooldownUntilFrame =
					state.frameSerial + kFailedGenerationCooldownFrames;
				return false;
			}
			entry->lastUse = ++state.useSerial;
			state.bytes += bytes;
			state.entries[entry->key] = entry;
			tracker.entry = entry;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CompositeCacheGenerated);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CompositeCacheBytes, bytes);
		return true;
	}

	void InvalidateNativeA8CompositeCacheHit(
		NativeA8ShapePayload& sourcePayload)
	{
		if (!sourcePayload.compositeCacheTracker)
			return;
		NativeA8CompositeCacheTracker& tracker =
			*sourcePayload.compositeCacheTracker;
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (tracker.entry)
		{
			auto found = state.entries.find(tracker.entry->key);
			if (found != state.entries.end()
				&& found->second == tracker.entry)
			{
				InvalidateEntryLocked(found->second, state);
				state.entries.erase(found);
			}
		}
		tracker.entry.reset();
		tracker.cooldownUntilFrame =
			state.frameSerial + kFailedGenerationCooldownFrames;
	}

	NativeA8PayloadTemplatePtr GetNativeA8CompositeCacheArtifact(
		const NativeA8ShapePayload& sourcePayload)
	{
		if (!sourcePayload.compositeCacheTracker)
			return {};
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const std::shared_ptr<NativeA8CompositeCacheEntry>& entry =
			sourcePayload.compositeCacheTracker->entry;
		return entry && entry->valid ? entry->cachedArtifact
			: NativeA8PayloadTemplatePtr{};
	}

	void ClearNativeA8CompositeCache()
	{
		CompositeCacheState& state = CacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		for (auto& item : state.entries)
			InvalidateEntryLocked(item.second, state);
		state.entries.clear();
		ClearPendingValidationsLocked(state);
		state.bytes = 0;
		state.generatedItemsThisFrame = 0;
		state.generatedTexelsThisFrame = 0;
	}
}
