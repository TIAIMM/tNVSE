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

		// A menu commonly submits far more than 16 text facades. A larger weak
		// table avoids direct-map thrashing without retaining deleted menu payloads.
		thread_local std::array<A8MetadataHotEntry, 256> s_metadataHotEntries;

		size_t GetMetadataHotSlot(const NiTriShape* shape)
		{
			return (reinterpret_cast<uintptr_t>(shape) >> 4)
				% s_metadataHotEntries.size();
		}

		class NativePixelConstantScope
		{
		public:
			// The native profile mirrors the stock Tile value into c0 for its final
			// packet and deliberately leaves it there, matching an ordinary Tile
			// draw. Only tNVSE-owned c1-c4 need isolation from the next shader.
			static constexpr UINT kFirstRegister = 1;
			static constexpr UINT kRegisterCount = 4;
			static constexpr size_t kFloatCount = kRegisterCount * 4;

			explicit NativePixelConstantScope(IDirect3DDevice9* device)
				: m_device(device)
			{
				m_result = m_device
					? m_device->GetPixelShaderConstantF(kFirstRegister,
						m_original.data(), kRegisterCount)
					: D3DERR_INVALIDCALL;
				m_captured = SUCCEEDED(m_result);
				if (!m_captured)
					m_operation = "capture-pixel-constants";
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
				// SetPixelShaderConstantF already reports a failed restore. The readback
				// is useful for diagnostics but forces another driver round trip for
				// every text facade, so retain it only in the detailed logging mode.
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
				m_operation = "none";
				m_result = D3D_OK;
				return true;
			}

		private:
			IDirect3DDevice9* m_device = nullptr;
			std::array<float, kFloatCount> m_original = {};
			std::array<float, kFloatCount> m_verify = {};
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
				m_captured = SUCCEEDED(m_result);
				if (!m_captured)
					return SetFailure("capture-pixel-constants", m_result);
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
				"tnvse_freetype_native: batched pixel-constant isolation fault phase=%s operation=%s hr=0x%08X register=%d generation=%u",
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
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		A8MetadataHotEntry& hot = s_metadataHotEntries[GetMetadataHotSlot(shape)];
		if (hot.shape == shape && hot.generation == generation)
		{
			A8ShapeMetadataPtr metadata = hot.metadata.lock();
			if (metadata)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::MetadataHotHit);
				return metadata;
			}
			hot = {};
		}

		RecordFreeTypePerf(FreeTypePerfCounter::MetadataLockedLookup);
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		const auto found = state.shapeMetadata.find(shape);
		if (found == state.shapeMetadata.end())
		{
			hot = {};
			return {};
		}
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
		const NiAlphaProperty* liveAlpha = shape->GetAlphaProperty();
		const bool liveAlphaBlending =
			liveAlpha && liveAlpha->GetAlphaBlending();
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
			&& frameEntry.generation == payload->preparedGeneration
			&& frameEntry.generation == GetNativeA8ShaderGeneration()
			&& payload->preflightAtlasTextureEpoch
				== GetNativeA8AtlasTextureEpoch()
			&& payload->preflightScaledFillSampling
				== NeedsScaledFillSampling(shape)
			&& payload->preflightAlphaBlending
				== liveAlphaBlending
			&& IsNativeA8AccumulatorHookCurrent()
			&& IsA8TileRenderPassHookCurrent())
		{
			// NativeA8RenderSorted retained the metadata owner and validated this
			// exact payload immediately before the stock sorted Tile traversal.
			failure = NativeA8FallbackReason::None;
		}
		else
			failure = PrepareNativeA8Group(shape, *metadata, *payload);

		if (failure == NativeA8FallbackReason::None)
		{
			bool runtimeFault = false;
			bool drewPacket = false;
			bool constantStateFault = false;
			NativeA8FallbackReason runtimeFailure =
				NativeA8FallbackReason::RuntimeFault;
			const char* faultOperation = "generation-changed-after-packet";
			HRESULT faultResult = D3DERR_DEVICELOST;
			SInt32 faultRegister = -1;
			{
				NativeRingSubmissionScope ringScope;
				failure = BeginNativeA8RingSubmission(shape, *payload,
					ringScope.submission);
				if (failure != NativeA8FallbackReason::None)
				{
					runtimeFault = true;
					runtimeFailure = failure;
					faultOperation = "ring-submission";
				}
				NativeTilePacketScope packetScope(pass);
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				IDirect3DDevice9* device = renderer
					? renderer->GetD3DDevice() : nullptr;
				if (!device)
				{
					runtimeFault = true;
					constantStateFault = true;
					faultOperation = "capture-pixel-constants";
					faultResult = D3DERR_DEVICELOST;
				}
				const bool batchedConstants =
					s_pixelConstantBatch.FrameActive();
				std::optional<NativePixelConstantScope> localConstants;
				if (!runtimeFault)
				{
					// Retail 0xB64F90 calls 0xB994F0 once per sorted item with no
					// intervening draw; this hook is that call boundary. Consecutive
					// native facades therefore share one c1-c4 capture, while every
					// stock/non-native item and the frame end flush first.
					if (batchedConstants)
					{
						if (!s_pixelConstantBatch.EnsureCaptured(device))
						{
							runtimeFault = true;
							constantStateFault = true;
							faultOperation =
								s_pixelConstantBatch.Operation();
							faultResult = s_pixelConstantBatch.Result();
							faultRegister =
								s_pixelConstantBatch.MismatchRegister();
						}
					}
					else
					{
						localConstants.emplace(device);
						if (!localConstants->Captured())
						{
							runtimeFault = true;
							constantStateFault = true;
							faultOperation = localConstants->Operation();
							faultResult = localConstants->Result();
						}
					}
					for (UInt32 packetIndex = 0; !runtimeFault
						&& packetIndex < payload->packetShaders.size(); ++packetIndex)
					{
						NiTriShape* proxyShape = nullptr;
						const NativeA8FallbackReason packetFailure =
							PrepareNativeA8RingPacket(shape, *payload,
								ringScope.submission, packetIndex, proxyShape);
						if (packetFailure != NativeA8FallbackReason::None || !proxyShape)
						{
							runtimeFault = true;
							runtimeFailure = packetFailure != NativeA8FallbackReason::None
								? packetFailure : NativeA8FallbackReason::RuntimeFault;
							faultOperation = "ring-packet";
							break;
						}
						packetScope.Select(proxyShape);
						// The distance-field shader already produces continuous coverage.
						// The stock Tile call enables alpha testing for UI items,
						// which would threshold and discard those edge samples.
						state.originalTileRenderPass(pass, currentPass, false,
							true, setupDrawmode);
						drewPacket = true;
						if (!IsNativeA8ShaderGenerationCurrent(
							payload->preparedGeneration))
						{
							runtimeFault = true;
							break;
						}
					}
					if (!batchedConstants && localConstants
						&& !localConstants->RestoreAndVerify())
					{
						runtimeFault = true;
						constantStateFault = true;
						faultOperation = localConstants->Operation();
						faultResult = localConstants->Result();
						faultRegister =
							localConstants->MismatchRegister();
					}
				}
				if (batchedConstants && runtimeFault
					&& !FlushNativePixelConstantBatch(
						"native-runtime-fault"))
				{
					constantStateFault = true;
					faultOperation = s_pixelConstantBatch.Operation();
					faultResult = s_pixelConstantBatch.Result();
					faultRegister =
						s_pixelConstantBatch.MismatchRegister();
				}
			}
			if (!runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedTileRenderPassHit)
				{
					state.loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile group route hit shape=%p font=%u pass=%u packets=%u ranges=%u",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(payload->packetShaders.size()),
						payload->payloadTemplate
							? payload->payloadTemplate->sourceRangeCount : 0u);
				}
				return;
			}
			if (constantStateFault)
			{
				MarkNativeA8GenerationFault(payload->preparedGeneration,
					faultOperation, faultResult);
				gLog.FormattedMessage(
					"tnvse_freetype_native: pixel-constant isolation fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-group",
					faultOperation, static_cast<UInt32>(faultResult), faultRegister,
					shape, metadata->fontId, payload->preparedGeneration,
					drewPacket ? 1 : 0);
			}
			if (drewPacket)
			{
				MarkNativeA8RuntimeFault(*metadata, *payload, runtimeFailure);
				return;
			}
			failure = runtimeFailure;
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
