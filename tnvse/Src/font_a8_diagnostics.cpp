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
		A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape)
		{
			if (!shape)
				return {};
			std::lock_guard<std::mutex> lock(State().diagnosticsMutex);
			const auto found = State().shapeMetadata.find(shape);
			return found != State().shapeMetadata.end() ? found->second : A8ShapeMetadataPtr{};
		}

		A8ShapeMetadataPtr ResolveRenderMetadata(const NiTriShape* shape)
		{
			if (shape && shape == ThreadState().currentShape && ThreadState().currentMetadata)
				return ThreadState().currentMetadata;
			return FindA8ShapeMetadata(shape);
		}

		bool HasShadowRange(const A8ShapeMetadata& metadata)
		{
			return metadata.hasShadowRange;
		}

		A8RenderTraceContext* CurrentRenderTrace()
		{
			return ThreadState().renderTraceDepth
				? &ThreadState().renderTraceStack[ThreadState().renderTraceDepth - 1] : nullptr;
		}

		bool IsDetailedShadowTraceActive()
		{
			for (UInt32 index = 0; index < ThreadState().renderTraceDepth; ++index)
			{
				const A8RenderTraceContext& trace = ThreadState().renderTraceStack[index];
				if (trace.detailed && trace.shape == ThreadState().currentShape)
					return true;
			}
			return false;
		}


		void BeginA8RenderTrace(NiTriShape* shape, const char* entryPoint,
			const A8ShapeMetadataPtr& metadata)
		{
			// Trace bookkeeping is diagnostic-only. Keep the entire render hot path
			// free of serial increments and the diagnostics mutex when logging is off.
			if (!g_bEnableFreeTypeFontRenderingLog
				|| ThreadState().renderTraceDepth >= kMaximumRenderTraceDepth)
				return;

			A8RenderTraceContext& trace = ThreadState().renderTraceStack[ThreadState().renderTraceDepth++];
			trace = {};
			trace.serial = ++State().shadowTraceSerial;
			trace.shape = shape;
			trace.entryPoint = entryPoint;

			const bool haveMetadata = static_cast<bool>(metadata);
			{
				std::lock_guard<std::mutex> lock(State().diagnosticsMutex);
				bool inherited = false;
				if (ThreadState().renderTraceDepth > 1)
				{
					const A8RenderTraceContext& parent =
						ThreadState().renderTraceStack[ThreadState().renderTraceDepth - 2];
					inherited = parent.detailed && parent.shape == shape;
				}
				const bool newlyTraced = haveMetadata && HasShadowRange(*metadata)
					&& State().tracedShadowShapes.insert(shape).second;
				trace.detailed = inherited || newlyTraced;
				if (newlyTraced)
				{
					State().tracedShadowShapeOrder.push_back(shape);
					while (State().tracedShadowShapeOrder.size() > kMaximumShadowTraceShapes)
					{
						const NiTriShape* oldest = State().tracedShadowShapeOrder.front();
						State().tracedShadowShapeOrder.pop_front();
						State().tracedShadowShapes.erase(oldest);
					}
				}
			}

			if (trace.detailed)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: begin serial=%llu scopeDepth=%u entry=%s shape=%p parent=%p metadata=%u font=%u glyphs=%u quads=%u expected=(vertices=%u primitives=%u indices=%u ranges=%u)",
					static_cast<unsigned long long>(trace.serial), ThreadState().renderTraceDepth,
					entryPoint ? entryPoint : "unknown", shape,
					shape ? shape->m_pkParent : nullptr, haveMetadata ? 1 : 0,
					metadata ? metadata->fontId : 0,
					metadata ? metadata->glyphCount : 0,
					metadata ? metadata->quadCount : 0,
					metadata ? metadata->vertexCount : 0,
					metadata ? metadata->primitiveCount : 0,
					metadata ? metadata->indexCount : 0,
					metadata ? static_cast<UInt32>(metadata->effects.ranges.size()) : 0);
			}
		}

		void EndA8RenderTrace(NiTriShape* shape, const char* entryPoint)
		{
			if (!ThreadState().renderTraceDepth)
				return;
			A8RenderTraceContext trace = ThreadState().renderTraceStack[ThreadState().renderTraceDepth - 1];
			--ThreadState().renderTraceDepth;
			if (!trace.detailed)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_shadow_trace: end serial=%llu scopeDepth=%u entry=%s shape=%p expectedShape=%p draws=%u forwarded=%u rangeAttempts=%u effect=(ok=%u fail=%u) fill=(ok=%u fail=%u) verdict=%s",
				static_cast<unsigned long long>(trace.serial), ThreadState().renderTraceDepth + 1,
				entryPoint ? entryPoint : "unknown", shape, trace.shape,
				trace.drawCalls, trace.forwardedCalls, trace.rangeAttempts,
				trace.effectSuccesses, trace.effectFailures,
				trace.fillSuccesses, trace.fillFailures,
				trace.fillSuccesses ? "fill-submitted"
					: trace.forwardedCalls ? "original-draw-forwarded"
					: trace.drawCalls ? "fill-missing-or-failed" : "no-dip-observed");
		}


		void LogShadowTraceDeviceState(IDirect3DDevice9* device, const char* stage,
			UInt32 drawCall, int layer, IDirect3DPixelShader9* expectedShader,
			HRESULT result)
		{
			A8RenderTraceContext* trace = CurrentRenderTrace();
			if (!device || !trace || !trace->detailed
				|| trace->shape != ThreadState().currentShape)
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
			DWORD maximumMipLevel = 0;
			DWORD mipLodBias = 0;
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
			device->GetSamplerState(0, D3DSAMP_MAXMIPLEVEL, &maximumMipLevel);
			device->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &mipLodBias);

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
				"tnvse_freetype_shadow_trace:   renderState blend=(enable=%u rgb=%u/%u op=%u separate=%u alpha=%u/%u op=%u) alphaTest=(enable=%u func=%u ref=%u) depth=(enable=%u write=%u) colorWrite=0x%X scissor=%u stencil=(enable=%u writeMask=0x%X) sampler=(min=%u mag=%u mip=%u maxMip=%u lodBiasBits=0x%08X)",
				alphaBlend, sourceBlend, destinationBlend, blendOperation,
				separateAlpha, sourceAlphaBlend, destinationAlphaBlend,
				alphaBlendOperation, alphaTest, alphaFunction, alphaReference,
				zEnable, zWrite, colorWrite, scissorEnable, stencilEnable,
				stencilWriteMask, minFilter, magFilter, mipFilter,
				maximumMipLevel, mipLodBias);

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
			UINT startIndex, UINT primitiveCount,
			const A8ShapeMetadata& metadata)
		{
			if (!g_bEnableFreeTypeFontRenderingLog || !device || !ThreadState().currentShape)
				return;

			{
				std::lock_guard<std::mutex> lock(State().diagnosticsMutex);
				if (State().diagnosticLogCount >= kMaximumDiagnosticShapes
					|| !State().loggedShapes.insert(ThreadState().currentShape).second)
				{
					return;
				}
				++State().diagnosticLogCount;
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
			DWORD maximumMipLevel = 0, mipLodBias = 0;
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
			device->GetSamplerState(0, D3DSAMP_MAXMIPLEVEL, &maximumMipLevel);
			device->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &mipLodBias);

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
			if (NiTriShapeData* data = ThreadState().currentShape->GetModelData())
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
			if (NiAlphaProperty* property = ThreadState().currentShape->GetAlphaProperty())
			{
				alphaFlags = property->m_usFlags.Get();
				propertyAlphaReference = property->m_ucAlphaTestRef;
			}
			NiColorA shadeColor = { -1.0f, -1.0f, -1.0f, -1.0f };
			if (NiShadeProperty* shade = ThreadState().currentShape->GetShadeProperty())
				shadeColor = *reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68);

			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag: shape=%p parent=%p font=%u glyphs=%u quads=%u vertices=%u triangles=%u draw=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u)",
				ThreadState().currentShape, ThreadState().currentShape->m_pkParent, metadata.fontId,
				metadata.glyphCount, metadata.quadCount, vertexCount, triangleCount,
				static_cast<UInt32>(primitiveType), baseVertexIndex,
				minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   local=(%.3f,%.3f,%.3f scale=%.3f) world=(%.3f,%.3f,%.3f scale=%.3f) shadeColor=(%.4f,%.4f,%.4f,%.4f) alphaProperty=(flags=0x%04X ref=%u)",
				ThreadState().currentShape->m_kLocal.m_Translate.x, ThreadState().currentShape->m_kLocal.m_Translate.y,
				ThreadState().currentShape->m_kLocal.m_Translate.z, ThreadState().currentShape->m_kLocal.m_fScale,
				ThreadState().currentShape->m_kWorld.m_Translate.x, ThreadState().currentShape->m_kWorld.m_Translate.y,
				ThreadState().currentShape->m_kWorld.m_Translate.z, ThreadState().currentShape->m_kWorld.m_fScale,
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
				"tnvse_freetype_a8_diag:   d3d ps=%p cachedPs=%p cacheMatch=%u vs=%p tex0=%p blend=(enable=%u rgb=%u/%u op=%u separate=%u alpha=%u/%u op=%u) alphaTest=(enable=%u func=%u ref=%u) zWrite=%u colorWrite=0x%X textureFactor=0x%08X sampler=(min=%u mag=%u mip=%u maxMip=%u lodBiasBits=0x%08X)",
				pixelShader, cachedPixelShader, pixelShader == cachedPixelShader,
				vertexShader, texture, alphaBlend, sourceBlend, destinationBlend,
				blendOperation, separateAlpha, sourceBlendAlpha, destinationBlendAlpha,
				blendOperationAlpha, alphaTest, alphaFunction, alphaReference,
				zWrite, colorWrite, textureFactor, minFilter, magFilter, mipFilter,
				maximumMipLevel, mipLodBias);
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

}
