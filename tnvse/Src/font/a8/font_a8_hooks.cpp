#include "font_a8_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiRenderer.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace fonthook::vectorfont
{
	namespace
	{
		struct A8MetadataHotEntry
		{
			const NiTriShape* shape = nullptr;
			UInt64 generation = 0;
			std::weak_ptr<const A8ShapeMetadata> metadata;
		};

		inline constexpr size_t kMetadataHotWayCount = 8;
		inline constexpr size_t kMetadataHotSetCount = 2048;
		static_assert((kMetadataHotSetCount & (kMetadataHotSetCount - 1)) == 0,
			"metadata hot-cache set count must remain a power of two");

		struct A8MetadataHotSet
		{
			std::array<A8MetadataHotEntry, kMetadataHotWayCount> ways;
			UInt8 nextVictim = 0;
		};

		// Eight ways keep allocator-neighbouring facades from evicting one another,
		// while weak ownership preserves menu/shape destruction semantics. 16384
		// entries cover the observed multi-page Pip-Boy working set without adding
		// any process-wide locks to the render path.
		thread_local std::unique_ptr<
			std::array<A8MetadataHotSet, kMetadataHotSetCount>>
			s_metadataHotSets;

		A8MetadataHotSet& GetMetadataHotSet(const NiTriShape* shape)
		{
			if (!s_metadataHotSets)
			{
				s_metadataHotSets = std::make_unique<
					std::array<A8MetadataHotSet, kMetadataHotSetCount>>();
			}
			const size_t index = HashMetadataShapeAddress(shape)
				& (kMetadataHotSetCount - 1);
			return (*s_metadataHotSets)[index];
		}

		A8MetadataHotEntry& SelectMetadataHotVictim(A8MetadataHotSet& set)
		{
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (!entry.shape)
					return entry;
			}
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (entry.metadata.expired())
				{
					entry = {};
					return entry;
				}
			}
			A8MetadataHotEntry& victim =
				set.ways[set.nextVictim % kMetadataHotWayCount];
			set.nextVictim = static_cast<UInt8>(
				(set.nextVictim + 1) % kMetadataHotWayCount);
			victim = {};
			return victim;
		}

		class NativePixelConstantScope
		{
		public:
			// The native profile mirrors the stock Tile value into c0 for its final
			// packet and deliberately leaves it there, matching an ordinary Tile
			// draw. tNVSE-owned pixel c1-c8 and vertex c4 need isolation from the
			// next shader; VS c4 carries viewport/raster data for analytic AA.
			static constexpr UINT kFirstRegister = 1;
			static constexpr UINT kRegisterCount =
				static_cast<UINT>(kNativeA8PacketConstantRegisterCount);
			static constexpr size_t kFloatCount = kRegisterCount * 4;
			static constexpr UINT kVertexRegister =
				static_cast<UINT>(kNativeA8VertexAaConstantRegister);
			static constexpr UINT kVertexRegisterCount = 1;
			static constexpr size_t kVertexFloatCount = 4;

			explicit NativePixelConstantScope(IDirect3DDevice9* device)
				: m_device(device)
			{
				if (!m_device)
				{
					m_result = D3DERR_INVALIDCALL;
					m_operation = "capture-pixel-constants";
					return;
				}
				m_result = m_device->GetPixelShaderConstantF(kFirstRegister,
					m_original.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "capture-pixel-constants";
					return;
				}
				m_result = m_device->GetVertexShaderConstantF(kVertexRegister,
					m_originalVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "capture-vertex-aa-constant";
					return;
				}
				m_captured = true;
				m_operation = "none";
			}

			~NativePixelConstantScope()
			{
				if (m_captured && !m_finished)
					RestoreAndVerify();
			}

			bool Captured() const { return m_captured; }
			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }

			bool RestoreAndVerify()
			{
				if (!m_captured)
					return false;
				if (m_finished)
					return SUCCEEDED(m_result);
				m_finished = true;

				m_result = m_device->SetPixelShaderConstantF(kFirstRegister,
					m_original.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "restore-pixel-constants";
					return false;
				}
				m_result = m_device->SetVertexShaderConstantF(kVertexRegister,
					m_originalVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "restore-vertex-aa-constant";
					return false;
				}
				// The Set calls already report a failed restore. Readback is useful
				// for diagnostics but forces driver round trips, so retain it only
				// in detailed logging mode.
				if (!g_bEnableFreeTypeFontRenderingLog)
				{
					m_operation = "none";
					m_result = D3D_OK;
					return true;
				}
				m_result = m_device->GetPixelShaderConstantF(kFirstRegister,
					m_verify.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "verify-pixel-constants";
					return false;
				}
				for (size_t index = 0; index < kFloatCount; ++index)
				{
					if (std::memcmp(&m_original[index], &m_verify[index],
						sizeof(float)) != 0)
					{
						m_operation = "pixel-constant-mismatch";
						m_mismatchRegister = static_cast<SInt32>(
							kFirstRegister + index / 4);
						m_result = E_FAIL;
						return false;
					}
				}
				m_result = m_device->GetVertexShaderConstantF(kVertexRegister,
					m_verifyVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "verify-vertex-aa-constant";
					return false;
				}
				for (size_t index = 0; index < kVertexFloatCount; ++index)
				{
					if (std::memcmp(&m_originalVertex[index],
						&m_verifyVertex[index], sizeof(float)) != 0)
					{
						m_operation = "vertex-aa-constant-mismatch";
						m_mismatchRegister =
							static_cast<SInt32>(kVertexRegister);
						m_result = E_FAIL;
						return false;
					}
				}
				m_operation = "none";
				m_result = D3D_OK;
				return true;
			}

		private:
			IDirect3DDevice9* m_device = nullptr;
			std::array<float, kFloatCount> m_original = {};
			std::array<float, kFloatCount> m_verify = {};
			std::array<float, kVertexFloatCount> m_originalVertex = {};
			std::array<float, kVertexFloatCount> m_verifyVertex = {};
			HRESULT m_result = D3DERR_INVALIDCALL;
			const char* m_operation = "capture-pixel-constants";
			SInt32 m_mismatchRegister = -1;
			bool m_captured = false;
			bool m_finished = false;
		};

		class NativePixelConstantBatch
		{
		public:
			void BeginFrame()
			{
				m_frameActive = true;
			}

			bool FrameActive() const
			{
				return m_frameActive;
			}

			bool EnsureCaptured(IDirect3DDevice9* device)
			{
				if (!m_frameActive || !device)
					return SetFailure("capture-pixel-constants",
						D3DERR_INVALIDCALL);
				if (m_captured)
				{
					if (m_device == device)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::ConstantBatchReuse);
						return true;
					}
					if (!Flush())
						return false;
				}
				return Capture(device);
			}

			bool Flush()
			{
				if (!m_captured)
					return true;
				m_captured = false;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantBatchFlush);
				m_result = m_device->SetPixelShaderConstantF(
					NativePixelConstantScope::kFirstRegister,
					m_original.data(),
					NativePixelConstantScope::kRegisterCount);
				if (FAILED(m_result))
					return SetFailure("restore-pixel-constants", m_result);
				m_result = m_device->SetVertexShaderConstantF(
					NativePixelConstantScope::kVertexRegister,
					m_originalVertex.data(),
					NativePixelConstantScope::kVertexRegisterCount);
				if (FAILED(m_result))
					return SetFailure(
						"restore-vertex-aa-constant", m_result);
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					m_result = m_device->GetPixelShaderConstantF(
						NativePixelConstantScope::kFirstRegister,
						m_verify.data(),
						NativePixelConstantScope::kRegisterCount);
					if (FAILED(m_result))
						return SetFailure("verify-pixel-constants", m_result);
					for (size_t index = 0;
						index < NativePixelConstantScope::kFloatCount; ++index)
					{
						if (std::memcmp(&m_original[index], &m_verify[index],
							sizeof(float)) != 0)
						{
							m_mismatchRegister = static_cast<SInt32>(
								NativePixelConstantScope::kFirstRegister
									+ index / 4);
							return SetFailure(
								"pixel-constant-mismatch", E_FAIL);
						}
					}
					m_result = m_device->GetVertexShaderConstantF(
						NativePixelConstantScope::kVertexRegister,
						m_verifyVertex.data(),
						NativePixelConstantScope::kVertexRegisterCount);
					if (FAILED(m_result))
						return SetFailure(
							"verify-vertex-aa-constant", m_result);
					for (size_t index = 0;
						index < NativePixelConstantScope::kVertexFloatCount;
						++index)
					{
						if (std::memcmp(&m_originalVertex[index],
							&m_verifyVertex[index], sizeof(float)) != 0)
						{
							m_mismatchRegister = static_cast<SInt32>(
								NativePixelConstantScope::kVertexRegister);
							return SetFailure(
								"vertex-aa-constant-mismatch", E_FAIL);
						}
					}
				}
				m_device = nullptr;
				m_operation = "none";
				m_result = D3D_OK;
				m_mismatchRegister = -1;
				return true;
			}

			void EndFrame()
			{
				m_frameActive = false;
			}

			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }
			UInt32 Generation() const { return m_generation; }

		private:
			bool Capture(IDirect3DDevice9* device)
			{
				m_device = device;
				m_generation = GetNativeA8ShaderGeneration();
				m_mismatchRegister = -1;
				m_result = device->GetPixelShaderConstantF(
					NativePixelConstantScope::kFirstRegister,
					m_original.data(),
					NativePixelConstantScope::kRegisterCount);
				if (FAILED(m_result))
					return SetFailure("capture-pixel-constants", m_result);
				m_result = device->GetVertexShaderConstantF(
					NativePixelConstantScope::kVertexRegister,
					m_originalVertex.data(),
					NativePixelConstantScope::kVertexRegisterCount);
				if (FAILED(m_result))
					return SetFailure(
						"capture-vertex-aa-constant", m_result);
				m_captured = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantBatchCapture);
				m_operation = "none";
				return true;
			}

			bool SetFailure(const char* operation, HRESULT result)
			{
				m_operation = operation;
				m_result = result;
				m_device = nullptr;
				m_captured = false;
				return false;
			}

			IDirect3DDevice9* m_device = nullptr;
			std::array<float, NativePixelConstantScope::kFloatCount> m_original = {};
			std::array<float, NativePixelConstantScope::kFloatCount> m_verify = {};
			std::array<float,
				NativePixelConstantScope::kVertexFloatCount>
				m_originalVertex = {};
			std::array<float,
				NativePixelConstantScope::kVertexFloatCount>
				m_verifyVertex = {};
			HRESULT m_result = D3D_OK;
			const char* m_operation = "none";
			SInt32 m_mismatchRegister = -1;
			UInt32 m_generation = 0;
			bool m_frameActive = false;
			bool m_captured = false;
		};

		thread_local NativePixelConstantBatch s_pixelConstantBatch;

		bool FlushNativePixelConstantBatch(const char* phase)
		{
			if (s_pixelConstantBatch.Flush())
				return true;
			MarkNativeA8GenerationFault(s_pixelConstantBatch.Generation(),
				s_pixelConstantBatch.Operation(),
				s_pixelConstantBatch.Result());
			gLog.FormattedMessage(
				"tnvse_freetype_native: batched shader-constant isolation fault phase=%s operation=%s hr=0x%08X register=%d generation=%u",
				phase ? phase : "unknown",
				s_pixelConstantBatch.Operation(),
				static_cast<UInt32>(s_pixelConstantBatch.Result()),
				s_pixelConstantBatch.MismatchRegister(),
				s_pixelConstantBatch.Generation());
			return false;
		}

		class NativeTilePacketScope
		{
		public:
			explicit NativeTilePacketScope(BSShaderProperty::RenderPass* pass)
				: m_pass(pass), m_facade(pass ? pass->pGeometry : nullptr)
			{
			}

			~NativeTilePacketScope()
			{
				if (m_pass)
					m_pass->pGeometry = m_facade;
			}

			void Select(NiGeometry* geometry)
			{
				m_pass->pGeometry = geometry;
			}

		private:
			BSShaderProperty::RenderPass* m_pass = nullptr;
			NiGeometry* m_facade = nullptr;
		};

		class NativeRingSubmissionScope
		{
		public:
			~NativeRingSubmissionScope()
			{
				EndNativeA8RingSubmission(submission);
			}

			NativeA8RingSubmission submission;
		};

		class NativeFacadeShaderBatchScope
		{
		public:
			NativeFacadeShaderBatchScope()
			{
				BeginNativeA8FacadeShaderBatch();
			}

			~NativeFacadeShaderBatchScope()
			{
				EndNativeA8FacadeShaderBatch();
			}
		};

		struct NativePacketDrawResult
		{
			bool runtimeFault = false;
			bool drewPacket = false;
			bool stockLikeBitmapRoute = false;
			bool constantStateFault = false;
			NativeA8FallbackReason failure =
				NativeA8FallbackReason::RuntimeFault;
			const char* operation = "generation-changed-after-packet";
			HRESULT result = D3DERR_DEVICELOST;
			SInt32 mismatchRegister = -1;
		};

		NativePacketDrawResult DrawNativePacketSet(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Submit);
			NativePacketDrawResult draw;
			NativeRingSubmissionScope ringScope;
			NativeA8FallbackReason failure = BeginNativeA8RingSubmission(
				facade, payload, ringScope.submission);
			if (failure != NativeA8FallbackReason::None)
			{
				draw.runtimeFault = true;
				draw.failure = failure;
				draw.operation = "ring-submission";
			}
			NativeTilePacketScope packetScope(pass);
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "capture-pixel-constants";
				draw.result = D3DERR_DEVICELOST;
			}
			// Retail NiDX9Renderer's indexed-array loop cannot express
			// triangle-list page boundaries: m_pusArrayLengths is interpreted as
			// strip lengths (primitiveCount = length - 2). Keep the page split at
			// the one stock sorted Tile callsite instead. Each packet selects a
			// Font::MakeTriShape proxy and then executes the untouched
			// TileRenderPass -> NiTriShape::RenderImmediate renderer path.
			//
			// Final ARGB and baked-coverage bitmaps use only c0. Skipping the
			// distance-field c1-c8/VS-c4 isolation and facade bookkeeping removes
			// the only per-facade driver readback/writeback from this stock-like
			// multipage route.
			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& !g_bDisableFreeTypeExtendedCaches
				&& s_pixelConstantBatch.FrameActive();
			std::optional<NativePixelConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				// Retail 0xB64F90 calls 0xB994F0 once per sorted item with no
				// intervening draw. Capture PS c1-c8 and VS c4 once for that batch and
				// preserve the local scope for direct/non-sorted submissions.
				if (batchedConstants)
				{
					if (!s_pixelConstantBatch.EnsureCaptured(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_pixelConstantBatch.Operation();
						draw.result = s_pixelConstantBatch.Result();
						draw.mismatchRegister =
							s_pixelConstantBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Captured())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
				for (UInt32 packetIndex = 0; !draw.runtimeFault
					&& packetIndex < payload.packetShaders.size(); ++packetIndex)
				{
					NiTriShape* proxyShape = nullptr;
					const NativeA8FallbackReason packetFailure =
						PrepareNativeA8RingPacket(facade, payload,
							ringScope.submission, packetIndex, proxyShape);
					if (packetFailure != NativeA8FallbackReason::None
						|| !proxyShape)
					{
						draw.runtimeFault = true;
						draw.failure =
							packetFailure != NativeA8FallbackReason::None
								? packetFailure
								: NativeA8FallbackReason::RuntimeFault;
						draw.operation = "ring-packet";
						break;
					}
					packetScope.Select(proxyShape);
					State().originalTileRenderPass(pass, currentPass, false,
						true, setupDrawmode);
					draw.drewPacket = true;
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					const std::vector<NativeA8PacketTemplate>& activePackets =
						GetNativeA8Packets(*payload.payloadTemplate,
							payload.useCompositePackets);
					if (packetIndex < activePackets.size()
						&& activePackets[packetIndex].shaderClass
							== NativeA8ShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
					if (!IsNativeA8ShaderGenerationCurrent(
						payload.preparedGeneration))
					{
						draw.runtimeFault = true;
						break;
					}
				}
				if (isolatePacketConstants && !batchedConstants
					&& localConstants
					&& !localConstants->RestoreAndVerify())
				{
					draw.runtimeFault = true;
					draw.constantStateFault = true;
					draw.operation = localConstants->Operation();
					draw.result = localConstants->Result();
					draw.mismatchRegister =
						localConstants->MismatchRegister();
				}
			}
			if (isolatePacketConstants && batchedConstants && draw.runtimeFault
				&& !FlushNativePixelConstantBatch("native-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_pixelConstantBatch.Operation();
				draw.result = s_pixelConstantBatch.Result();
				draw.mismatchRegister =
					s_pixelConstantBatch.MismatchRegister();
			}
			return draw;
		}

		void LogMissingMetadata(NiTriShape* shape, const char* phase)
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return;
			static std::atomic<UInt32> logCount = 0;
			constexpr UInt32 kMaximumLogs = 8;
			const UInt32 ordinal = logCount.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=%s shape=%p thread=%u",
					phase ? phase : "unknown", shape, GetCurrentThreadId());
			}
			else if (ordinal == kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: metadata-missing render logs capped at %u entries",
					kMaximumLogs);
			}
		}

		void SuppressImmediateRoute(NiTriShape* shape, const char* phase)
		{
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(shape);
			if (!metadata)
			{
				LogMissingMetadata(shape, phase);
				return;
			}
			RecordNativeA8Suppression(shape, *metadata,
				metadata->nativePayload.buildComplete
					? NativeA8FallbackReason::DirectImmediate
					: NativeA8FallbackReason::PacketBuild,
				phase);
		}

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			A8State& state = State();
			A8ShapeMetadataPtr retiredMetadata;
			{
				std::lock_guard<std::mutex> lock(state.metadataMutex);
				const auto found = state.shapeMetadata.find(shape);
				if (found != state.shapeMetadata.end())
				{
					state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(1,
						std::memory_order_release);
					retiredMetadata = std::move(found->second);
					state.shapeMetadata.erase(found);
				}
			}
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
		}
	}

	void BeginA8SortedTileConstantBatch()
	{
		s_pixelConstantBatch.BeginFrame();
	}

	void EndA8SortedTileConstantBatch()
	{
		FlushNativePixelConstantBatch("sorted-frame-end");
		s_pixelConstantBatch.EndFrame();
	}

	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape)
	{
		if (!shape)
			return {};
		A8State& state = State();
		if (g_bDisableFreeTypeExtendedCaches)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::MetadataLockedLookup);
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			const auto found = state.shapeMetadata.find(shape);
			return found == state.shapeMetadata.end()
				? A8ShapeMetadataPtr{} : found->second;
		}
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		A8MetadataHotSet& hotSet = GetMetadataHotSet(shape);
		A8MetadataHotEntry* replacement = nullptr;
		for (A8MetadataHotEntry& hot : hotSet.ways)
		{
			if (hot.shape != shape)
				continue;
			if (hot.generation == generation)
			{
				A8ShapeMetadataPtr metadata = hot.metadata.lock();
				if (metadata)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::MetadataHotHit);
					return metadata;
				}
			}
			hot = {};
			replacement = &hot;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::MetadataLockedLookup);
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		const auto found = state.shapeMetadata.find(shape);
		if (found == state.shapeMetadata.end())
			return {};
		A8MetadataHotEntry& hot = replacement
			? *replacement : SelectMetadataHotVictim(hotSet);
		hot.shape = shape;
		hot.generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_relaxed);
		hot.metadata = found->second;
		return found->second;
	}

	TileRenderPassFn ReadTileRenderPassCallTarget()
	{
		const UInt8* call = reinterpret_cast<const UInt8*>(kTileRenderPassCallSite);
		if (!call || call[0] != 0xE8)
			return nullptr;
		SInt32 displacement = 0;
		std::memcpy(&displacement, call + 1, sizeof(displacement));
		return reinterpret_cast<TileRenderPassFn>(
			kTileRenderPassCallSite + 5 + displacement);
	}

	bool IsA8TileRenderPassHookCurrent()
	{
		return State().originalTileRenderPass
			&& ReadTileRenderPassCallTarget() == &A8TileRenderPass;
	}

	void __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode)
	{
		A8State& state = State();
		if (!state.originalTileRenderPass)
			return;
		NiTriShape* shape = pass
			? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
		if (!IsA8AtlasShape(shape))
		{
			if (s_pixelConstantBatch.FrameActive())
				FlushNativePixelConstantBatch("before-stock-tile");
			InvalidateNativeA8SortedShaderState();
			state.originalTileRenderPass(pass, currentPass, testAlpha,
				blendAlpha, setupDrawmode);
			return;
		}

		NativeA8SortedFrameEntryView frameEntry;
		const bool sortedFrameHit =
			FindNativeA8SortedFrameEntry(shape, frameEntry);
		A8ShapeMetadataPtr metadataOwner;
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		if (sortedFrameHit)
		{
			metadata = frameEntry.metadata;
			payload = frameEntry.payload;
		}
		else
		{
			metadataOwner = FindA8ShapeMetadata(shape);
			metadata = metadataOwner.get();
			if (metadata && metadata->nativePayload.buildComplete)
				payload = &metadata->nativePayload;
		}
		if (!metadata)
		{
			LogMissingMetadata(shape, "tile-render-pass");
			return;
		}
		NativeA8FallbackReason failure = NativeA8FallbackReason::None;
		if (!payload)
			failure = NativeA8FallbackReason::PacketBuild;
		else if (payload->suppressNextSubmit.exchange(false,
			std::memory_order_acq_rel))
		{
			failure = payload->stickyReason.exchange(
				NativeA8FallbackReason::None, std::memory_order_acq_rel);
			if (failure == NativeA8FallbackReason::None)
				failure = NativeA8FallbackReason::RuntimeFault;
		}
		else if (sortedFrameHit
			&& frameEntry.preflightResult == NativeA8FallbackReason::None
			&& frameEntry.validationToken
			&& frameEntry.generation == payload->preparedGeneration
			)
		{
			// NativeA8RenderSorted retained the metadata owner and validated this
			// exact payload immediately before the stock sorted Tile traversal.
			failure = NativeA8FallbackReason::None;
		}
		else
			failure = PrepareNativeA8Group(shape, *metadata, *payload);

		if (failure == NativeA8FallbackReason::None)
		{
			NativeA8ShapePayload* const sourcePayload = payload;
			NativePacketDrawResult draw = DrawNativePacketSet(pass,
				currentPass, setupDrawmode, shape, *sourcePayload);
			if (!draw.runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedTileRenderPassHit)
				{
					state.loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile group route hit shape=%p font=%u pass=%u packets=%u ranges=%u route=%s",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(
							sourcePayload->packetShaders.size()),
						sourcePayload->payloadTemplate
							? sourcePayload->payloadTemplate->sourceRangeCount
							: 0u,
						draw.stockLikeBitmapRoute
							? "stock-like-bitmap-pages"
							: "effect-packets");
				}
				return;
			}
			InvalidateNativeA8SortedShaderState();
			if (draw.constantStateFault)
			{
				MarkNativeA8GenerationFault(
					sourcePayload->preparedGeneration,
					draw.operation, draw.result);
				gLog.FormattedMessage(
					"tnvse_freetype_native: shader-constant isolation fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-group",
					draw.operation, static_cast<UInt32>(draw.result),
					draw.mismatchRegister, shape, metadata->fontId,
					sourcePayload->preparedGeneration,
					draw.drewPacket ? 1 : 0);
			}
			if (draw.drewPacket)
			{
				MarkNativeA8RuntimeFault(*metadata, *sourcePayload,
					draw.failure);
				return;
			}
			failure = draw.failure;
		}

		RecordNativeA8Suppression(shape, *metadata, failure, "tile-render-pass");
	}

	bool HookTileRenderPass()
	{
		TileRenderPassFn current = ReadTileRenderPassCallTarget();
		const TileRenderPassFn hook = &A8TileRenderPass;
		if (current == hook)
		{
			State().tileRenderPassHookInstalled = State().originalTileRenderPass != nullptr;
			return State().tileRenderPassHookInstalled;
		}
		if (!current)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site is not CALL rel32; native route unavailable");
			}
			return false;
		}
		if (State().tileRenderPassHookInstalled)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator native route was replaced; marked groups will be suppressed");
			}
			return false;
		}
		if (reinterpret_cast<UInt32>(current) != kStockTileRenderPassImmediately)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site already has a non-stock target=%p; leaving it untouched",
					current);
			}
			return false;
		}

		State().originalTileRenderPass = current;
		WriteRelCall(kTileRenderPassCallSite, hook);
		State().tileRenderPassHookInstalled = ReadTileRenderPassCallTarget() == hook;
		if (!State().tileRenderPassHookInstalled)
		{
			State().originalTileRenderPass = nullptr;
			return false;
		}
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: installed Tile accumulator native route original=%p stock=1",
				current);
		}
		return true;
	}

	bool IsA8AtlasShape(const NiTriShape* shape)
	{
		return shape && *reinterpret_cast<void* const* const*>(shape)
			== &State().triShapeVtable[1];
	}

	void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer*)
	{
		SuppressImmediateRoute(shape, "shape-immediate");
	}

	void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer*)
	{
		SuppressImmediateRoute(shape, "shape-immediate-alt");
	}

	bool InitializeA8TriShapeVtable(NiTriShape* shape)
	{
		void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
		if (!source)
			return false;
		if (source == &State().triShapeVtable[1])
			return true;
		if (State().originalTriShapeVtable)
			return source == State().originalTriShapeVtable;

		State().originalTriShapeVtable = source;
		State().triShapeVtable[0] = source[-1];
		std::copy(source, source + kCopiedTriShapeVtableEntries,
			State().triShapeVtable.begin() + 1);
		State().originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
			State().triShapeVtable[kRenderImmediateSlot + 1]);
		State().originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
			State().triShapeVtable[kRenderImmediateAltSlot + 1]);
		State().originalDeleteThis = reinterpret_cast<DeleteThisFn>(
			State().triShapeVtable[kDeleteThisSlot + 1]);
		State().triShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&A8DeleteThis);
		State().triShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediate);
		State().triShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediateAlt);
		return State().originalRenderImmediate && State().originalRenderImmediateAlt
			&& State().originalDeleteThis;
	}
}
