#include "font_a8_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"

#include "NiDX9RenderState.hpp"
#include "NiDX9TextureData.hpp"
#include "NiAlphaProperty.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace fonthook::vectorfont
{
		bool HookD3DDevice();
		namespace
		{
			class NativePixelConstantScope
			{
			public:
				static constexpr UINT kFirstRegister = 0;
				static constexpr UINT kRegisterCount = 5;
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
							m_mismatchRegister = static_cast<SInt32>(index / 4);
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

			class NativeTilePacketScope
			{
			public:
				explicit NativeTilePacketScope(BSShaderProperty::RenderPass* pass)
					: m_pass(pass), m_facade(pass ? pass->pGeometry : nullptr),
					m_thread(ThreadState())
				{
					++m_thread.nativePacketDepth;
				}

				~NativeTilePacketScope()
				{
					if (m_pass)
						m_pass->pGeometry = m_facade;
					--m_thread.nativePacketDepth;
				}

				void Select(NiGeometry* geometry)
				{
					m_pass->pGeometry = geometry;
				}

			private:
				BSShaderProperty::RenderPass* m_pass = nullptr;
				NiGeometry* m_facade = nullptr;
				A8ThreadState& m_thread;
			};

			void ActivateDirectBridgeFallback(NiTriShape* shape,
				const A8ShapeMetadataPtr& metadata)
			{
				A8ThreadState& thread = ThreadState();
				if (thread.nativePacketDepth || thread.renderDepth || !metadata)
					return;
				const bool bridgeReady = EnsureA8BridgeFallbackReady();
				const NativeA8FallbackReason reason = metadata->nativePayload
					? NativeA8FallbackReason::DirectImmediate
					: NativeA8FallbackReason::PacketBuild;
				RecordNativeA8Fallback(shape, *metadata,
					bridgeReady ? reason
						: NativeA8FallbackReason::BridgeUnavailable,
					bridgeReady);
			}
		}

		bool HasA8ShapeMetadata(const NiTriShape* shape, bool* hasShadow = nullptr,
			UInt32* fontId = nullptr)
		{
			if (hasShadow)
				*hasShadow = false;
			if (fontId)
				*fontId = 0;
			if (!shape)
				return false;
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			if (!metadata)
				return false;
			if (hasShadow)
				*hasShadow = HasShadowRange(*metadata);
			if (fontId)
				*fontId = metadata->fontId;
			return true;
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
				state.originalTileRenderPass(pass, currentPass, testAlpha,
					blendAlpha, setupDrawmode);
				return;
			}
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			const bool tracked = static_cast<bool>(metadata);
			if (tracked)
			{
				NativeA8FallbackReason failure = NativeA8FallbackReason::None;
				NativeA8ShapePayload* payload = metadata->nativePayload.get();
				if (!payload)
				{
					failure = NativeA8FallbackReason::PacketBuild;
				}
				else if (payload->bridgeNextSubmit.exchange(false,
					std::memory_order_acq_rel))
				{
					failure = payload->stickyReason.exchange(
						NativeA8FallbackReason::None, std::memory_order_acq_rel);
					if (failure == NativeA8FallbackReason::None)
						failure = NativeA8FallbackReason::RuntimeFault;
				}
				else
				{
					failure = PrepareNativeA8Group(shape, *metadata, *payload);
				}

				if (failure == NativeA8FallbackReason::None)
				{
					bool runtimeFault = false;
					bool drewPacket = false;
					bool constantStateFault = false;
					const char* faultOperation = "generation-changed-after-packet";
					HRESULT faultResult = D3DERR_DEVICELOST;
					SInt32 faultRegister = -1;
					{
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
						for (NativeA8Packet& packet : payload->packets)
						{
							if (runtimeFault)
								break;
							packetScope.Select(packet.shape.m_pObject);
							NativePixelConstantScope constants(device);
							if (!constants.Captured())
							{
								runtimeFault = true;
								constantStateFault = true;
								faultOperation = constants.Operation();
								faultResult = constants.Result();
								break;
							}

							state.originalTileRenderPass(pass, currentPass, testAlpha,
								blendAlpha, setupDrawmode);
							drewPacket = true;
							if (!constants.RestoreAndVerify())
							{
								runtimeFault = true;
								constantStateFault = true;
								faultOperation = constants.Operation();
								faultResult = constants.Result();
								faultRegister = constants.MismatchRegister();
								break;
							}
							if (!IsNativeA8ShaderGenerationCurrent(
								payload->preparedGeneration))
							{
								runtimeFault = true;
								break;
							}
						}
					}
					if (runtimeFault)
					{
						if (constantStateFault)
						{
							MarkNativeA8GenerationFault(payload->preparedGeneration,
								faultOperation, faultResult);
							gLog.FormattedMessage(
								"tnvse_freetype_native: pixel-constant isolation fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u; aborting native group and explicitly routing bridge fallback",
								faultOperation, static_cast<UInt32>(faultResult),
								faultRegister, shape, metadata->fontId,
								payload->preparedGeneration, drewPacket ? 1 : 0);
						}
						if (drewPacket)
						{
							MarkNativeA8RuntimeFault(*payload,
								NativeA8FallbackReason::RuntimeFault);
							return;
						}
						failure = NativeA8FallbackReason::RuntimeFault;
					}
					else if (g_bEnableFreeTypeFontRenderingLog
						&& !state.loggedTileRenderPassHit)
					{
						state.loggedTileRenderPassHit = true;
						gLog.FormattedMessage(
							"tnvse_freetype_native: native Tile group route hit shape=%p font=%u pass=%u packets=%u ranges=%u",
							shape, metadata->fontId, currentPass,
							static_cast<UInt32>(payload->packets.size()),
							static_cast<UInt32>(metadata->compiledRanges.size()));
					}
					if (!runtimeFault)
					{
						RecordNativeA8Recovery(shape, *metadata);
						return;
					}
				}

				const bool bridgeReady = EnsureA8BridgeFallbackReady();
				RecordNativeA8Fallback(shape, *metadata,
					bridgeReady ? failure
						: NativeA8FallbackReason::BridgeUnavailable,
					bridgeReady);
			}

			const bool hasShadow = metadata && HasShadowRange(*metadata);
			const UInt32 fontId = metadata ? metadata->fontId : 0;
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			if (tracked)
			{
				// Startup menus can reach the Tile accumulator before NVSE's
				// DeferredInit message. At this call site the renderer/device are live,
				// so make one final synchronous attempt before this shape's first DIP.
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				IDirect3DDevice9* device = renderer
					? renderer->GetD3DDevice() : nullptr;
				if (!state.rangeBridgeAvailable || device != state.hookedDevice)
					state.rangeBridgeAvailable = HookD3DDevice();
				if (g_bEnableFreeTypeFontRenderingLog && !state.loggedTileRenderPassHit)
				{
					state.loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator bridge route hit shape=%p font=%u pass=%u depth=%u",
						shape, fontId, currentPass, ThreadState().renderDepth);
				}
				BeginA8RenderTrace(shape, "tile-render-pass", metadata);
				++ThreadState().renderDepth;
				ThreadState().currentShape = shape;
				ThreadState().currentMetadata = metadata;
			}
			state.originalTileRenderPass(pass, currentPass, testAlpha, blendAlpha,
				setupDrawmode);
			if (tracked)
			{
				A8RenderTraceContext* trace = CurrentRenderTrace();
				if (hasShadow && trace && trace->shape == shape
					&& state.loggedTileShadowResultFonts.insert(fontId).second)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator shadow result shape=%p font=%u draws=%u forwarded=%u ranges=%u effectOk=%u effectFail=%u fillOk=%u fillFail=%u bridge=%u",
						shape, fontId, trace->drawCalls, trace->forwardedCalls,
						trace->rangeAttempts,
						trace->effectSuccesses, trace->effectFailures,
						trace->fillSuccesses, trace->fillFailures,
						state.rangeBridgeAvailable ? 1 : 0);
				}
				ThreadState().currentShape = previousShape;
				ThreadState().currentMetadata = std::move(previousMetadata);
				--ThreadState().renderDepth;
				EndA8RenderTrace(shape, "tile-render-pass");
			}
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
						"tnvse_freetype_font: Tile accumulator call site is not CALL rel32; startup range route unavailable");
				}
				return false;
			}
			if (State().tileRenderPassHookInstalled)
			{
				if (!State().loggedTileRenderPassHookConflict)
				{
					State().loggedTileRenderPassHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator range route was replaced; retaining safe fallback routes");
				}
				return false;
			}
			if (reinterpret_cast<UInt32>(current)
				!= kStockTileRenderPassImmediately)
			{
				if (!State().loggedTileRenderPassHookConflict)
				{
					State().loggedTileRenderPassHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator call site already has a non-stock target=%p; leaving it untouched",
						current);
				}
				return false;
			}

			// Only wrap the verified stock cdecl target. An arbitrary pre-existing
			// naked hook may depend on volatile registers from the original call site;
			// invoking it through a C++ wrapper would not be a safe compatibility chain.
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
					"tnvse_freetype_font: installed Tile accumulator range route original=%p stock=%u",
					current, reinterpret_cast<UInt32>(current)
						== kStockTileRenderPassImmediately ? 1 : 0);
			}
			return true;
		}


		bool IsA8AtlasShape(const NiTriShape* shape)
		{
			return shape && *reinterpret_cast<void* const* const*>(shape)
				== &State().triShapeVtable[1];
		}

		void __fastcall A8RenderShape(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			const A8ShapeMetadataPtr metadata = a8 ? ResolveRenderMetadata(shape) : nullptr;
			ActivateDirectBridgeFallback(shape, metadata);
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			if (a8)
			{
				BeginA8RenderTrace(shape, "renderer-shape", metadata);
				++ThreadState().renderDepth;
				ThreadState().currentShape = shape;
				ThreadState().currentMetadata = metadata;
			}
			State().originalRenderShape(renderer, shape);
			if (a8)
			{
				ThreadState().currentShape = previousShape;
				ThreadState().currentMetadata = std::move(previousMetadata);
				--ThreadState().renderDepth;
				EndA8RenderTrace(shape, "renderer-shape");
			}
		}

		void __fastcall A8RenderShapeAlt(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			const A8ShapeMetadataPtr metadata = a8 ? ResolveRenderMetadata(shape) : nullptr;
			ActivateDirectBridgeFallback(shape, metadata);
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			if (a8)
			{
				BeginA8RenderTrace(shape, "renderer-shape-alt", metadata);
				++ThreadState().renderDepth;
				ThreadState().currentShape = shape;
				ThreadState().currentMetadata = metadata;
			}
			State().originalRenderShapeAlt(renderer, shape);
			if (a8)
			{
				ThreadState().currentShape = previousShape;
				ThreadState().currentMetadata = std::move(previousMetadata);
				--ThreadState().renderDepth;
				EndA8RenderTrace(shape, "renderer-shape-alt");
			}
		}

		void __fastcall A8BatchRenderShape(NiDX9Renderer* renderer, void*,
			NiTriShape* shape)
		{
			if (!State().originalBatchRenderShape)
				return;
			if (!renderer)
				return;
			if (!IsA8AtlasShape(shape)
				|| !State().originalBeginBatch || !State().originalEndBatch)
			{
				State().originalBatchRenderShape(renderer, shape);
				return;
			}
			if (g_bEnableFreeTypeFontRenderingLog && !State().loggedBatchRouteHit)
			{
				State().loggedBatchRouteHit = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: renderer batch route hit shape=%p depth=%u",
					shape, ThreadState().renderDepth);
			}

			// NiDX9Renderer's batch path queues geometry and emits its D3D draw calls
			// later from Do_EndBatch. At that point there is no per-shape virtual call
			// left from which the range bridge can recover this shape's metadata. Isolate
			// a marked atlas shape in its own batch so the original renderer keeps the
			// caller's Tile properties and shader selection, while the D3D bridge retains
			// an unambiguous shape context for every draw in that batch.
			NiPropertyState* properties = renderer->m_pkBatchedPropertyState;
			NiDynamicEffectState* effects = renderer->m_pkBatchedEffectState;
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			ActivateDirectBridgeFallback(shape, metadata);
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			const UInt32 previousDepth = ThreadState().renderDepth;
			// End even an apparently empty batch: AddToBatch may already have selected
			// m_spBatchedShader before rejecting all geometry. Reusing that cached shader
			// for this shape would recreate the same missing/incorrect-pass failure. Any
			// older queued geometry is not part of this shape, so do not let an inherited
			// immediate-render scope misclassify its draw calls as FreeType ranges.
			ThreadState().renderDepth = 0;
			ThreadState().currentShape = nullptr;
			ThreadState().currentMetadata.reset();
			State().originalEndBatch(renderer);
			ThreadState().renderDepth = previousDepth;
			ThreadState().currentShape = previousShape;
			ThreadState().currentMetadata = previousMetadata;
			State().originalBeginBatch(renderer, properties, effects);

			BeginA8RenderTrace(shape, "renderer-batch-shape", metadata);
			++ThreadState().renderDepth;
			ThreadState().currentShape = shape;
			ThreadState().currentMetadata = metadata;
			// Keep the scope active while submitting as well as flushing. The stock
			// function only queues, but this also remains correct if another renderer
			// extension has wrapped the slot and chooses to issue an immediate draw.
			State().originalBatchRenderShape(renderer, shape);
			State().originalEndBatch(renderer);
			ThreadState().currentShape = previousShape;
			ThreadState().currentMetadata = std::move(previousMetadata);
			--ThreadState().renderDepth;
			EndA8RenderTrace(shape, "renderer-batch-shape");

			// The caller still owns the surrounding batch and will submit more objects.
			State().originalBeginBatch(renderer, properties, effects);
		}

		bool WriteVtableEntry(void** vtable, UInt32 slot, void* replacement)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(&vtable[slot], sizeof(void*),
				PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				return false;
			}
			vtable[slot] = replacement;
			DWORD ignored = 0;
			VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &ignored);
			FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
			return true;
		}

		bool HookRendererShapeEntries(NiDX9Renderer* renderer)
		{
			void** vtable = renderer ? *reinterpret_cast<void***>(renderer) : nullptr;
			if (!vtable)
				return false;

			// Begin/End are not replaced, but always follow the current vtable targets so
			// a renderer extension installed before or after tNVSE remains in the chain.
			State().originalBeginBatch = reinterpret_cast<BeginBatchFn>(
				vtable[kRendererBeginBatchSlot]);
			State().originalEndBatch = reinterpret_cast<EndBatchFn>(
				vtable[kRendererEndBatchSlot]);

			void* batchEntry = vtable[kRendererBatchRenderShapeSlot];
			void* renderEntry = vtable[kRendererRenderShapeSlot];
			void* renderAltEntry = vtable[kRendererRenderShapeAltSlot];
			void* batchHook = reinterpret_cast<void*>(&A8BatchRenderShape);
			void* renderHook = reinterpret_cast<void*>(&A8RenderShape);
			void* renderAltHook = reinterpret_cast<void*>(&A8RenderShapeAlt);

			if (!State().originalBatchRenderShape && batchEntry != batchHook)
				State().originalBatchRenderShape = reinterpret_cast<BatchRenderShapeFn>(batchEntry);
			if (!State().originalRenderShape && renderEntry != renderHook)
				State().originalRenderShape = reinterpret_cast<RenderShapeFn>(renderEntry);
			if (!State().originalRenderShapeAlt && renderAltEntry != renderAltHook)
				State().originalRenderShapeAlt = reinterpret_cast<RenderShapeFn>(renderAltEntry);
			const bool entriesValid = State().originalBatchRenderShape
				&& State().originalRenderShape && State().originalRenderShapeAlt
				&& (batchEntry == batchHook || batchEntry
					== reinterpret_cast<void*>(State().originalBatchRenderShape))
				&& (renderEntry == renderHook || renderEntry
					== reinterpret_cast<void*>(State().originalRenderShape))
				&& (renderAltEntry == renderAltHook || renderAltEntry
					== reinterpret_cast<void*>(State().originalRenderShapeAlt));
			if (!entriesValid)
			{
				if (!State().loggedRendererHookConflict)
				{
					State().loggedRendererHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: renderer shape bridge was replaced; new atlases use 32-bit fallback");
				}
				return false;
			}

			bool wroteBatch = false;
			bool wroteRender = false;
			if (batchEntry != batchHook)
			{
				if (!WriteVtableEntry(vtable, kRendererBatchRenderShapeSlot, batchHook))
					return false;
				wroteBatch = true;
			}
			if (renderEntry != renderHook)
			{
				if (!WriteVtableEntry(vtable, kRendererRenderShapeSlot, renderHook))
				{
					if (wroteBatch)
						WriteVtableEntry(vtable, kRendererBatchRenderShapeSlot,
							reinterpret_cast<void*>(State().originalBatchRenderShape));
					return false;
				}
				wroteRender = true;
			}
			if (renderAltEntry != renderAltHook
				&& !WriteVtableEntry(vtable, kRendererRenderShapeAltSlot, renderAltHook))
			{
				if (wroteRender)
					WriteVtableEntry(vtable, kRendererRenderShapeSlot,
						reinterpret_cast<void*>(State().originalRenderShape));
				if (wroteBatch)
					WriteVtableEntry(vtable, kRendererBatchRenderShapeSlot,
						reinterpret_cast<void*>(State().originalBatchRenderShape));
				return false;
			}
			return State().originalBeginBatch && State().originalEndBatch
				&& State().originalBatchRenderShape && State().originalRenderShape
				&& State().originalRenderShapeAlt;
		}

		bool HookD3DDevice()
		{
			const bool tileRouteAvailable = HookTileRenderPass();
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return false;
			void** vtable = *reinterpret_cast<void***>(device);
			if (!vtable)
				return false;
			if (vtable[kDrawIndexedPrimitiveSlot]
				== reinterpret_cast<void*>(&A8DrawIndexedPrimitive))
			{
				State().hookedDevice = device;
				const bool rendererRoutesAvailable = HookRendererShapeEntries(renderer);
				return State().originalDrawIndexedPrimitive
					&& (rendererRoutesAvailable || tileRouteAvailable);
			}
			if (State().hookedDevice && State().hookedDevice == device)
			{
				if (!State().loggedHookConflict)
				{
					State().loggedHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: D3D9 draw bridge was replaced; new atlases use 32-bit fallback");
				}
				return false;
			}

			State().originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(
				vtable[kDrawIndexedPrimitiveSlot]);
			if (!WriteVtableEntry(vtable, kDrawIndexedPrimitiveSlot,
				reinterpret_cast<void*>(&A8DrawIndexedPrimitive)))
			{
				return false;
			}
			State().hookedDevice = device;
			const bool rendererBridgeAvailable = HookRendererShapeEntries(renderer);
			if (g_bEnableFreeTypeFontRenderingLog)
				gLog.FormattedMessage(
					"tnvse_freetype_font: installed FreeType atlas D3D9 range draw bridge rendererRoutes=%u batchRoute=%u",
					rendererBridgeAvailable ? 1 : 0,
					vtable && renderer
						&& (*reinterpret_cast<void***>(renderer))[kRendererBatchRenderShapeSlot]
							== reinterpret_cast<void*>(&A8BatchRenderShape) ? 1 : 0);
			return rendererBridgeAvailable || tileRouteAvailable;
		}

		bool IsPublishedRangeBridgeReady()
		{
			if (!State().rangeBridgeAvailable || !State().originalDrawIndexedPrimitive)
				return false;
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			void** deviceVtable = device ? *reinterpret_cast<void***>(device) : nullptr;
			if (!device || device != State().hookedDevice || !deviceVtable
				|| deviceVtable[kDrawIndexedPrimitiveSlot]
					!= reinterpret_cast<void*>(&A8DrawIndexedPrimitive))
			{
				return false;
			}

			void** rendererVtable = renderer
				? *reinterpret_cast<void***>(renderer) : nullptr;
			const bool rendererRoutesCurrent = rendererVtable
				&& rendererVtable[kRendererBatchRenderShapeSlot]
					== reinterpret_cast<void*>(&A8BatchRenderShape)
				&& rendererVtable[kRendererRenderShapeSlot]
					== reinterpret_cast<void*>(&A8RenderShape)
				&& rendererVtable[kRendererRenderShapeAltSlot]
					== reinterpret_cast<void*>(&A8RenderShapeAlt);
			return rendererRoutesCurrent || IsA8TileRenderPassHookCurrent();
		}

		void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			if (!IsPublishedRangeBridgeReady())
				State().rangeBridgeAvailable = HookD3DDevice();
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			ActivateDirectBridgeFallback(shape, metadata);
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			BeginA8RenderTrace(shape, "shape-immediate", metadata);
			++ThreadState().renderDepth;
			ThreadState().currentShape = shape;
			ThreadState().currentMetadata = metadata;
			State().originalRenderImmediate(shape, renderer);
			ThreadState().currentShape = previousShape;
			ThreadState().currentMetadata = std::move(previousMetadata);
			--ThreadState().renderDepth;
			EndA8RenderTrace(shape, "shape-immediate");
		}

		void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			if (!IsPublishedRangeBridgeReady())
				State().rangeBridgeAvailable = HookD3DDevice();
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			ActivateDirectBridgeFallback(shape, metadata);
			NiTriShape* previousShape = ThreadState().currentShape;
			A8ShapeMetadataPtr previousMetadata = ThreadState().currentMetadata;
			BeginA8RenderTrace(shape, "shape-immediate-alt", metadata);
			++ThreadState().renderDepth;
			ThreadState().currentShape = shape;
			ThreadState().currentMetadata = metadata;
			State().originalRenderImmediateAlt(shape, renderer);
			ThreadState().currentShape = previousShape;
			ThreadState().currentMetadata = std::move(previousMetadata);
			--ThreadState().renderDepth;
			EndA8RenderTrace(shape, "shape-immediate-alt");
		}

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			A8State& state = State();
			A8ShapeMetadataPtr retiredMetadata;
			{
				std::lock_guard<std::mutex> lock(state.diagnosticsMutex);
				auto found = state.shapeMetadata.find(shape);
				if (found != state.shapeMetadata.end())
				{
					retiredMetadata = std::move(found->second);
					state.shapeMetadata.erase(found);
				}
				state.loggedShapes.erase(shape);
				state.tracedShadowShapes.erase(shape);
				state.tracedShadowShapeOrder.erase(std::remove(
					state.tracedShadowShapeOrder.begin(), state.tracedShadowShapeOrder.end(),
					shape), state.tracedShadowShapeOrder.end());
			}
			ForgetNativeA8FallbackShape(shape);
			// Packet smart pointers may release renderer resources; keep that work out
			// of the metadata registry lock and before the facade itself is destroyed.
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
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
