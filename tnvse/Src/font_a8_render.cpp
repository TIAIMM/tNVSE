#include "font_vector_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"

#include "NiD3DPixelShader.hpp"
#include "NiDX9RenderState.hpp"
#include "NiDX9Renderer.hpp"
#include "NiAlphaProperty.hpp"
#include "NiRenderer.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"
#include "BSShaderProperty.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook::vectorfont
{
	static_assert(sizeof(void*) == 4, "FreeType A8 rendering requires the Win32 runtime");
	static_assert(sizeof(BSShaderProperty::RenderPass) == 0x10,
		"Tile RenderPass ABI changed");

	namespace
	{
		constexpr UInt32 kDrawIndexedPrimitiveSlot = 82;
		constexpr UInt32 kDeleteThisSlot = 1;
		constexpr UInt32 kRenderImmediateSlot = 55;
		constexpr UInt32 kRenderImmediateAltSlot = 56;
		constexpr UInt32 kRendererBeginBatchSlot = 103;
		constexpr UInt32 kRendererEndBatchSlot = 104;
		constexpr UInt32 kRendererBatchRenderShapeSlot = 105;
		constexpr UInt32 kRendererRenderShapeSlot = 107;
		constexpr UInt32 kRendererRenderShapeAltSlot = 109;
		constexpr UInt32 kCopiedTriShapeVtableEntries = 64;
		constexpr UInt32 kShaderRefreshMessage = 0;
		constexpr UInt32 kTileRenderPassCallSite = 0xB64FD1;
		constexpr UInt32 kStockTileRenderPassImmediately = 0xB994F0;

		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*,
			D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
		using BeginBatchFn = void(__thiscall*)(NiDX9Renderer*,
			const NiPropertyState*, NiDynamicEffectState*);
		using EndBatchFn = void(__thiscall*)(NiDX9Renderer*);
		using BatchRenderShapeFn = void(__thiscall*)(NiDX9Renderer*, NiTriShape*);
		using RenderImmediateFn = void(__thiscall*)(NiTriShape*, NiRenderer*);
		using RenderShapeFn = void(__thiscall*)(NiDX9Renderer*, NiTriShape*);
		using DeleteThisFn = void(__thiscall*)(NiTriShape*);
		using TileRenderPassFn = void(__cdecl*)(BSShaderProperty::RenderPass*,
			UInt32, bool, bool, bool);

		NiD3DPixelShaderPtr s_a8Shader;
		std::array<NiD3DPixelShaderPtr, 3> s_effectShaders;
		std::array<void*, kCopiedTriShapeVtableEntries + 1> s_a8TriShapeVtable = {};
		void** s_originalTriShapeVtable = nullptr;
		RenderImmediateFn s_originalRenderImmediate = nullptr;
		RenderImmediateFn s_originalRenderImmediateAlt = nullptr;
		DrawIndexedPrimitiveFn s_originalDrawIndexedPrimitive = nullptr;
		BeginBatchFn s_originalBeginBatch = nullptr;
		EndBatchFn s_originalEndBatch = nullptr;
		BatchRenderShapeFn s_originalBatchRenderShape = nullptr;
		RenderShapeFn s_originalRenderShape = nullptr;
		RenderShapeFn s_originalRenderShapeAlt = nullptr;
		DeleteThisFn s_originalDeleteThis = nullptr;
		TileRenderPassFn s_originalTileRenderPass = nullptr;
		IDirect3DDevice9* s_hookedDevice = nullptr;
		bool s_initializationInProgress = false;
		bool s_initializationAttempted = false;
		bool s_shaderLoaderCompatible = false;
		bool s_a8Available = false;
		bool s_rangeBridgeAvailable = false;
		bool s_tileRenderPassHookInstalled = false;
		bool s_loggedTileRenderPassHookConflict = false;
		bool s_loggedTileRenderPassHit = false;
		std::unordered_set<UInt32> s_loggedTileShadowResultFonts;
		bool s_loggedPendingRangeShape = false;
		bool s_loggedShaderLoaderUnavailable = false;
		bool s_loggedA8ShaderLoadFailure = false;
		std::array<bool, 3> s_loggedEffectShaderLoadFailure = {};
		bool s_loggedHookConflict = false;
		bool s_loggedRendererHookConflict = false;
		bool s_loggedBatchRouteHit = false;
		thread_local UInt32 s_a8RenderDepth = 0;
		thread_local NiTriShape* s_currentA8Shape = nullptr;
		UInt32 s_stateMismatchLogCount = 0;
		UInt32 s_shadowContractLogCount = 0;
		UInt32 s_rangeDrawFailureLogCount = 0;
		UInt32 s_shapeValidationFailureLogCount = 0;
		UInt64 s_shadowTraceSerial = 0;
		DWORD s_lastInitializationAttemptTick = 0;
		constexpr UInt32 kMaximumStateMismatchLogs = 16;
		constexpr UInt32 kMaximumRangeDrawFailureLogs = 16;
		constexpr UInt32 kMaximumShapeValidationFailureLogs = 16;
		constexpr UInt32 kMaximumShadowTraceShapes = 256;
		constexpr UInt32 kMaximumRenderTraceDepth = 8;

		struct A8ShapeMetadata
		{
			UInt32 fontId = 0;
			UInt32 glyphCount = 0;
			UInt32 quadCount = 0;
			UInt32 vertexCount = 0;
			UInt32 primitiveCount = 0;
			UInt32 indexCount = 0;
			A8ShapeColorContract colorContract;
			A8EffectShapeConfig effects;
		};

		struct A8RenderTraceContext
		{
			UInt64 serial = 0;
			NiTriShape* shape = nullptr;
			const char* entryPoint = nullptr;
			bool detailed = false;
			UInt32 drawCalls = 0;
			UInt32 forwardedCalls = 0;
			UInt32 rangeAttempts = 0;
			UInt32 effectSuccesses = 0;
			UInt32 effectFailures = 0;
			UInt32 fillSuccesses = 0;
			UInt32 fillFailures = 0;
		};

		std::mutex s_diagnosticsMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadata> s_shapeMetadata;
		std::unordered_set<const NiTriShape*> s_loggedShapes;
		std::unordered_set<const NiTriShape*> s_tracedShadowShapes;
		std::deque<const NiTriShape*> s_tracedShadowShapeOrder;
		UInt32 s_diagnosticLogCount = 0;
		constexpr UInt32 kMaximumDiagnosticShapes = 128;
		thread_local std::array<A8RenderTraceContext, kMaximumRenderTraceDepth>
			s_renderTraceStack = {};
		thread_local UInt32 s_renderTraceDepth = 0;

		bool HasShadowRange(const A8ShapeMetadata& metadata)
		{
			return std::any_of(metadata.effects.ranges.begin(),
				metadata.effects.ranges.end(), [](const A8DrawRange& range)
				{
					return range.layer == 0 && range.vertexCount && range.primitiveCount;
				});
		}

		A8RenderTraceContext* CurrentRenderTrace()
		{
			return s_renderTraceDepth
				? &s_renderTraceStack[s_renderTraceDepth - 1] : nullptr;
		}

		bool IsDetailedShadowTraceActive()
		{
			for (UInt32 index = 0; index < s_renderTraceDepth; ++index)
			{
				const A8RenderTraceContext& trace = s_renderTraceStack[index];
				if (trace.detailed && trace.shape == s_currentA8Shape)
					return true;
			}
			return false;
		}

		template <class Callback>
		void UpdateCurrentRenderTraces(Callback&& callback)
		{
			for (UInt32 index = 0; index < s_renderTraceDepth; ++index)
			{
				A8RenderTraceContext& trace = s_renderTraceStack[index];
				if (trace.shape == s_currentA8Shape)
					callback(trace);
			}
		}

		void BeginA8RenderTrace(NiTriShape* shape, const char* entryPoint)
		{
			if (s_renderTraceDepth >= kMaximumRenderTraceDepth)
				return;

			A8RenderTraceContext& trace = s_renderTraceStack[s_renderTraceDepth++];
			trace = {};
			trace.serial = ++s_shadowTraceSerial;
			trace.shape = shape;
			trace.entryPoint = entryPoint;

			A8ShapeMetadata metadata;
			bool haveMetadata = false;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				auto found = s_shapeMetadata.find(shape);
				if (found != s_shapeMetadata.end())
				{
					metadata = found->second;
					haveMetadata = true;
				}
				bool inherited = false;
				if (s_renderTraceDepth > 1)
				{
					const A8RenderTraceContext& parent =
						s_renderTraceStack[s_renderTraceDepth - 2];
					inherited = parent.detailed && parent.shape == shape;
				}
				const bool newlyTraced = g_bEnableFreeTypeFontRenderingLog
					&& haveMetadata && HasShadowRange(metadata)
					&& s_tracedShadowShapes.insert(shape).second;
				trace.detailed = inherited || newlyTraced;
				if (newlyTraced)
				{
					s_tracedShadowShapeOrder.push_back(shape);
					while (s_tracedShadowShapeOrder.size() > kMaximumShadowTraceShapes)
					{
						const NiTriShape* oldest = s_tracedShadowShapeOrder.front();
						s_tracedShadowShapeOrder.pop_front();
						s_tracedShadowShapes.erase(oldest);
					}
				}
			}

			if (trace.detailed)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: begin serial=%llu scopeDepth=%u entry=%s shape=%p parent=%p metadata=%u font=%u glyphs=%u quads=%u expected=(vertices=%u primitives=%u indices=%u ranges=%u)",
					static_cast<unsigned long long>(trace.serial), s_renderTraceDepth,
					entryPoint ? entryPoint : "unknown", shape,
					shape ? shape->m_pkParent : nullptr, haveMetadata ? 1 : 0,
					metadata.fontId, metadata.glyphCount, metadata.quadCount,
					metadata.vertexCount, metadata.primitiveCount, metadata.indexCount,
					static_cast<UInt32>(metadata.effects.ranges.size()));
			}
		}

		void EndA8RenderTrace(NiTriShape* shape, const char* entryPoint)
		{
			if (!s_renderTraceDepth)
				return;
			A8RenderTraceContext trace = s_renderTraceStack[s_renderTraceDepth - 1];
			--s_renderTraceDepth;
			if (!trace.detailed)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_shadow_trace: end serial=%llu scopeDepth=%u entry=%s shape=%p expectedShape=%p draws=%u forwarded=%u rangeAttempts=%u effect=(ok=%u fail=%u) fill=(ok=%u fail=%u) verdict=%s",
				static_cast<unsigned long long>(trace.serial), s_renderTraceDepth + 1,
				entryPoint ? entryPoint : "unknown", shape, trace.shape,
				trace.drawCalls, trace.forwardedCalls, trace.rangeAttempts,
				trace.effectSuccesses, trace.effectFailures,
				trace.fillSuccesses, trace.fillFailures,
				trace.fillSuccesses ? "fill-submitted"
					: trace.forwardedCalls ? "original-draw-forwarded"
					: trace.drawCalls ? "fill-missing-or-failed" : "no-dip-observed");
		}

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
			std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
			const auto found = s_shapeMetadata.find(shape);
			if (found == s_shapeMetadata.end())
				return false;
			if (hasShadow)
				*hasShadow = HasShadowRange(found->second);
			if (fontId)
				*fontId = found->second.fontId;
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

		void __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode)
		{
			if (!s_originalTileRenderPass)
				return;

			NiTriShape* shape = pass
				? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
			bool hasShadow = false;
			UInt32 fontId = 0;
			const bool tracked = HasA8ShapeMetadata(shape, &hasShadow, &fontId);
			NiTriShape* previousShape = s_currentA8Shape;
			if (tracked)
			{
				// Startup menus can reach the Tile accumulator before NVSE's
				// DeferredInit message. At this call site the renderer/device are live,
				// so make one final synchronous attempt before this shape's first DIP.
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				IDirect3DDevice9* device = renderer
					? renderer->GetD3DDevice() : nullptr;
				if (!s_rangeBridgeAvailable || device != s_hookedDevice)
					s_rangeBridgeAvailable = HookD3DDevice();
				if (g_bEnableFreeTypeFontRenderingLog && !s_loggedTileRenderPassHit)
				{
					s_loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator range route hit shape=%p font=%u pass=%u depth=%u",
						shape, fontId, currentPass, s_a8RenderDepth);
				}
				BeginA8RenderTrace(shape, "tile-render-pass");
				++s_a8RenderDepth;
				s_currentA8Shape = shape;
			}
			s_originalTileRenderPass(pass, currentPass, testAlpha, blendAlpha,
				setupDrawmode);
			if (tracked)
			{
				A8RenderTraceContext* trace = CurrentRenderTrace();
				if (hasShadow && trace && trace->shape == shape
					&& s_loggedTileShadowResultFonts.insert(fontId).second)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator shadow result shape=%p font=%u draws=%u forwarded=%u ranges=%u effectOk=%u effectFail=%u fillOk=%u fillFail=%u bridge=%u",
						shape, fontId, trace->drawCalls, trace->forwardedCalls,
						trace->rangeAttempts,
						trace->effectSuccesses, trace->effectFailures,
						trace->fillSuccesses, trace->fillFailures,
						s_rangeBridgeAvailable ? 1 : 0);
				}
				s_currentA8Shape = previousShape;
				--s_a8RenderDepth;
				EndA8RenderTrace(shape, "tile-render-pass");
			}
		}

		bool HookTileRenderPass()
		{
			TileRenderPassFn current = ReadTileRenderPassCallTarget();
			const TileRenderPassFn hook = &A8TileRenderPass;
			if (current == hook)
			{
				s_tileRenderPassHookInstalled = s_originalTileRenderPass != nullptr;
				return s_tileRenderPassHookInstalled;
			}
			if (!current)
			{
				if (!s_loggedTileRenderPassHookConflict)
				{
					s_loggedTileRenderPassHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator call site is not CALL rel32; startup range route unavailable");
				}
				return false;
			}
			if (s_tileRenderPassHookInstalled)
			{
				if (!s_loggedTileRenderPassHookConflict)
				{
					s_loggedTileRenderPassHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator range route was replaced; retaining safe fallback routes");
				}
				return false;
			}
			if (reinterpret_cast<UInt32>(current)
				!= kStockTileRenderPassImmediately)
			{
				if (!s_loggedTileRenderPassHookConflict)
				{
					s_loggedTileRenderPassHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Tile accumulator call site already has a non-stock target=%p; leaving it untouched",
						current);
				}
				return false;
			}

			// Only wrap the verified stock cdecl target. An arbitrary pre-existing
			// naked hook may depend on volatile registers from the original call site;
			// invoking it through a C++ wrapper would not be a safe compatibility chain.
			s_originalTileRenderPass = current;
			WriteRelCall(kTileRenderPassCallSite, hook);
			s_tileRenderPassHookInstalled = ReadTileRenderPassCallTarget() == hook;
			if (!s_tileRenderPassHookInstalled)
			{
				s_originalTileRenderPass = nullptr;
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

		void LogShadowTraceDeviceState(IDirect3DDevice9* device, const char* stage,
			UInt32 drawCall, int layer, IDirect3DPixelShader9* expectedShader,
			HRESULT result)
		{
			A8RenderTraceContext* trace = CurrentRenderTrace();
			if (!device || !trace || !trace->detailed
				|| trace->shape != s_currentA8Shape)
			{
				return;
			}

			IDirect3DPixelShader9* actualShader = nullptr;
			IDirect3DBaseTexture9* texture = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DSurface9* renderTarget = nullptr;
			IDirect3DSurface9* backBuffer = nullptr;
			UINT streamOffset = 0;
			UINT streamStride = 0;
			device->GetPixelShader(&actualShader);
			device->GetTexture(0, &texture);
			device->GetStreamSource(0, &vertexBuffer, &streamOffset, &streamStride);
			device->GetIndices(&indexBuffer);
			device->GetRenderTarget(0, &renderTarget);
			device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

			std::array<float, 20> constants = {};
			device->GetPixelShaderConstantF(0, constants.data(), 5);
			DWORD alphaBlend = 0;
			DWORD sourceBlend = 0;
			DWORD destinationBlend = 0;
			DWORD blendOperation = 0;
			DWORD separateAlpha = 0;
			DWORD sourceAlphaBlend = 0;
			DWORD destinationAlphaBlend = 0;
			DWORD alphaBlendOperation = 0;
			DWORD alphaTest = 0;
			DWORD alphaFunction = 0;
			DWORD alphaReference = 0;
			DWORD zWrite = 0;
			DWORD zEnable = 0;
			DWORD colorWrite = 0;
			DWORD scissorEnable = 0;
			DWORD stencilEnable = 0;
			DWORD stencilWriteMask = 0;
			DWORD minFilter = 0;
			DWORD magFilter = 0;
			DWORD mipFilter = 0;
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
			device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend);
			device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend);
			device->GetRenderState(D3DRS_BLENDOP, &blendOperation);
			device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
			device->GetRenderState(D3DRS_SRCBLENDALPHA, &sourceAlphaBlend);
			device->GetRenderState(D3DRS_DESTBLENDALPHA, &destinationAlphaBlend);
			device->GetRenderState(D3DRS_BLENDOPALPHA, &alphaBlendOperation);
			device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
			device->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunction);
			device->GetRenderState(D3DRS_ALPHAREF, &alphaReference);
			device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
			device->GetRenderState(D3DRS_ZENABLE, &zEnable);
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
			device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnable);
			device->GetRenderState(D3DRS_STENCILENABLE, &stencilEnable);
			device->GetRenderState(D3DRS_STENCILWRITEMASK, &stencilWriteMask);
			device->GetSamplerState(0, D3DSAMP_MINFILTER, &minFilter);
			device->GetSamplerState(0, D3DSAMP_MAGFILTER, &magFilter);
			device->GetSamplerState(0, D3DSAMP_MIPFILTER, &mipFilter);

			D3DSURFACE_DESC textureDescription = {};
			D3DSURFACE_DESC targetDescription = {};
			if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
			{
				static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0,
					&textureDescription);
			}
			if (renderTarget)
				renderTarget->GetDesc(&targetDescription);
			FreeTypeFontDebugLog(
				"tnvse_freetype_shadow_trace: state serial=%llu dip=%u stage=%s layer=%d hr=0x%08X ps=(actual=%p expected=%p match=%u) tex=(ptr=%p format=%u size=%ux%u) rt0=(ptr=%p backbuffer=%u format=%u size=%ux%u) buffers=(vb=%p offset=%u stride=%u ib=%p)",
				static_cast<unsigned long long>(trace->serial), drawCall,
				stage ? stage : "unknown", layer, static_cast<UInt32>(result),
				actualShader, expectedShader,
				!expectedShader || actualShader == expectedShader ? 1 : 0,
				texture, static_cast<UInt32>(textureDescription.Format),
				textureDescription.Width, textureDescription.Height, renderTarget,
				renderTarget && renderTarget == backBuffer ? 1 : 0,
				static_cast<UInt32>(targetDescription.Format), targetDescription.Width,
				targetDescription.Height, vertexBuffer,
				streamOffset, streamStride, indexBuffer);
			FreeTypeFontDebugLog(
				"tnvse_freetype_shadow_trace:   constants c0=(%.5g,%.5g,%.5g,%.5g) c1=(%.5g,%.5g,%.5g,%.5g) c2=(%.5g,%.5g,%.5g,%.5g) c3=(%.5g,%.5g,%.5g,%.5g) c4=(%.5g,%.5g,%.5g,%.5g)",
				constants[0], constants[1], constants[2], constants[3],
				constants[4], constants[5], constants[6], constants[7],
				constants[8], constants[9], constants[10], constants[11],
				constants[12], constants[13], constants[14], constants[15],
				constants[16], constants[17], constants[18], constants[19]);
			FreeTypeFontDebugLog(
				"tnvse_freetype_shadow_trace:   renderState blend=(enable=%u rgb=%u/%u op=%u separate=%u alpha=%u/%u op=%u) alphaTest=(enable=%u func=%u ref=%u) depth=(enable=%u write=%u) colorWrite=0x%X scissor=%u stencil=(enable=%u writeMask=0x%X) sampler=(min=%u mag=%u mip=%u)",
				alphaBlend, sourceBlend, destinationBlend, blendOperation,
				separateAlpha, sourceAlphaBlend, destinationAlphaBlend,
				alphaBlendOperation, alphaTest, alphaFunction, alphaReference,
				zEnable, zWrite, colorWrite, scissorEnable, stencilEnable,
				stencilWriteMask, minFilter, magFilter,
				mipFilter);

			if (indexBuffer)
				indexBuffer->Release();
			if (backBuffer)
				backBuffer->Release();
			if (renderTarget)
				renderTarget->Release();
			if (vertexBuffer)
				vertexBuffer->Release();
			if (texture)
				texture->Release();
			if (actualShader)
				actualShader->Release();
		}

		void LogA8DrawDiagnostics(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex,
			UINT minimumVertexIndex, UINT numberOfVertices,
			UINT startIndex, UINT primitiveCount)
		{
			if (!g_bEnableFreeTypeFontRenderingLog || !device || !s_currentA8Shape)
				return;

			A8ShapeMetadata metadata;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				if (s_diagnosticLogCount >= kMaximumDiagnosticShapes
					|| !s_loggedShapes.insert(s_currentA8Shape).second)
				{
					return;
				}
				++s_diagnosticLogCount;
				auto found = s_shapeMetadata.find(s_currentA8Shape);
				if (found != s_shapeMetadata.end())
					metadata = found->second;
			}

			IDirect3DPixelShader9* pixelShader = nullptr;
			IDirect3DVertexShader9* vertexShader = nullptr;
			IDirect3DBaseTexture9* texture = nullptr;
			device->GetPixelShader(&pixelShader);
			device->GetVertexShader(&vertexShader);
			device->GetTexture(0, &texture);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DPixelShader9* cachedPixelShader = renderer && renderer->m_pkRenderState
				? renderer->m_pkRenderState->GetPixelShader() : nullptr;

			DWORD alphaBlend = 0, sourceBlend = 0, destinationBlend = 0, blendOperation = 0;
			DWORD separateAlpha = 0, sourceBlendAlpha = 0, destinationBlendAlpha = 0;
			DWORD blendOperationAlpha = 0, alphaTest = 0, alphaFunction = 0;
			DWORD alphaReference = 0;
			DWORD zWrite = 0, colorWrite = 0, textureFactor = 0;
			DWORD minFilter = 0, magFilter = 0, mipFilter = 0;
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
			device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend);
			device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend);
			device->GetRenderState(D3DRS_BLENDOP, &blendOperation);
			device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
			device->GetRenderState(D3DRS_SRCBLENDALPHA, &sourceBlendAlpha);
			device->GetRenderState(D3DRS_DESTBLENDALPHA, &destinationBlendAlpha);
			device->GetRenderState(D3DRS_BLENDOPALPHA, &blendOperationAlpha);
			device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
			device->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunction);
			device->GetRenderState(D3DRS_ALPHAREF, &alphaReference);
			device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
			device->GetRenderState(D3DRS_TEXTUREFACTOR, &textureFactor);
			device->GetSamplerState(0, D3DSAMP_MINFILTER, &minFilter);
			device->GetSamplerState(0, D3DSAMP_MAGFILTER, &magFilter);
			device->GetSamplerState(0, D3DSAMP_MIPFILTER, &mipFilter);

			std::array<float, 16> pixelConstants = {};
			std::array<float, 32> vertexConstants = {};
			device->GetPixelShaderConstantF(0, pixelConstants.data(), 4);
			device->GetVertexShaderConstantF(4, vertexConstants.data(), 8);

			NiPoint3 minimumVertex(std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
			NiPoint3 maximumVertex(-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
			NiPoint2 minimumUv(std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity());
			NiPoint2 maximumUv(-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity());
			UInt32 vertexCount = 0;
			UInt32 triangleCount = 0;
			if (NiTriShapeData* data = s_currentA8Shape->GetModelData())
			{
				vertexCount = data->m_usVertices;
				triangleCount = data->m_uiTriListLength / 3;
				for (UInt32 index = 0; index < vertexCount; ++index)
				{
					if (data->m_pkVertex)
					{
						const NiPoint3& vertex = data->m_pkVertex[index];
						minimumVertex.x = std::min(minimumVertex.x, vertex.x);
						minimumVertex.y = std::min(minimumVertex.y, vertex.y);
						minimumVertex.z = std::min(minimumVertex.z, vertex.z);
						maximumVertex.x = std::max(maximumVertex.x, vertex.x);
						maximumVertex.y = std::max(maximumVertex.y, vertex.y);
						maximumVertex.z = std::max(maximumVertex.z, vertex.z);
					}
					if (data->m_pkTexture)
					{
						const NiPoint2& uv = data->m_pkTexture[index];
						minimumUv.x = std::min(minimumUv.x, uv.x);
						minimumUv.y = std::min(minimumUv.y, uv.y);
						maximumUv.x = std::max(maximumUv.x, uv.x);
						maximumUv.y = std::max(maximumUv.y, uv.y);
					}
				}
			}
			if (!std::isfinite(minimumVertex.x))
				minimumVertex = maximumVertex = NiPoint3(-1.0f, -1.0f, -1.0f);
			if (!std::isfinite(minimumUv.x))
				minimumUv = maximumUv = NiPoint2(-1.0f, -1.0f);

			UInt16 alphaFlags = 0;
			UInt8 propertyAlphaReference = 0;
			if (NiAlphaProperty* property = s_currentA8Shape->GetAlphaProperty())
			{
				alphaFlags = property->m_usFlags.Get();
				propertyAlphaReference = property->m_ucAlphaTestRef;
			}
			NiColorA shadeColor = { -1.0f, -1.0f, -1.0f, -1.0f };
			if (NiShadeProperty* shade = s_currentA8Shape->GetShadeProperty())
				shadeColor = *reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68);

			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag: shape=%p parent=%p font=%u glyphs=%u quads=%u vertices=%u triangles=%u draw=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u)",
				s_currentA8Shape, s_currentA8Shape->m_pkParent, metadata.fontId,
				metadata.glyphCount, metadata.quadCount, vertexCount, triangleCount,
				static_cast<UInt32>(primitiveType), baseVertexIndex,
				minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   local=(%.3f,%.3f,%.3f scale=%.3f) world=(%.3f,%.3f,%.3f scale=%.3f) shadeColor=(%.4f,%.4f,%.4f,%.4f) alphaProperty=(flags=0x%04X ref=%u)",
				s_currentA8Shape->m_kLocal.m_Translate.x, s_currentA8Shape->m_kLocal.m_Translate.y,
				s_currentA8Shape->m_kLocal.m_Translate.z, s_currentA8Shape->m_kLocal.m_fScale,
				s_currentA8Shape->m_kWorld.m_Translate.x, s_currentA8Shape->m_kWorld.m_Translate.y,
				s_currentA8Shape->m_kWorld.m_Translate.z, s_currentA8Shape->m_kWorld.m_fScale,
				shadeColor.r, shadeColor.g, shadeColor.b, shadeColor.a,
				alphaFlags, propertyAlphaReference);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   contract=tile-fill-effect-rgb-v7 abi=%u rgb=(fill:c0.rgb*c1.rgb effect:c1.rgb) alpha=coverage*c0.a*c1.a modifier=[(%.4f,%.4f,%.4f,%.4f)..(%.4f,%.4f,%.4f,%.4f)]",
				metadata.colorContract.abiVersion,
				metadata.colorContract.minimumModifier.r,
				metadata.colorContract.minimumModifier.g,
				metadata.colorContract.minimumModifier.b,
				metadata.colorContract.minimumModifier.a,
				metadata.colorContract.maximumModifier.r,
				metadata.colorContract.maximumModifier.g,
				metadata.colorContract.maximumModifier.b,
				metadata.colorContract.maximumModifier.a);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   geometry xyz=[(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)] uv=[(%.6f,%.6f)..(%.6f,%.6f)]",
				minimumVertex.x, minimumVertex.y, minimumVertex.z,
				maximumVertex.x, maximumVertex.y, maximumVertex.z,
				minimumUv.x, minimumUv.y, maximumUv.x, maximumUv.y);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   d3d ps=%p cachedPs=%p cacheMatch=%u vs=%p tex0=%p blend=(enable=%u rgb=%u/%u op=%u separate=%u alpha=%u/%u op=%u) alphaTest=(enable=%u func=%u ref=%u) zWrite=%u colorWrite=0x%X textureFactor=0x%08X sampler=(min=%u mag=%u mip=%u)",
				pixelShader, cachedPixelShader, pixelShader == cachedPixelShader,
				vertexShader, texture, alphaBlend, sourceBlend, destinationBlend,
				blendOperation, separateAlpha, sourceBlendAlpha, destinationBlendAlpha,
				blendOperationAlpha, alphaTest, alphaFunction, alphaReference,
				zWrite, colorWrite, textureFactor, minFilter, magFilter, mipFilter);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   ps_c0_c3=(%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g)",
				pixelConstants[0], pixelConstants[1], pixelConstants[2], pixelConstants[3],
				pixelConstants[4], pixelConstants[5], pixelConstants[6], pixelConstants[7],
				pixelConstants[8], pixelConstants[9], pixelConstants[10], pixelConstants[11],
				pixelConstants[12], pixelConstants[13], pixelConstants[14], pixelConstants[15]);
			if (metadata.effects.enabled)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag:   effects quality=%u shaderEffects=%u fillUsesSdf=%u atlasTexel=(%.7f,%.7f) spread=%.3f shadow=(blur=%.3f power=%.3f) glow=(inner=%.3f outer=%.3f power=%.3f) outline=(width=%.3f softness=%.3f) contract=tile-fill-effect-rgb-v7",
					static_cast<UInt32>(metadata.effects.quality),
					metadata.effects.shaderEffects ? 1 : 0,
					metadata.effects.fillUsesSdf ? 1 : 0,
					metadata.effects.inverseAtlasWidth,
					metadata.effects.inverseAtlasHeight,
					metadata.effects.sdfSpreadPixels,
					metadata.effects.shadowBlurPixels,
					metadata.effects.shadowPower,
					metadata.effects.glowInnerPixels,
					metadata.effects.glowOuterPixels,
					metadata.effects.glowPower,
					metadata.effects.outlineWidthPixels,
					metadata.effects.outlineSoftnessPixels);
				for (UInt32 rangeIndex = 0;
					rangeIndex < metadata.effects.ranges.size(); ++rangeIndex)
				{
					const A8DrawRange& range = metadata.effects.ranges[rangeIndex];
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag:     range=%u layer=%u usesSdf=%u color=(%.4f,%.4f,%.4f,%.4f) vertices=(first=%u count=%u) indices=(start=%u primitives=%u)",
						rangeIndex, range.layer, range.usesSdf ? 1 : 0,
						range.colorModifier.r, range.colorModifier.g,
						range.colorModifier.b, range.colorModifier.a,
						range.firstVertex, range.vertexCount,
						range.startIndex, range.primitiveCount);
				}
			}
			for (UInt32 row = 0; row < 2; ++row)
			{
				const UInt32 offset = row * 16;
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag:   vs_c%u_c%u=(%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g)",
					4 + row * 4, 7 + row * 4,
					vertexConstants[offset + 0], vertexConstants[offset + 1],
					vertexConstants[offset + 2], vertexConstants[offset + 3],
					vertexConstants[offset + 4], vertexConstants[offset + 5],
					vertexConstants[offset + 6], vertexConstants[offset + 7],
					vertexConstants[offset + 8], vertexConstants[offset + 9],
					vertexConstants[offset + 10], vertexConstants[offset + 11],
					vertexConstants[offset + 12], vertexConstants[offset + 13],
					vertexConstants[offset + 14], vertexConstants[offset + 15]);
			}

			if (texture)
				texture->Release();
			if (vertexShader)
				vertexShader->Release();
			if (pixelShader)
				pixelShader->Release();
		}

		bool HaveA8Shader()
		{
			return s_a8Shader && s_a8Shader->GetShaderHandle();
		}

		bool HaveEffectShader(EffectQuality quality)
		{
			const size_t index = static_cast<size_t>(quality);
			return index < s_effectShaders.size() && s_effectShaders[index]
				&& s_effectShaders[index]->GetShaderHandle();
		}

		bool HaveAllEffectShaders()
		{
			for (size_t index = 0; index < s_effectShaders.size(); ++index)
			{
				if (!HaveEffectShader(static_cast<EffectQuality>(index)))
					return false;
			}
			return true;
		}

		bool NeedsScaledFillSampling(const NiTriShape* shape)
		{
			if (!shape)
				return false;
			const float worldScale = std::abs(shape->m_kWorld.m_fScale);
			return std::isfinite(worldScale) && std::abs(worldScale - 1.0f) > 0.001f;
		}

		bool LoadA8Shader(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return false;
			NiD3DPixelShaderPtr loaded = createPixelShader("tnvse_freetype_a8.pso");
			if (!loaded || !loaded->GetShaderHandle())
			{
				if (!s_loggedA8ShaderLoadFailure)
				{
					s_loggedA8ShaderLoadFailure = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Shader Loader failed to load tnvse_freetype_a8.pso; using 32-bit atlases and retrying");
				}
				return false;
			}
			s_loggedA8ShaderLoadFailure = false;
			s_a8Shader = loaded;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog("tnvse_freetype_font: loaded Shader Loader A8 font shader");
			return true;
		}

		void LoadEffectShaders(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return;
			const char* names[] = {
				"tnvse_freetype_effects_fast.pso",
				"tnvse_freetype_effects_balanced.pso",
				"tnvse_freetype_effects_high.pso"
			};
			for (size_t index = 0; index < s_effectShaders.size(); ++index)
			{
				NiD3DPixelShaderPtr loaded = createPixelShader(names[index]);
				if (loaded && loaded->GetShaderHandle())
				{
					s_effectShaders[index] = loaded;
					s_loggedEffectShaderLoadFailure[index] = false;
					if (g_bEnableFreeTypeFontRenderingLog)
						FreeTypeFontDebugLog("tnvse_freetype_font: loaded %s", names[index]);
				}
				else if (!s_effectShaders[index]
					&& !s_loggedEffectShaderLoadFailure[index])
				{
					s_loggedEffectShaderLoadFailure[index] = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: failed to load %s; using a lower shader quality or CPU effects",
						names[index]);
				}
			}
		}

		bool RefreshShaderSet(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return false;
			NiD3DPixelShaderPtr base = createPixelShader("tnvse_freetype_a8.pso");
			if (!base || !base->GetShaderHandle())
				return false;
			const char* names[] = {
				"tnvse_freetype_effects_fast.pso",
				"tnvse_freetype_effects_balanced.pso",
				"tnvse_freetype_effects_high.pso"
			};
			std::array<NiD3DPixelShaderPtr, 3> effects;
			for (size_t index = 0; index < effects.size(); ++index)
			{
				effects[index] = createPixelShader(names[index]);
				if (!effects[index] || !effects[index]->GetShaderHandle())
					return false;
			}
			s_a8Shader = base;
			s_effectShaders = effects;
			if (g_bEnableFreeTypeFontRenderingLog)
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: atomically refreshed Tile-compatible shader set contract=tile-fill-effect-rgb-v7");
			return true;
		}

		void LogStateIsolationFailure(const char* reason,
			IDirect3DPixelShader9* actual = nullptr,
			IDirect3DPixelShader9* cached = nullptr)
		{
			if (s_stateMismatchLogCount++ >= kMaximumStateMismatchLogs)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: custom shader state diagnostic reason=%s actualPs=%p cachedPs=%p",
				reason ? reason : "unknown", actual, cached);
		}

		class ScopedA8RenderState
		{
		public:
			explicit ScopedA8RenderState(IDirect3DDevice9* device) : m_device(device)
			{
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				m_cachedRenderState = renderer && renderer->GetD3DDevice() == device
					? renderer->m_pkRenderState : nullptr;
				if (!m_device)
				{
					LogStateIsolationFailure("d3d-device-unavailable");
					return;
				}
				m_supportsSeparateAlphaBlend = renderer
					&& (renderer->m_kD3DCaps9.PrimitiveMiscCaps
						& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;

				if (FAILED(m_device->GetPixelShader(&m_originalPixelShader)))
				{
					LogStateIsolationFailure("get-pixel-shader-failed");
					return;
				}
				IDirect3DPixelShader9* cached = m_cachedRenderState
					? m_cachedRenderState->GetPixelShader() : nullptr;
				if (m_cachedRenderState && cached != m_originalPixelShader)
				{
					// NVR and other render extensions can update the D3D device without
					// updating Gamebryo's shadow cache. The actual device state is the
					// authoritative state for this transient draw bridge.
					LogStateIsolationFailure("pixel-shader-cache-mismatch",
						m_originalPixelShader, cached);
				}
				if (FAILED(m_device->GetPixelShaderConstantF(1,
					m_originalConstants.data(), 4)))
				{
					LogStateIsolationFailure("get-private-constants-failed");
					return;
				}
				if (FAILED(m_device->GetPixelShaderConstantF(0, m_originalTileColor.data(), 1)))
				{
					LogStateIsolationFailure("get-tile-color-failed");
					return;
				}

				for (SamplerSetting& setting : m_samplerSettings)
				{
					if (FAILED(m_device->GetSamplerState(0, setting.state, &setting.value)))
					{
						LogStateIsolationFailure("get-sampler-state-failed");
						return;
					}
				}
				if (FAILED(m_device->GetRenderState(D3DRS_ALPHABLENDENABLE,
					&m_originalAlphaBlend))
					|| FAILED(m_device->GetRenderState(D3DRS_ALPHATESTENABLE,
					&m_originalAlphaTest))
					|| FAILED(m_device->GetRenderState(D3DRS_ZWRITEENABLE,
						&m_originalZWrite))
					|| FAILED(m_device->GetRenderState(D3DRS_COLORWRITEENABLE,
						&m_originalColorWrite))
					|| FAILED(m_device->GetRenderState(D3DRS_STENCILWRITEMASK,
						&m_originalStencilWriteMask)))
				{
					LogStateIsolationFailure("get-render-state-failed");
					return;
				}
				if (m_supportsSeparateAlphaBlend
					&& (FAILED(m_device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
						&m_originalSeparateAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_SRCBLENDALPHA,
							&m_originalSourceAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_DESTBLENDALPHA,
							&m_originalDestinationAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_BLENDOPALPHA,
							&m_originalAlphaBlendOperation))))
				{
					LogStateIsolationFailure("get-separate-alpha-state-failed");
					return;
				}
				m_valid = true;
			}

			~ScopedA8RenderState()
			{
				if (m_modified && m_device)
				{
					m_device->SetPixelShaderConstantF(0,
						m_originalTileColor.data(), 1);
					m_device->SetPixelShaderConstantF(1, m_originalConstants.data(), 4);
					for (const SamplerSetting& setting : m_samplerSettings)
						m_device->SetSamplerState(0, setting.state, setting.value);
					m_device->SetRenderState(D3DRS_ALPHATESTENABLE, m_originalAlphaTest);
					m_device->SetRenderState(D3DRS_ZWRITEENABLE, m_originalZWrite);
					m_device->SetRenderState(D3DRS_COLORWRITEENABLE, m_originalColorWrite);
					if (m_supportsSeparateAlphaBlend)
					{
						m_device->SetRenderState(D3DRS_SRCBLENDALPHA,
							m_originalSourceAlphaBlend);
						m_device->SetRenderState(D3DRS_DESTBLENDALPHA,
							m_originalDestinationAlphaBlend);
						m_device->SetRenderState(D3DRS_BLENDOPALPHA,
							m_originalAlphaBlendOperation);
						m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
							m_originalSeparateAlphaBlend);
					}
					m_device->SetRenderState(D3DRS_STENCILWRITEMASK,
						m_originalStencilWriteMask);
					m_device->SetPixelShader(m_originalPixelShader);
					for (const SamplerSetting& setting : m_samplerSettings)
					{
						DWORD restoredValue = 0;
						if (FAILED(m_device->GetSamplerState(0, setting.state, &restoredValue))
							|| restoredValue != setting.value)
						{
							LogStateIsolationFailure("sampler-restore-mismatch");
							break;
						}
					}
					std::array<float, 4> tileColor = {};
					if (FAILED(m_device->GetPixelShaderConstantF(0, tileColor.data(), 1))
						|| tileColor != m_originalTileColor)
					{
						LogStateIsolationFailure("tile-color-c0-changed");
					}
					VerifyRestoredRenderState(D3DRS_ALPHATESTENABLE,
						m_originalAlphaTest, "alpha-test-restore-mismatch");
					VerifyRestoredRenderState(D3DRS_ZWRITEENABLE,
						m_originalZWrite, "z-write-restore-mismatch");
					VerifyRestoredRenderState(D3DRS_COLORWRITEENABLE,
						m_originalColorWrite, "color-write-restore-mismatch");
					if (m_supportsSeparateAlphaBlend)
					{
						VerifyRestoredRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
							m_originalSeparateAlphaBlend,
							"separate-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_SRCBLENDALPHA,
							m_originalSourceAlphaBlend,
							"source-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_DESTBLENDALPHA,
							m_originalDestinationAlphaBlend,
							"destination-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_BLENDOPALPHA,
							m_originalAlphaBlendOperation,
							"alpha-operation-restore-mismatch");
					}
					VerifyRestoredRenderState(D3DRS_STENCILWRITEMASK,
						m_originalStencilWriteMask,
						"stencil-write-mask-restore-mismatch");

					IDirect3DPixelShader9* restored = nullptr;
					if (SUCCEEDED(m_device->GetPixelShader(&restored)))
					{
						if (restored != m_originalPixelShader)
						{
							LogStateIsolationFailure("pixel-shader-restore-mismatch",
								restored, m_originalPixelShader);
						}
						if (restored)
							restored->Release();
					}
				}
				if (m_originalPixelShader)
					m_originalPixelShader->Release();
			}

			bool IsValid() const { return m_valid; }

			bool Apply(IDirect3DPixelShader9* shader)
			{
				if (!m_valid || !shader)
					return false;
				// Mark the guard dirty before the first mutation so a partial failure is
				// still restored. RGB blend, depth-test, scissor, stencil test/ref and
				// stream state remain exactly as established by the original Tile pass.
				m_modified = true;
				return SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_MINFILTER,
					D3DTEXF_POINT))
					&& SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_MAGFILTER,
						D3DTEXF_POINT))
					&& SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_MIPFILTER,
						D3DTEXF_NONE))
					&& SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_ADDRESSU,
						D3DTADDRESS_CLAMP))
					&& SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_ADDRESSV,
						D3DTADDRESS_CLAMP))
					&& SUCCEEDED(m_device->SetPixelShader(shader));
			}

			bool SetPassState(bool effectPass, bool customCoverageShader)
			{
				if (!m_valid)
					return false;
				// Fill follows the original TILE1000 c0 contract. Effects instead own
				// their configured RGB and inherit only the live Tile alpha. This avoids
				// dim/zero Tile RGB channels destroying a configured shadow color, while
				// preserving menu fades and per-glyph alpha exactly.
				const std::array<float, 4> passTileColor = effectPass
					? std::array<float, 4>{ 1.0f, 1.0f, 1.0f,
						m_originalTileColor[3] }
					: m_originalTileColor;
				// Off-screen UI targets consume destination alpha when composited later.
				// RGB-only effects disappear outside the fill, while the caller's usual
				// non-separate SRCALPHA blend can reduce existing destination alpha. Use
				// source-over union for alpha only; RGB blend remains caller-controlled.
				const bool writeEffectAlpha = effectPass
					&& m_originalAlphaBlend && m_supportsSeparateAlphaBlend
					&& (m_originalColorWrite & D3DCOLORWRITEENABLE_ALPHA) != 0;
				const DWORD colorWrite = effectPass && !writeEffectAlpha
					? (m_originalColorWrite
						& ~static_cast<DWORD>(D3DCOLORWRITEENABLE_ALPHA))
					: m_originalColorWrite;
				const DWORD alphaTest = customCoverageShader
					? FALSE : (effectPass ? FALSE : m_originalAlphaTest);
				m_modified = true;
				const bool tileColorReady = SUCCEEDED(m_device->SetPixelShaderConstantF(
					0, passTileColor.data(), 1));
				bool alphaStateReady = true;
				if (m_supportsSeparateAlphaBlend)
				{
					alphaStateReady = SUCCEEDED(m_device->SetRenderState(
						D3DRS_SRCBLENDALPHA,
						writeEffectAlpha ? D3DBLEND_ONE : m_originalSourceAlphaBlend))
						&& SUCCEEDED(m_device->SetRenderState(D3DRS_DESTBLENDALPHA,
							writeEffectAlpha ? D3DBLEND_INVSRCALPHA
								: m_originalDestinationAlphaBlend))
						&& SUCCEEDED(m_device->SetRenderState(D3DRS_BLENDOPALPHA,
							writeEffectAlpha ? D3DBLENDOP_ADD
								: m_originalAlphaBlendOperation))
						&& SUCCEEDED(m_device->SetRenderState(
							D3DRS_SEPARATEALPHABLENDENABLE,
							writeEffectAlpha ? TRUE : m_originalSeparateAlphaBlend));
				}
				return tileColorReady && alphaStateReady
					&& SUCCEEDED(m_device->SetRenderState(D3DRS_ALPHATESTENABLE,
					alphaTest))
					&& SUCCEEDED(m_device->SetRenderState(D3DRS_ZWRITEENABLE,
						effectPass ? FALSE : m_originalZWrite))
					&& SUCCEEDED(m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
						colorWrite))
					&& SUCCEEDED(m_device->SetRenderState(D3DRS_STENCILWRITEMASK,
						effectPass ? 0 : m_originalStencilWriteMask));
			}

			bool SetSmoothSampling(bool enabled)
			{
				if (!m_valid)
					return false;
				const DWORD filter = enabled ? D3DTEXF_LINEAR : D3DTEXF_POINT;
				m_modified = true;
				return SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_MINFILTER,
					filter))
					&& SUCCEEDED(m_device->SetSamplerState(0, D3DSAMP_MAGFILTER,
						filter));
			}

		private:
			void VerifyRestoredRenderState(D3DRENDERSTATETYPE state,
				DWORD expected, const char* reason)
			{
				DWORD actual = 0;
				if (FAILED(m_device->GetRenderState(state, &actual)) || actual != expected)
					LogStateIsolationFailure(reason);
			}

			struct SamplerSetting
			{
				D3DSAMPLERSTATETYPE state;
				DWORD value = 0;
			};

			IDirect3DDevice9* m_device = nullptr;
			NiDX9RenderState* m_cachedRenderState = nullptr;
			IDirect3DPixelShader9* m_originalPixelShader = nullptr;
			std::array<float, 16> m_originalConstants = {};
			std::array<float, 4> m_originalTileColor = {};
			std::array<SamplerSetting, 5> m_samplerSettings = {{
				{ D3DSAMP_MINFILTER, 0 },
				{ D3DSAMP_MAGFILTER, 0 },
				{ D3DSAMP_MIPFILTER, 0 },
				{ D3DSAMP_ADDRESSU, 0 },
				{ D3DSAMP_ADDRESSV, 0 }
			}};
			UInt32 m_originalAlphaTest = FALSE;
			UInt32 m_originalAlphaBlend = FALSE;
			UInt32 m_originalZWrite = FALSE;
			UInt32 m_originalColorWrite = D3DCOLORWRITEENABLE_RED
				| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE
				| D3DCOLORWRITEENABLE_ALPHA;
			UInt32 m_originalSeparateAlphaBlend = FALSE;
			UInt32 m_originalSourceAlphaBlend = D3DBLEND_ONE;
			UInt32 m_originalDestinationAlphaBlend = D3DBLEND_ZERO;
			UInt32 m_originalAlphaBlendOperation = D3DBLENDOP_ADD;
			UInt32 m_originalStencilWriteMask = 0xFFFFFFFFu;
			bool m_supportsSeparateAlphaBlend = false;
			bool m_valid = false;
			bool m_modified = false;
		};

		HRESULT __stdcall A8DrawIndexedPrimitive(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex, UINT minimumVertexIndex,
			UINT numberOfVertices, UINT startIndex, UINT primitiveCount)
		{
			if (!s_originalDrawIndexedPrimitive)
				return D3DERR_INVALIDCALL;

			if (!s_a8RenderDepth)
			{
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
				{ ++trace.drawCalls; });
			A8RenderTraceContext* currentTrace = CurrentRenderTrace();
			const UInt32 drawCall = currentTrace ? currentTrace->drawCalls : 0;
			const bool detailedTrace = IsDetailedShadowTraceActive();
			A8ShapeMetadata metadata;
			bool haveMetadata = false;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				auto found = s_shapeMetadata.find(s_currentA8Shape);
				if (found != s_shapeMetadata.end())
				{
					metadata = found->second;
					haveMetadata = true;
				}
			}
			const bool useOriginalShader = haveMetadata
				&& metadata.effects.useOriginalShader;
			if (!useOriginalShader && !HaveA8Shader())
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=a8-shader-unavailable shape=%p depth=%u args=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u)",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						s_currentA8Shape, s_a8RenderDepth,
						static_cast<UInt32>(primitiveType), baseVertexIndex,
						minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
				}
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			LogA8DrawDiagnostics(device, primitiveType, baseVertexIndex,
				minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			const bool validContract = haveMetadata
				&& metadata.colorContract.abiVersion
					== A8ShapeColorContract::kTileUniformColorAbi;
			const bool geometryMatches = validContract
				&& primitiveType == D3DPT_TRIANGLELIST
				&& metadata.vertexCount == numberOfVertices
				&& metadata.primitiveCount == primitiveCount
				&& static_cast<UInt64>(metadata.indexCount)
					== static_cast<UInt64>(primitiveCount) * 3;
			if (detailedTrace)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: dip-begin serial=%llu dip=%u shape=%p depth=%u multipleDip=%u args=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u indices=%u) expected=(metadata=%u vertices=%u primitives=%u indices=%u ranges=%u geometryMatch=%u)",
					static_cast<unsigned long long>(currentTrace->serial), drawCall,
					s_currentA8Shape, s_a8RenderDepth, drawCall > 1 ? 1 : 0,
					static_cast<UInt32>(primitiveType), baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount,
					primitiveCount * 3, haveMetadata ? 1 : 0,
					metadata.vertexCount, metadata.primitiveCount, metadata.indexCount,
					static_cast<UInt32>(metadata.effects.ranges.size()),
					geometryMatches ? 1 : 0);
				LogShadowTraceDeviceState(device, "dip-entry", drawCall, -1,
					nullptr, D3D_OK);
			}
			if (!geometryMatches)
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (s_rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_a8_diag: draw contract mismatch; forwarding original draw metadata=%u abi=0x%08X type=%u vertices=%u/%u primitives=%u/%u indices=%llu/%u",
						haveMetadata ? 1 : 0, metadata.colorContract.abiVersion,
						static_cast<UInt32>(primitiveType), numberOfVertices,
						metadata.vertexCount, primitiveCount, metadata.primitiveCount,
						static_cast<unsigned long long>(
							static_cast<UInt64>(primitiveCount) * 3), metadata.indexCount);
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=draw-contract-mismatch metadata=%u abi=0x%08X type=%u vertices=%u/%u primitives=%u/%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						haveMetadata ? 1 : 0, metadata.colorContract.abiVersion,
						static_cast<UInt32>(primitiveType), numberOfVertices,
						metadata.vertexCount, primitiveCount, metadata.primitiveCount);
				}
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			ScopedA8RenderState state(device);
			if (!state.IsValid())
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=render-state-snapshot-failed",
						static_cast<unsigned long long>(currentTrace->serial), drawCall);
				}
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}
			const bool haveRanges = validContract && metadata.effects.enabled
				&& !metadata.effects.ranges.empty();
			bool useShaderEffects = !useOriginalShader && haveRanges
				&& metadata.effects.shaderEffects
				&& HaveEffectShader(metadata.effects.quality);
			IDirect3DPixelShader9* effectShader = useShaderEffects
				? s_effectShaders[static_cast<size_t>(metadata.effects.quality)]->GetShaderHandle()
				: nullptr;
			IDirect3DPixelShader9* fillShader = useOriginalShader
				? nullptr : s_a8Shader->GetShaderHandle();
			// Install the fill shader first. Optional effect shaders are selected
			// per range below, so enabling an effect can never replace the shader
			// responsible for the mandatory text body.
			if (!useOriginalShader && !state.Apply(fillShader))
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.fillFailures; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=fill-shader-apply-failed shader=%p",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						fillShader);
				}
				return D3DERR_INVALIDCALL;
			}

			if (!haveRanges)
			{
				const float constants[16] = {
					1.0f, 1.0f, 1.0f, 1.0f,
					0.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 0.0f
				};
				const HRESULT constantResult = useOriginalShader ? D3D_OK
					: device->SetPixelShaderConstantF(1, constants, 4);
				if (FAILED(constantResult))
				{
					UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
						{ ++trace.fillFailures; });
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=fill-constant-upload-failed hr=0x%08X",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							static_cast<UInt32>(constantResult));
					}
					return D3DERR_INVALIDCALL;
				}
				if (!state.SetPassState(false, !useOriginalShader)
					|| (!useOriginalShader
						&& !state.SetSmoothSampling(NeedsScaledFillSampling(s_currentA8Shape))))
				{
					UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
						{ ++trace.fillFailures; });
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=fill-pass-state-failed",
							static_cast<unsigned long long>(currentTrace->serial), drawCall);
					}
					return D3DERR_INVALIDCALL;
				}
				LogShadowTraceDeviceState(device, "fill-ready-no-ranges", drawCall,
					3, fillShader, D3D_OK);
				const HRESULT fillResult = s_originalDrawIndexedPrimitive(device, primitiveType,
					baseVertexIndex, minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
				UpdateCurrentRenderTraces([fillResult](A8RenderTraceContext& trace)
					{
						if (SUCCEEDED(fillResult))
							++trace.fillSuccesses;
						else
							++trace.fillFailures;
					});
				LogShadowTraceDeviceState(device, "fill-complete-no-ranges", drawCall,
					3, fillShader, fillResult);
				return fillResult;
			}

			HRESULT firstEffectFailure = D3D_OK;
			HRESULT firstFillFailure = D3D_OK;
			bool drewRange = false;
			bool drewFill = false;
			UInt32 rangeOrdinal = 0;
			auto recordRangeResult = [](UInt32 layer, HRESULT result)
			{
				UpdateCurrentRenderTraces([layer, result](A8RenderTraceContext& trace)
					{
						if (layer == 3)
						{
							if (SUCCEEDED(result))
								++trace.fillSuccesses;
							else
								++trace.fillFailures;
						}
						else if (SUCCEEDED(result))
							++trace.effectSuccesses;
						else
							++trace.effectFailures;
					});
			};
			for (const A8DrawRange& range : metadata.effects.ranges)
			{
				const UInt32 currentOrdinal = rangeOrdinal++;
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.rangeAttempts; });
				if (!range.primitiveCount || !range.vertexCount || range.layer > 3)
				{
					recordRangeResult(range.layer, D3DERR_INVALIDCALL);
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=empty-or-invalid-range firstVertex=%u vertices=%u start=%u primitives=%u",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer, range.firstVertex,
							range.vertexCount, range.startIndex, range.primitiveCount);
					}
					continue;
				}
				// A shader-effect batch cannot reconstruct its CPU masks after a live
				// shader loss. Preserve readable text by drawing only the fill ranges.
				if (metadata.effects.shaderEffects && !useShaderEffects && range.layer != 3)
				{
					recordRangeResult(range.layer, D3DERR_NOTAVAILABLE);
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=effect-shader-unavailable quality=%u",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer,
							static_cast<UInt32>(metadata.effects.quality));
					}
					continue;
				}

				const UInt64 rangeVertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 rangeIndexEnd = static_cast<UInt64>(range.startIndex)
					+ static_cast<UInt64>(range.primitiveCount) * 3;
				const UInt64 submittedIndexCount = static_cast<UInt64>(primitiveCount) * 3;
				if (rangeVertexEnd > numberOfVertices || rangeIndexEnd > submittedIndexCount)
				{
					const HRESULT rangeResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, rangeResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = rangeResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = rangeResult;
					if (g_bEnableFreeTypeFontRenderingLog
						&& s_rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_a8_diag: invalid range layer=%u firstVertex=%u vertexCount=%u vertices=%u rangeStart=%u rangePrimitives=%u primitives=%u",
							range.layer, range.firstVertex, range.vertexCount,
							numberOfVertices, range.startIndex, range.primitiveCount,
							primitiveCount);
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=range-out-of-bounds vertexEnd=%llu/%u indexEnd=%llu/%llu",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer,
							static_cast<unsigned long long>(rangeVertexEnd), numberOfVertices,
							static_cast<unsigned long long>(rangeIndexEnd),
							static_cast<unsigned long long>(submittedIndexCount));
					}
					continue;
				}

				float parameter0 = metadata.effects.shadowBlurPixels;
				float parameter1 = metadata.effects.shadowPower;
				float parameter2 = 0.0f;
				if (range.layer == 1)
				{
					parameter0 = metadata.effects.glowInnerPixels;
					parameter1 = metadata.effects.glowOuterPixels;
					parameter2 = metadata.effects.glowPower;
				}
				else if (range.layer == 2)
				{
					parameter0 = metadata.effects.outlineWidthPixels;
					parameter1 = metadata.effects.outlineSoftnessPixels;
					parameter2 = 0.0f;
				}
				else if (range.layer == 3)
				{
					parameter0 = 0.0f;
					parameter1 = 0.0f;
					parameter2 = 0.0f;
				}
				const float constants[16] = {
					range.colorModifier.r, range.colorModifier.g,
					range.colorModifier.b, range.colorModifier.a,
					metadata.effects.inverseAtlasWidth,
					metadata.effects.inverseAtlasHeight,
					static_cast<float>(range.layer), metadata.effects.sdfSpreadPixels,
					parameter0, parameter1, parameter2, 0.0f,
					range.usesSdf ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
				};
				if (!std::all_of(std::begin(constants), std::end(constants),
					[](float value) { return std::isfinite(value); }))
				{
					const HRESULT constantResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, constantResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = constantResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = constantResult;
					if (g_bEnableFreeTypeFontRenderingLog
						&& s_rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_a8_diag: rejected non-finite range constants layer=%u",
							range.layer);
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=non-finite-constants",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer);
					}
					continue;
				}

				IDirect3DPixelShader9* rangeShader = fillShader;
				if (range.layer != 3 && metadata.effects.shaderEffects)
				{
					if (!useShaderEffects)
					{
						recordRangeResult(range.layer, D3DERR_NOTAVAILABLE);
						if (detailedTrace)
						{
							FreeTypeFontDebugLog(
								"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=effect-disabled-after-prior-failure",
								static_cast<unsigned long long>(currentTrace->serial), drawCall,
								currentOrdinal, range.layer);
						}
						continue;
					}
					rangeShader = effectShader;
				}
				if (!useOriginalShader && !state.Apply(rangeShader))
				{
					const HRESULT shaderResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, shaderResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = shaderResult;
					}
					else
					{
						if (SUCCEEDED(firstEffectFailure))
							firstEffectFailure = shaderResult;
						useShaderEffects = false;
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=shader-apply-failed shader=%p",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer, rangeShader);
					}
					continue;
				}
				const HRESULT constantResult = useOriginalShader ? D3D_OK
					: device->SetPixelShaderConstantF(1, constants, 4);
				if (FAILED(constantResult))
				{
					recordRangeResult(range.layer, constantResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = constantResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = constantResult;
					if (g_bEnableFreeTypeFontRenderingLog
						&& s_rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_a8_diag: range constant upload failed hr=0x%08X layer=%u",
							static_cast<UInt32>(constantResult), range.layer);
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=constant-upload-failed hr=0x%08X",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer,
							static_cast<UInt32>(constantResult));
					}
					continue;
				}
				const bool needsSmoothSampling = range.layer == 1 || range.layer == 2
					|| (range.layer == 0
						&& metadata.effects.shadowBlurPixels > 0.001f)
					|| range.usesSdf
					|| (range.layer == 3
						&& NeedsScaledFillSampling(s_currentA8Shape));
				const bool passStateReady = state.SetPassState(range.layer != 3,
					!useOriginalShader)
					&& (useOriginalShader
						|| state.SetSmoothSampling(needsSmoothSampling));
				if (!passStateReady)
				{
					const HRESULT stateResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, stateResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = stateResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = stateResult;
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=pass-state-failed",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer);
					}
					continue;
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: range-ready serial=%llu dip=%u ordinal=%u layer=%u geometry=(firstVertex=%u vertices=%u start=%u primitives=%u absoluteStart=%u) color=(%.5g,%.5g,%.5g,%.5g) sdf=%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						currentOrdinal, range.layer, range.firstVertex, range.vertexCount,
						range.startIndex, range.primitiveCount,
						startIndex + range.startIndex, range.colorModifier.r,
						range.colorModifier.g, range.colorModifier.b,
						range.colorModifier.a, range.usesSdf ? 1 : 0);
					LogShadowTraceDeviceState(device, "range-ready", drawCall,
						static_cast<int>(range.layer), rangeShader, D3D_OK);
				}

				if (!useOriginalShader && g_bEnableFreeTypeFontRenderingLog
					&& range.layer == 0
					&& s_shadowContractLogCount++ < 8)
				{
					std::array<float, 4> actualTile = {};
					std::array<float, 4> actualLayer = {};
					DWORD actualSeparateAlpha = 0;
					DWORD actualSourceAlpha = 0;
					DWORD actualDestinationAlpha = 0;
					DWORD actualAlphaOperation = 0;
					DWORD actualColorWrite = 0;
					device->GetPixelShaderConstantF(0, actualTile.data(), 1);
					device->GetPixelShaderConstantF(1, actualLayer.data(), 1);
					device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
						&actualSeparateAlpha);
					device->GetRenderState(D3DRS_SRCBLENDALPHA, &actualSourceAlpha);
					device->GetRenderState(D3DRS_DESTBLENDALPHA,
						&actualDestinationAlpha);
					device->GetRenderState(D3DRS_BLENDOPALPHA, &actualAlphaOperation);
					device->GetRenderState(D3DRS_COLORWRITEENABLE, &actualColorWrite);
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag: shadow contract=tile-fill-effect-rgb-v7 c0=(%.4f,%.4f,%.4f,%.4f) c1=(%.4f,%.4f,%.4f,%.4f) coverage=atlas-sampled expectedMaxAlpha=%.4f blend=rgb-caller-alpha-source-over alphaState=(separate=%u src=%u dst=%u op=%u) colorWrite=0x%X alphaTest=disabled",
						actualTile[0], actualTile[1], actualTile[2], actualTile[3],
						actualLayer[0], actualLayer[1], actualLayer[2], actualLayer[3],
						actualTile[3] * actualLayer[3], actualSeparateAlpha,
						actualSourceAlpha, actualDestinationAlpha,
						actualAlphaOperation, actualColorWrite);
				}

				UInt32 samples = 1;
				const UInt32 quality = static_cast<UInt32>(metadata.effects.quality);
				const bool sdfPass = range.layer == 1 || range.layer == 2
					|| (range.layer == 0 && metadata.effects.shadowBlurPixels > 0.001f)
					|| range.usesSdf;
				if (useShaderEffects && sdfPass)
					samples = quality == 0 ? 1 : quality == 1 ? 4 : 8;
				if (useShaderEffects)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectPass);
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectSamples,
						static_cast<UInt64>(samples) * metadata.glyphCount);
				}
				// The engine may submit a shape from a packed vertex buffer where both
				// BaseVertexIndex and MinVertexIndex are non-zero. The range indices are
				// already relative to the original draw, so offsetting MinVertexIndex a
				// second time can make otherwise valid fill passes fail validation.
				// Keep the original full vertex window and restrict only the index range.
				const HRESULT passResult = s_originalDrawIndexedPrimitive(device,
					primitiveType, baseVertexIndex, minimumVertexIndex, numberOfVertices,
					startIndex + range.startIndex, range.primitiveCount);
				drewRange = true;
				recordRangeResult(range.layer, passResult);
				if (range.layer == 3)
				{
					drewFill = drewFill || SUCCEEDED(passResult);
					if (FAILED(passResult) && SUCCEEDED(firstFillFailure))
						firstFillFailure = passResult;
				}
				else if (FAILED(passResult) && SUCCEEDED(firstEffectFailure))
					firstEffectFailure = passResult;
				if (FAILED(passResult) && g_bEnableFreeTypeFontRenderingLog
					&& s_rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag: range draw failed hr=0x%08X layer=%u base=%d min=%u vertices=%u start=%u rangeStart=%u rangePrimitives=%u",
						static_cast<UInt32>(passResult), range.layer, baseVertexIndex,
						minimumVertexIndex, numberOfVertices, startIndex,
						range.startIndex, range.primitiveCount);
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: range-result serial=%llu dip=%u ordinal=%u layer=%u hr=0x%08X fill=%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						currentOrdinal, range.layer, static_cast<UInt32>(passResult),
						range.layer == 3 ? 1 : 0);
					LogShadowTraceDeviceState(device, "range-complete", drawCall,
						static_cast<int>(range.layer), rangeShader, passResult);
				}
			}

			// Effects are optional. A failed shadow/glow/outline pass must not report
			// the whole text draw as failed after its mandatory fill was rendered.
			if (detailedTrace)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: dip-end serial=%llu dip=%u drewRange=%u drewFill=%u firstEffectFailure=0x%08X firstFillFailure=0x%08X result=%s",
					static_cast<unsigned long long>(currentTrace->serial), drawCall,
					drewRange ? 1 : 0, drewFill ? 1 : 0,
					static_cast<UInt32>(firstEffectFailure),
					static_cast<UInt32>(firstFillFailure), drewFill ? "success"
						: FAILED(firstFillFailure) ? "fill-failure"
						: FAILED(firstEffectFailure) ? "effect-failure" : "no-range");
			}
			if (drewFill)
				return D3D_OK;
			if (FAILED(firstFillFailure))
				return firstFillFailure;
			if (FAILED(firstEffectFailure))
				return firstEffectFailure;
			return drewRange ? D3D_OK : D3DERR_INVALIDCALL;
		}

		bool IsA8AtlasShape(const NiTriShape* shape)
		{
			return shape && *reinterpret_cast<void* const* const*>(shape)
				== &s_a8TriShapeVtable[1];
		}

		void __fastcall A8RenderShape(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			NiTriShape* previousShape = s_currentA8Shape;
			if (a8)
			{
				BeginA8RenderTrace(shape, "renderer-shape");
				++s_a8RenderDepth;
				s_currentA8Shape = shape;
			}
			s_originalRenderShape(renderer, shape);
			if (a8)
			{
				s_currentA8Shape = previousShape;
				--s_a8RenderDepth;
				EndA8RenderTrace(shape, "renderer-shape");
			}
		}

		void __fastcall A8RenderShapeAlt(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			NiTriShape* previousShape = s_currentA8Shape;
			if (a8)
			{
				BeginA8RenderTrace(shape, "renderer-shape-alt");
				++s_a8RenderDepth;
				s_currentA8Shape = shape;
			}
			s_originalRenderShapeAlt(renderer, shape);
			if (a8)
			{
				s_currentA8Shape = previousShape;
				--s_a8RenderDepth;
				EndA8RenderTrace(shape, "renderer-shape-alt");
			}
		}

		void __fastcall A8BatchRenderShape(NiDX9Renderer* renderer, void*,
			NiTriShape* shape)
		{
			if (!s_originalBatchRenderShape)
				return;
			if (!renderer)
				return;
			if (!IsA8AtlasShape(shape)
				|| !s_originalBeginBatch || !s_originalEndBatch)
			{
				s_originalBatchRenderShape(renderer, shape);
				return;
			}
			if (g_bEnableFreeTypeFontRenderingLog && !s_loggedBatchRouteHit)
			{
				s_loggedBatchRouteHit = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: renderer batch route hit shape=%p depth=%u",
					shape, s_a8RenderDepth);
			}

			// NiDX9Renderer's batch path queues geometry and emits its D3D draw calls
			// later from Do_EndBatch. At that point there is no per-shape virtual call
			// left from which the range bridge can recover this shape's metadata. Isolate
			// a marked atlas shape in its own batch so the original renderer keeps the
			// caller's Tile properties and shader selection, while the D3D bridge retains
			// an unambiguous shape context for every draw in that batch.
			NiPropertyState* properties = renderer->m_pkBatchedPropertyState;
			NiDynamicEffectState* effects = renderer->m_pkBatchedEffectState;
			NiTriShape* previousShape = s_currentA8Shape;
			const UInt32 previousDepth = s_a8RenderDepth;
			// End even an apparently empty batch: AddToBatch may already have selected
			// m_spBatchedShader before rejecting all geometry. Reusing that cached shader
			// for this shape would recreate the same missing/incorrect-pass failure. Any
			// older queued geometry is not part of this shape, so do not let an inherited
			// immediate-render scope misclassify its draw calls as FreeType ranges.
			s_a8RenderDepth = 0;
			s_currentA8Shape = nullptr;
			s_originalEndBatch(renderer);
			s_a8RenderDepth = previousDepth;
			s_currentA8Shape = previousShape;
			s_originalBeginBatch(renderer, properties, effects);

			BeginA8RenderTrace(shape, "renderer-batch-shape");
			++s_a8RenderDepth;
			s_currentA8Shape = shape;
			// Keep the scope active while submitting as well as flushing. The stock
			// function only queues, but this also remains correct if another renderer
			// extension has wrapped the slot and chooses to issue an immediate draw.
			s_originalBatchRenderShape(renderer, shape);
			s_originalEndBatch(renderer);
			s_currentA8Shape = previousShape;
			--s_a8RenderDepth;
			EndA8RenderTrace(shape, "renderer-batch-shape");

			// The caller still owns the surrounding batch and will submit more objects.
			s_originalBeginBatch(renderer, properties, effects);
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
			s_originalBeginBatch = reinterpret_cast<BeginBatchFn>(
				vtable[kRendererBeginBatchSlot]);
			s_originalEndBatch = reinterpret_cast<EndBatchFn>(
				vtable[kRendererEndBatchSlot]);

			void* batchEntry = vtable[kRendererBatchRenderShapeSlot];
			void* renderEntry = vtable[kRendererRenderShapeSlot];
			void* renderAltEntry = vtable[kRendererRenderShapeAltSlot];
			void* batchHook = reinterpret_cast<void*>(&A8BatchRenderShape);
			void* renderHook = reinterpret_cast<void*>(&A8RenderShape);
			void* renderAltHook = reinterpret_cast<void*>(&A8RenderShapeAlt);

			if (!s_originalBatchRenderShape && batchEntry != batchHook)
				s_originalBatchRenderShape = reinterpret_cast<BatchRenderShapeFn>(batchEntry);
			if (!s_originalRenderShape && renderEntry != renderHook)
				s_originalRenderShape = reinterpret_cast<RenderShapeFn>(renderEntry);
			if (!s_originalRenderShapeAlt && renderAltEntry != renderAltHook)
				s_originalRenderShapeAlt = reinterpret_cast<RenderShapeFn>(renderAltEntry);
			const bool entriesValid = s_originalBatchRenderShape
				&& s_originalRenderShape && s_originalRenderShapeAlt
				&& (batchEntry == batchHook || batchEntry
					== reinterpret_cast<void*>(s_originalBatchRenderShape))
				&& (renderEntry == renderHook || renderEntry
					== reinterpret_cast<void*>(s_originalRenderShape))
				&& (renderAltEntry == renderAltHook || renderAltEntry
					== reinterpret_cast<void*>(s_originalRenderShapeAlt));
			if (!entriesValid)
			{
				if (!s_loggedRendererHookConflict)
				{
					s_loggedRendererHookConflict = true;
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
							reinterpret_cast<void*>(s_originalBatchRenderShape));
					return false;
				}
				wroteRender = true;
			}
			if (renderAltEntry != renderAltHook
				&& !WriteVtableEntry(vtable, kRendererRenderShapeAltSlot, renderAltHook))
			{
				if (wroteRender)
					WriteVtableEntry(vtable, kRendererRenderShapeSlot,
						reinterpret_cast<void*>(s_originalRenderShape));
				if (wroteBatch)
					WriteVtableEntry(vtable, kRendererBatchRenderShapeSlot,
						reinterpret_cast<void*>(s_originalBatchRenderShape));
				return false;
			}
			return s_originalBeginBatch && s_originalEndBatch
				&& s_originalBatchRenderShape && s_originalRenderShape
				&& s_originalRenderShapeAlt;
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
				s_hookedDevice = device;
				const bool rendererRoutesAvailable = HookRendererShapeEntries(renderer);
				return s_originalDrawIndexedPrimitive
					&& (rendererRoutesAvailable || tileRouteAvailable);
			}
			if (s_hookedDevice && s_hookedDevice == device)
			{
				if (!s_loggedHookConflict)
				{
					s_loggedHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: D3D9 draw bridge was replaced; new atlases use 32-bit fallback");
				}
				return false;
			}

			s_originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(
				vtable[kDrawIndexedPrimitiveSlot]);
			if (!WriteVtableEntry(vtable, kDrawIndexedPrimitiveSlot,
				reinterpret_cast<void*>(&A8DrawIndexedPrimitive)))
			{
				return false;
			}
			s_hookedDevice = device;
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

		void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			s_rangeBridgeAvailable = HookD3DDevice();
			NiTriShape* previousShape = s_currentA8Shape;
			BeginA8RenderTrace(shape, "shape-immediate");
			++s_a8RenderDepth;
			s_currentA8Shape = shape;
			s_originalRenderImmediate(shape, renderer);
			s_currentA8Shape = previousShape;
			--s_a8RenderDepth;
			EndA8RenderTrace(shape, "shape-immediate");
		}

		void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			s_rangeBridgeAvailable = HookD3DDevice();
			NiTriShape* previousShape = s_currentA8Shape;
			BeginA8RenderTrace(shape, "shape-immediate-alt");
			++s_a8RenderDepth;
			s_currentA8Shape = shape;
			s_originalRenderImmediateAlt(shape, renderer);
			s_currentA8Shape = previousShape;
			--s_a8RenderDepth;
			EndA8RenderTrace(shape, "shape-immediate-alt");
		}

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				s_shapeMetadata.erase(shape);
				s_loggedShapes.erase(shape);
				s_tracedShadowShapes.erase(shape);
				s_tracedShadowShapeOrder.erase(std::remove(
					s_tracedShadowShapeOrder.begin(), s_tracedShadowShapeOrder.end(),
					shape), s_tracedShadowShapeOrder.end());
			}
			s_originalDeleteThis(shape);
		}

		bool InitializeA8TriShapeVtable(NiTriShape* shape)
		{
			void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
			if (!source)
				return false;
			if (source == &s_a8TriShapeVtable[1])
				return true;
			if (s_originalTriShapeVtable)
				return source == s_originalTriShapeVtable;

			s_originalTriShapeVtable = source;
			s_a8TriShapeVtable[0] = source[-1];
			std::copy(source, source + kCopiedTriShapeVtableEntries,
				s_a8TriShapeVtable.begin() + 1);
			s_originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
				s_a8TriShapeVtable[kRenderImmediateSlot + 1]);
			s_originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
				s_a8TriShapeVtable[kRenderImmediateAltSlot + 1]);
			s_originalDeleteThis = reinterpret_cast<DeleteThisFn>(
				s_a8TriShapeVtable[kDeleteThisSlot + 1]);
			s_a8TriShapeVtable[kDeleteThisSlot + 1]
				= reinterpret_cast<void*>(&A8DeleteThis);
			s_a8TriShapeVtable[kRenderImmediateSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediate);
			s_a8TriShapeVtable[kRenderImmediateAltSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediateAlt);
			return s_originalRenderImmediate && s_originalRenderImmediateAlt
				&& s_originalDeleteThis;
		}

		bool IsFiniteColor(const NiColorA& color)
		{
			return std::isfinite(color.r) && std::isfinite(color.g)
				&& std::isfinite(color.b) && std::isfinite(color.a);
		}

		bool RejectA8Shape(const char* reason)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& s_shapeValidationFailureLogCount++
					< kMaximumShapeValidationFailureLogs)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag: rejected shape contract=tile-fill-effect-rgb-v7 reason=%s",
					reason ? reason : "unknown");
			}
			return false;
		}

		bool ValidateA8Shape(NiTriShape* shape,
			const A8EffectShapeConfig* effectConfig,
			const A8ShapeColorContract* colorContract)
		{
			if (!shape || !colorContract)
				return RejectA8Shape("missing-shape-or-color-contract");
			if (colorContract->abiVersion
				!= A8ShapeColorContract::kTileUniformColorAbi)
			{
				return RejectA8Shape("color-contract-abi-mismatch");
			}
			if (!IsFiniteColor(colorContract->minimumModifier)
				|| !IsFiniteColor(colorContract->maximumModifier))
			{
				return RejectA8Shape("non-finite-color-contract");
			}

			NiTriShapeData* data = shape->GetModelData();
			if (!data || !data->m_usVertices || !data->m_usTriangles
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				return RejectA8Shape("missing-geometry-data");
			}
			// The custom shader follows the original TILE1000 uniform-color path.
			// A COLOR0 stream would select a different original Tile contract.
			if (data->m_pkColor)
				return RejectA8Shape("unexpected-vertex-color-stream");
			if (!effectConfig || !effectConfig->enabled)
				return true;

			const std::array<float, 9> scalarValues = {
				effectConfig->inverseAtlasWidth,
				effectConfig->inverseAtlasHeight,
				effectConfig->sdfSpreadPixels,
				effectConfig->shadowBlurPixels,
				effectConfig->shadowPower,
				effectConfig->glowInnerPixels,
				effectConfig->glowOuterPixels,
				effectConfig->glowPower,
				effectConfig->outlineWidthPixels
			};
			if (!std::all_of(scalarValues.begin(), scalarValues.end(),
				[](float value) { return std::isfinite(value); })
				|| !std::isfinite(effectConfig->outlineSoftnessPixels))
			{
				return RejectA8Shape("non-finite-effect-configuration");
			}
			if (static_cast<UInt32>(effectConfig->quality)
				> static_cast<UInt32>(EffectQuality::High))
			{
				return RejectA8Shape("invalid-effect-quality");
			}

			const UInt64 triangleIndices = static_cast<UInt64>(data->m_usTriangles) * 3;
			const UInt64 availableIndices = data->m_uiTriListLength
				? std::min<UInt64>(triangleIndices, data->m_uiTriListLength)
				: triangleIndices;
			UInt64 previousVertexEnd = 0;
			UInt64 previousIndexEnd = 0;
			UInt32 previousLayer = 0;
			bool firstRange = true;
			bool haveFill = false;
			for (const A8DrawRange& range : effectConfig->ranges)
			{
				if (range.layer > 3 || !range.vertexCount || !range.primitiveCount
					|| !IsFiniteColor(range.colorModifier))
				{
					return RejectA8Shape("invalid-draw-range");
				}
				const UInt64 vertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 indexEnd = static_cast<UInt64>(range.startIndex)
					+ static_cast<UInt64>(range.primitiveCount) * 3;
				if (vertexEnd > data->m_usVertices || indexEnd > availableIndices)
					return RejectA8Shape("draw-range-out-of-bounds");
				if (!firstRange && (range.layer < previousLayer
					|| range.firstVertex < previousVertexEnd
					|| range.startIndex < previousIndexEnd))
				{
					return RejectA8Shape("draw-ranges-not-global-and-monotonic");
				}
				for (UInt64 index = range.startIndex; index < indexEnd; ++index)
				{
					if (data->m_pusTriList[index] >= data->m_usVertices)
						return RejectA8Shape("triangle-index-out-of-bounds");
				}
				if (range.usesSdf && effectConfig->sdfSpreadPixels <= 0.0f)
					return RejectA8Shape("sdf-range-without-positive-spread");
				haveFill = haveFill || range.layer == 3;
				previousLayer = range.layer;
				previousVertexEnd = vertexEnd;
				previousIndexEnd = indexEnd;
				firstRange = false;
			}
			return haveFill || RejectA8Shape("missing-fill-range");
		}

		bool TryInitializeA8Renderer(bool forceShaderAttempt, bool reportFailures)
		{
			if (!g_bEnableFreeTypeFontRendering)
			{
				s_a8Available = false;
				s_rangeBridgeAvailable = false;
				return false;
			}

			// The original-shader ARGB path needs only the Tile/D3D range bridge;
			// shader-loader discovery is an independent, opportunistic upgrade.
			HookTileRenderPass();
			s_rangeBridgeAvailable = HookD3DDevice();
			if (!g_bEnableFreeTypeA8Atlas)
			{
				s_a8Available = false;
				return false;
			}
			const bool baseShaderReady = HaveA8Shader();
			s_a8Available = baseShaderReady && s_rangeBridgeAvailable;
			if (baseShaderReady && HaveAllEffectShaders())
			{
				return s_a8Available;
			}
			if (s_initializationInProgress)
				return s_a8Available;

			const DWORD now = GetTickCount();
			if (!forceShaderAttempt && s_initializationAttempted
				&& now - s_lastInitializationAttemptTick < 1000)
			{
				return s_a8Available;
			}
			s_initializationAttempted = true;
			s_lastInitializationAttemptTick = now;
			s_initializationInProgress = true;
			bool loaded = false;
			do
			{
				if (!g_cmdTableInterface
					|| !g_cmdTableInterface->GetPluginInfoByDLLName)
				{
					break;
				}
				const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
					"Fallout Shader Loader.dll");
				if (!info || info->infoVersion != PluginInfo::kInfoVersion
					|| info->version < dependencies::kShaderLoaderMinVersion)
				{
					break;
				}
				HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
				const auto createPixelShader = module
					? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(
						module, "CreatePixelShader")) : nullptr;
				if (!createPixelShader)
					break;
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				if (!renderer
					|| renderer->m_kD3DCaps9.PixelShaderVersion < D3DPS_VERSION(3, 0))
				{
					break;
				}
				s_shaderLoaderCompatible = true;
				loaded = baseShaderReady || LoadA8Shader(createPixelShader);
				if (loaded)
					LoadEffectShaders(createPixelShader);
			} while (false);
			s_initializationInProgress = false;
			s_a8Available = HaveA8Shader() && s_rangeBridgeAvailable;

			if (!HaveA8Shader() && reportFailures
				&& !s_loggedShaderLoaderUnavailable)
			{
				s_loggedShaderLoaderUnavailable = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: Shader Loader/PS3 font shader is not ready; retaining range-safe 32-bit atlases and retrying later");
			}
			return s_a8Available;
		}
	}

	void FinalizeA8RendererDetection()
	{
		TryInitializeA8Renderer(true, true);
	}

	void HandleA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		TryInitializeA8Renderer(false, false);
	}

	void HandleA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage)
			return;
		TryInitializeA8Renderer(true, false);
		if (!s_shaderLoaderCompatible)
			return;
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (RefreshShaderSet(createPixelShader))
		{
			s_rangeBridgeAvailable = HookD3DDevice();
			s_a8Available = s_rangeBridgeAvailable;
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: shader refresh validation failed; retaining the previous shader set");
		}
	}

	bool IsA8RendererAvailable()
	{
		TryInitializeA8Renderer(false, false);
		return s_a8Available && HaveA8Shader()
			&& IsAtlasRangeRendererAvailable();
	}

	bool IsAtlasRangeRendererAvailable()
	{
		HookTileRenderPass();
		s_rangeBridgeAvailable = HookD3DDevice();
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		return s_rangeBridgeAvailable && s_originalDrawIndexedPrimitive
			&& renderer && renderer->GetD3DDevice();
	}

	bool ResolveA8EffectQuality(EffectQuality requested, EffectQuality& resolved)
	{
		for (int quality = static_cast<int>(requested); quality >= 0; --quality)
		{
			const EffectQuality candidate = static_cast<EffectQuality>(quality);
			if (HaveEffectShader(candidate))
			{
				resolved = candidate;
				return true;
			}
		}
		return false;
	}

	bool IsA8EffectRendererAvailable(EffectQuality quality)
	{
		EffectQuality resolved = quality;
		return IsA8RendererAvailable() && ResolveA8EffectQuality(quality, resolved);
	}

	bool PrepareA8AtlasShape(NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount, const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract)
	{
		const bool useOriginalShader = effectConfig
			&& effectConfig->useOriginalShader;
		const bool rangeBridgeReady = useOriginalShader
			? IsAtlasRangeRendererAvailable() : false;
		const bool rendererAvailable = useOriginalShader
			? (rangeBridgeReady || HookTileRenderPass()) : IsA8RendererAvailable();
		if (!rendererAvailable
			|| !ValidateA8Shape(shape, effectConfig, colorContract)
			|| !InitializeA8TriShapeVtable(shape))
			return false;
		{
			std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
			A8ShapeMetadata metadata;
			metadata.fontId = fontId;
			metadata.glyphCount = glyphCount;
			metadata.quadCount = quadCount;
			if (NiTriShapeData* data = shape->GetModelData())
			{
				metadata.vertexCount = data->m_usVertices;
				metadata.primitiveCount = data->m_usTriangles;
				metadata.indexCount = data->m_uiTriListLength
					? data->m_uiTriListLength : data->m_usTriangles * 3;
			}
			if (colorContract)
				metadata.colorContract = *colorContract;
			if (effectConfig)
				metadata.effects = *effectConfig;
			s_shapeMetadata[shape] = metadata;
		}
		// Publish the marker vtable only after its metadata is complete. The draw
		// bridge must never observe a custom shape with a stale/default contract.
		*reinterpret_cast<void***>(shape) = &s_a8TriShapeVtable[1];
		if (useOriginalShader && !rangeBridgeReady
			&& g_bEnableFreeTypeFontRenderingLog && !s_loggedPendingRangeShape)
		{
			s_loggedPendingRangeShape = true;
			gLog.FormattedMessage(
				"tnvse_freetype_font: retained startup effect shape pending first-render D3D range bridge shape=%p font=%u",
				shape, fontId);
		}
		return true;
	}
}

namespace fonthook
{
	void FinalizeFreeTypeA8Detection()
	{
		vectorfont::FinalizeA8RendererDetection();
	}

	void HandleFreeTypeA8MainLoop()
	{
		vectorfont::HandleA8RendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::HandleA8ShaderLoaderMessage(messageType);
	}
}
