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
#include <cmath>
#include <cstring>
#include <limits>

namespace fonthook::vectorfont
{
		bool HookD3DDevice();

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

		SInt32 __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode)
		{
			if (!State().originalTileRenderPass)
				return 0;

			NiTriShape* shape = pass
				? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
			const bool tracked = static_cast<bool>(metadata);
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
				if (!State().rangeBridgeAvailable || device != State().hookedDevice)
					State().rangeBridgeAvailable = HookD3DDevice();
				if (g_bEnableFreeTypeFontRenderingLog && !State().loggedTileRenderPassHit)
				{
					State().loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator range route hit shape=%p font=%u pass=%u depth=%u",
						shape, fontId, currentPass, ThreadState().renderDepth);
				}
				BeginA8RenderTrace(shape, "tile-render-pass", metadata);
				++ThreadState().renderDepth;
				ThreadState().currentShape = shape;
				ThreadState().currentMetadata = metadata;
			}
			const SInt32 result = State().originalTileRenderPass(
				pass, currentPass, testAlpha, blendAlpha,
				setupDrawmode);
			if (tracked)
			{
				A8RenderTraceContext* trace = CurrentRenderTrace();
				if (hasShadow && trace && trace->shape == shape
					&& State().loggedTileShadowResultFonts.insert(fontId).second)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator shadow result shape=%p font=%u draws=%u forwarded=%u ranges=%u effectOk=%u effectFail=%u fillOk=%u fillFail=%u bridge=%u",
						shape, fontId, trace->drawCalls, trace->forwardedCalls,
						trace->rangeAttempts,
						trace->effectSuccesses, trace->effectFailures,
						trace->fillSuccesses, trace->fillFailures,
						State().rangeBridgeAvailable ? 1 : 0);
				}
				ThreadState().currentShape = previousShape;
				ThreadState().currentMetadata = std::move(previousMetadata);
				--ThreadState().renderDepth;
				EndA8RenderTrace(shape, "tile-render-pass");
			}
			return result;
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
			return device && device == State().hookedDevice;
		}

		void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			if (!IsPublishedRangeBridgeReady())
				State().rangeBridgeAvailable = HookD3DDevice();
			const A8ShapeMetadataPtr metadata = ResolveRenderMetadata(shape);
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
			{
				std::lock_guard<std::mutex> lock(State().diagnosticsMutex);
				State().shapeMetadata.erase(shape);
				State().loggedShapes.erase(shape);
				State().tracedShadowShapes.erase(shape);
				State().tracedShadowShapeOrder.erase(std::remove(
					State().tracedShadowShapeOrder.begin(), State().tracedShadowShapeOrder.end(),
					shape), State().tracedShadowShapeOrder.end());
			}
			State().originalDeleteThis(shape);
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
