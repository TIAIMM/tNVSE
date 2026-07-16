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
	namespace
	{
		A8State s_a8State;
		thread_local A8ThreadState s_a8ThreadState;
	}

	A8State& State()
	{
		return s_a8State;
	}

	A8ThreadState& ThreadState()
	{
		return s_a8ThreadState;
	}

		bool HaveA8Shader()
		{
			return State().a8Shader && State().a8Shader->GetShaderHandle();
		}

		bool HaveEffectShader(EffectQuality quality)
		{
			const size_t index = static_cast<size_t>(quality);
			return index < State().effectShaders.size() && State().effectShaders[index]
				&& State().effectShaders[index]->GetShaderHandle();
		}

		bool HaveAllEffectShaders()
		{
			for (size_t index = 0; index < State().effectShaders.size(); ++index)
			{
				if (!HaveEffectShader(static_cast<EffectQuality>(index)))
					return false;
			}
			return true;
		}

		void PublishCoverageShader(const NiD3DPixelShaderPtr& loaded)
		{
			if (loaded && loaded->GetShaderHandle())
			{
				State().coverageShader = loaded;
				State().loggedCoverageShaderLoadFailure = false;
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: loaded coverage-only A8 font shader");
				}
				return;
			}

			State().coverageShader = nullptr;
			if (!State().loggedCoverageShaderLoadFailure)
			{
				State().loggedCoverageShaderLoadFailure = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: failed to load tnvse_freetype_coverage.pso; using the generic A8 shader");
			}
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
				if (!State().loggedA8ShaderLoadFailure)
				{
					State().loggedA8ShaderLoadFailure = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: Shader Loader failed to load tnvse_freetype_a8.pso; using 32-bit atlases and retrying");
				}
				return false;
			}
			State().loggedA8ShaderLoadFailure = false;
			State().a8Shader = loaded;
			PublishCoverageShader(createPixelShader("tnvse_freetype_coverage.pso"));
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
			for (size_t index = 0; index < State().effectShaders.size(); ++index)
			{
				NiD3DPixelShaderPtr loaded = createPixelShader(names[index]);
				if (loaded && loaded->GetShaderHandle())
				{
					State().effectShaders[index] = loaded;
					State().loggedEffectShaderLoadFailure[index] = false;
					if (g_bEnableFreeTypeFontRenderingLog)
						FreeTypeFontDebugLog("tnvse_freetype_font: loaded %s", names[index]);
				}
				else if (!State().effectShaders[index]
					&& !State().loggedEffectShaderLoadFailure[index])
				{
					State().loggedEffectShaderLoadFailure[index] = true;
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
			NiD3DPixelShaderPtr coverage = createPixelShader(
				"tnvse_freetype_coverage.pso");
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
			State().a8Shader = base;
			State().effectShaders = effects;
			PublishCoverageShader(coverage);
			if (g_bEnableFreeTypeFontRenderingLog)
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: atomically refreshed Tile-compatible shader set contract=tile-fill-effect-rgb-v7");
			return true;
		}


		bool IsFiniteColor(const NiColorA& color)
		{
			return std::isfinite(color.r) && std::isfinite(color.g)
				&& std::isfinite(color.b) && std::isfinite(color.a);
		}

		bool RejectA8Shape(const char* reason)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& State().shapeValidationFailureLogCount++
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
			if (effectConfig->atlasTextures.size()
				!= effectConfig->atlasInverseSizes.size())
				return RejectA8Shape("atlas-page-metadata-size-mismatch");
			if (!effectConfig->atlasProperties.empty()
				&& effectConfig->atlasProperties.size()
					!= effectConfig->atlasTextures.size())
				return RejectA8Shape("atlas-page-property-size-mismatch");
			for (const NiPoint2& inverseSize : effectConfig->atlasInverseSizes)
			{
				if (!std::isfinite(inverseSize.x) || !std::isfinite(inverseSize.y))
					return RejectA8Shape("non-finite-atlas-inverse-size");
			}
			for (const A8DrawRange& range : effectConfig->ranges)
			{
				if (range.layer > 3 || !range.vertexCount || !range.primitiveCount
					|| !IsFiniteColor(range.colorModifier))
				{
					return RejectA8Shape("invalid-draw-range");
				}
				if (!effectConfig->atlasTextures.empty()
					&& (range.atlasPage >= effectConfig->atlasTextures.size()
						|| !effectConfig->atlasTextures[range.atlasPage]))
					return RejectA8Shape("invalid-atlas-page");
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

		void CompileA8DrawRanges(A8ShapeMetadata& metadata)
		{
			metadata.compiledRanges.clear();
			metadata.compiledRanges.reserve(metadata.effects.ranges.size());
			metadata.firstRangeShaderClass = A8CompiledShaderClass::Original;
			metadata.firstFillShaderClass = A8CompiledShaderClass::Original;
			metadata.hasShadowRange = false;
			bool haveFirstRange = false;
			bool haveFirstFill = false;
			for (const A8DrawRange& range : metadata.effects.ranges)
			{
				metadata.hasShadowRange = metadata.hasShadowRange
					|| (range.layer == 0 && range.vertexCount && range.primitiveCount);
				float inverseAtlasWidth = metadata.effects.inverseAtlasWidth;
				float inverseAtlasHeight = metadata.effects.inverseAtlasHeight;
				if (!metadata.effects.atlasInverseSizes.empty())
				{
					const NiPoint2& inverseSize =
						metadata.effects.atlasInverseSizes[range.atlasPage];
					inverseAtlasWidth = inverseSize.x;
					inverseAtlasHeight = inverseSize.y;
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
				}
				else if (range.layer == 3)
				{
					parameter0 = 0.0f;
					parameter1 = 0.0f;
				}

				A8CompiledRange compiled;
				compiled.range = range;
				compiled.constants = {{
					range.colorModifier.r, range.colorModifier.g,
					range.colorModifier.b, range.colorModifier.a,
					inverseAtlasWidth, inverseAtlasHeight,
					static_cast<float>(range.layer), metadata.effects.sdfSpreadPixels,
					parameter0, parameter1, parameter2, 0.0f,
					range.usesSdf ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
				}};
				if (metadata.effects.useOriginalShader)
				{
					compiled.shaderClass = A8CompiledShaderClass::Original;
				}
				else if (!range.usesSdf)
				{
					// Coverage masks, including zero-blur translated shadows, need
					// only the original single atlas lookup and Tile color ABI.
					compiled.shaderClass = A8CompiledShaderClass::Coverage;
				}
				else if (metadata.effects.shaderEffects && range.layer != 3)
				{
					compiled.shaderClass = A8CompiledShaderClass::Effect;
				}
				else
				{
					compiled.shaderClass = A8CompiledShaderClass::Body;
				}
				if (compiled.shaderClass == A8CompiledShaderClass::Effect)
				{
					switch (metadata.effects.quality)
					{
					case EffectQuality::Balanced:
						compiled.textureSamplesPerGlyph = 5;
						break;
					case EffectQuality::High:
						compiled.textureSamplesPerGlyph = 9;
						break;
					default:
						compiled.textureSamplesPerGlyph = 1;
						break;
					}
				}
				if (!haveFirstRange)
				{
					metadata.firstRangeShaderClass = compiled.shaderClass;
					haveFirstRange = true;
				}
				if (!haveFirstFill && range.layer == 3)
				{
					metadata.firstFillShaderClass = compiled.shaderClass;
					haveFirstFill = true;
				}
				compiled.staticSmoothSampling = range.layer == 1 || range.layer == 2
					|| (range.layer == 0
						&& metadata.effects.shadowBlurPixels > 0.001f)
					|| range.usesSdf;
				metadata.compiledRanges.push_back(std::move(compiled));
			}
		}

		bool TryInitializeA8Renderer(bool forceShaderAttempt, bool reportFailures)
		{
			if (!g_bEnableFreeTypeFontRendering)
			{
				State().a8Available = false;
				State().rangeBridgeAvailable = false;
				return false;
			}

			// The original-shader ARGB path needs only the Tile/D3D range bridge;
			// shader-loader discovery is an independent, opportunistic upgrade. Once
			// published, the bridge remains valid until the device changes or the
			// per-frame pointer audit marks it stale, so text construction must not
			// rerun the installation machinery.
			if (!IsPublishedRangeBridgeReady())
				State().rangeBridgeAvailable = HookD3DDevice();
			if (!g_bEnableFreeTypeA8Atlas)
			{
				State().a8Available = false;
				return false;
			}
			const bool baseShaderReady = HaveA8Shader();
			State().a8Available = baseShaderReady && State().rangeBridgeAvailable;
			if (baseShaderReady && HaveAllEffectShaders())
			{
				return State().a8Available;
			}
			if (State().initializationInProgress)
				return State().a8Available;

			const DWORD now = GetTickCount();
			if (!forceShaderAttempt && State().initializationAttempted
				&& now - State().lastInitializationAttemptTick < 1000)
			{
				return State().a8Available;
			}
			State().initializationAttempted = true;
			State().lastInitializationAttemptTick = now;
			State().initializationInProgress = true;
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
				State().shaderLoaderCompatible = true;
				loaded = baseShaderReady || LoadA8Shader(createPixelShader);
				if (loaded)
					LoadEffectShaders(createPixelShader);
			} while (false);
			State().initializationInProgress = false;
			State().a8Available = HaveA8Shader() && State().rangeBridgeAvailable;

			if (!HaveA8Shader() && reportFailures
				&& !State().loggedShaderLoaderUnavailable)
			{
				State().loggedShaderLoaderUnavailable = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: Shader Loader/PS3 font shader is not ready; retaining range-safe 32-bit atlases and retrying later");
			}
			return State().a8Available;
		}

	void FinalizeA8RendererDetection()
	{
		TryInitializeA8Renderer(true, true);
	}

	void HandleA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		void** deviceVtable = device ? *reinterpret_cast<void***>(device) : nullptr;
		void** rendererVtable = renderer ? *reinterpret_cast<void***>(renderer) : nullptr;
		const bool drawHookCurrent = deviceVtable
			&& deviceVtable[kDrawIndexedPrimitiveSlot]
				== reinterpret_cast<void*>(&A8DrawIndexedPrimitive);
		const bool rendererHooksCurrent = drawHookCurrent && rendererVtable
			&& rendererVtable[kRendererBatchRenderShapeSlot]
				== reinterpret_cast<void*>(&A8BatchRenderShape)
			&& rendererVtable[kRendererRenderShapeSlot]
				== reinterpret_cast<void*>(&A8RenderShape)
			&& rendererVtable[kRendererRenderShapeAltSlot]
				== reinterpret_cast<void*>(&A8RenderShapeAlt);
		// Either renderer route is sufficient for the bridge decision. Reading and
		// decoding the Tile call target cannot affect the result once the renderer
		// route is current, and is also irrelevant while the DIP hook is missing.
		const bool tileHookCurrent = drawHookCurrent && !rendererHooksCurrent
			&& ReadTileRenderPassCallTarget()
				== reinterpret_cast<TileRenderPassFn>(&A8TileRenderPass);
		const bool bridgeCurrent = drawHookCurrent
			&& (rendererHooksCurrent || tileHookCurrent);
		const bool haveA8Shader = g_bEnableFreeTypeA8Atlas && HaveA8Shader();
		if (bridgeCurrent && (!g_bEnableFreeTypeA8Atlas
			|| (haveA8Shader && HaveAllEffectShaders())))
		{
			State().hookedDevice = device;
			State().rangeBridgeAvailable = true;
			State().a8Available = haveA8Shader;
			return;
		}
		if (!bridgeCurrent)
		{
			// Publish the pointer-audit result before entering the shared initializer;
			// its hot-path fast check must not mistake an overwritten vtable entry for
			// a still-current bridge on the same device.
			State().rangeBridgeAvailable = false;
			State().a8Available = false;
		}
		const DWORD now = GetTickCount();
		if (bridgeCurrent && State().initializationAttempted
			&& now - State().lastInitializationAttemptTick < 1000)
		{
			return;
		}
		TryInitializeA8Renderer(false, false);
	}

	void HandleA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage)
			return;
		TryInitializeA8Renderer(true, false);
		if (!State().shaderLoaderCompatible)
			return;
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (RefreshShaderSet(createPixelShader))
		{
			State().rangeBridgeAvailable = HookD3DDevice();
			State().a8Available = State().rangeBridgeAvailable;
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: shader refresh validation failed; retaining the previous shader set");
		}
	}

	bool IsA8RendererAvailable()
	{
		return TryInitializeA8Renderer(false, false);
	}

	bool IsAtlasRangeRendererAvailable()
	{
		if (IsPublishedRangeBridgeReady())
			return true;
		State().rangeBridgeAvailable = HookD3DDevice();
		return IsPublishedRangeBridgeReady();
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
		auto metadata = std::make_shared<A8ShapeMetadata>();
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		if (NiTriShapeData* data = shape->GetModelData())
		{
			metadata->vertexCount = data->m_usVertices;
			metadata->primitiveCount = data->m_usTriangles;
			metadata->indexCount = data->m_uiTriListLength
				? data->m_uiTriListLength : data->m_usTriangles * 3;
		}
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (effectConfig)
			metadata->effects = *effectConfig;
		CompileA8DrawRanges(*metadata);
		{
			std::lock_guard<std::mutex> lock(State().diagnosticsMutex);
			State().shapeMetadata[shape] = std::move(metadata);
		}
		// Publish the marker vtable only after its metadata is complete. The draw
		// bridge must never observe a custom shape with a stale/default contract.
		*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		if (useOriginalShader && !rangeBridgeReady
			&& g_bEnableFreeTypeFontRenderingLog && !State().loggedPendingRangeShape)
		{
			State().loggedPendingRangeShape = true;
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
