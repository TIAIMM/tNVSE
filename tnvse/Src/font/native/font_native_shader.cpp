#include "font_native_shader_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	NativeFontBlendState ComputeNativeFontOwnedBlendState(
		const NiPropertyState* properties)
	{
		NativeFontBlendState state;
		if (!properties)
			return state;

		const BSShaderProperty* shade =
			properties->GetShadeProperty<BSShaderProperty>();
		if (!shade || shade->m_eShaderType == -1)
			return state;

		const NiAlphaProperty* alpha = properties->GetAlphaProperty();
		const UInt16 flags = alpha ? alpha->m_usFlags.Get() : 0;
		const bool propertyBlend = alpha
			&& (flags & NiAlphaProperty::ALPHA_BLEND_MASK) != 0;
		const bool opacityBlend = shade->fFadeAlpha < 1.0f
			|| (shade->fAlpha < 1.0f && !shade->HasNoFade());
		state.enabled = propertyBlend || opacityBlend;
		if (propertyBlend)
		{
			state.sourceFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::SRC_BLEND_MASK)
					>> NiAlphaProperty::SRC_BLEND_POS);
			state.destinationFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::DEST_BLEND_MASK)
					>> NiAlphaProperty::DEST_BLEND_POS);
		}
		return state;
	}

	namespace implementation::font_native_shader
	{
	NativeShaderRuntimeState& ShaderState()
	{
		static NativeShaderRuntimeState state;
		return state;
	}

	NativeShaderThreadState& ShaderThread()
	{
		thread_local NativeShaderThreadState state;
		return state;
	}
	}

	bool DeferInterruptedNativeFontRendererResetRecovery(const char* reason)
	{
		// Retail NiDX9Renderer::Reset (0xE736B0) returns immediately when any
		// before callback reports false.  It does not invoke the matching after
		// callback for callbacks that already released their resources.  Reopen
		// native publication now and force a rebuild at the next main-loop pump.
		if (!ShaderState().resetLifecycle.DeferInterruptedRecovery())
		{
			return false;
		}
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		gLog.FormattedMessage(
			"tnvse_freetype_native: interrupted renderer reset recovery deferred reason=%s generation=%u deviceEpoch=%u",
			reason ? reason : "unknown", current ? current->id : 0,
			ShaderState().deviceEpoch.load(std::memory_order_acquire));
		return true;
	}

	bool InitializeNativeFontRenderer(bool forceAttempt, bool reportFailures)
	{
		if (ShaderState().resetLifecycle.InProgress()
			|| !g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeNativeAtlas)
			return false;
		if (!forceAttempt)
		{
			NativeShaderGeneration* published = ShaderState().publishedGeneration.load(
				std::memory_order_acquire);
			if (GenerationMatchesCurrentDevice(published))
				return true;
		}

		std::lock_guard<std::mutex> lock(ShaderState().initializationMutex);
		if (ShaderState().resetLifecycle.InProgress())
			return false;
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (!forceAttempt && GenerationMatchesCurrentDevice(current))
			return true;

		const DWORD now = GetTickCount();
		if (!forceAttempt && ShaderState().lastInitializationAttempt
			&& now - ShaderState().lastInitializationAttempt < kInitializationRetryMilliseconds)
		{
			return GenerationMatchesCurrentDevice(current);
		}
		ShaderState().lastInitializationAttempt = now;

		const char* failure = "unknown";
		CreateVertexShaderFn createVS = nullptr;
		CreatePixelShaderFn createPS = nullptr;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		NativeShaderGeneration* candidate = nullptr;
		if (renderer && device && ShaderState().resetRenderer != renderer)
		{
			ThisStdCall<UInt32>(0x86BAE0, renderer,
				NativeRendererResetCallback, renderer);
			ShaderState().resetRenderer = renderer;
			gLog.FormattedMessage(
				"tnvse_freetype_native: registered renderer reset lifecycle renderer=%p",
				renderer);
		}
		if (renderer && device
			&& renderer->m_kD3DCaps9.VertexShaderVersion >= D3DVS_VERSION(3, 0)
			&& renderer->m_kD3DCaps9.PixelShaderVersion >= D3DPS_VERSION(3, 0)
			&& renderer->m_kD3DCaps9.MaxVertexShaderConst
				> kNativeFontVertexAaConstantRegister
			&& ResolveShaderLoader(createVS, createPS, failure))
		{
			candidate = BuildGeneration(createVS, createPS, renderer, device,
				failure);
		}
		else if (!renderer || !device)
		{
			failure = "renderer-device";
		}
		else if (renderer->m_kD3DCaps9.VertexShaderVersion < D3DVS_VERSION(3, 0)
			|| renderer->m_kD3DCaps9.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			failure = "shader-model-3";
		}
		else if (renderer->m_kD3DCaps9.MaxVertexShaderConst
			<= kNativeFontVertexAaConstantRegister)
		{
			failure = "shader-constant-registers";
		}

		if (!candidate || !GenerationResourcesReady(candidate))
		{
			if (reportFailures)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: initialization failed reason=%s; retaining generation=%u when valid; new text uses the vanilla ARGB TileShader fallback when no complete native generation is available",
					failure, current ? current->id : 0);
			}
			return GenerationMatchesCurrentDevice(current);
		}

		candidate->id = ShaderState().nextGeneration++;
		ShaderState().processGenerations.push_back(candidate);
		ShaderState().publishedGeneration.store(candidate, std::memory_order_release);
		NotifyNativeFontCommandExternalMutation(
			NativeFontCommandFallback::Generation);
		const char* compositeProfileMode =
			!UsesBakedEffectRoute()
				&& candidate->distanceFieldMethod
					== DistanceFieldMethod::Mtsdf
				? "lazy-36" : "disabled";
		gLog.FormattedMessage(
			"tnvse_freetype_native: published complete TileShader generation=%u device=%p route=%s distanceField=%s mtsdfCompositeProfiles=%s constantAbi=vanilla-ps-c0-vs-c0-c4-private-ps-c176-c183-vs-c208-c209 privateUpload=prefix-2-4-8 vanillaC0=map-owned vanillaLayoutCarry=witnessed-native-callback foreignPassBoundary=hard-invalidate vertexAa=analytic-c208-all-layouts vanillaGlyph=c209 vertexFormat=float4 vertexStride=%u declTypes=0x%08X maxVertexConstants=%u",
			candidate->id, candidate->device,
			UsesBakedEffectRoute()
				? "argb-composite" : "distance-field",
			UsesBakedEffectRoute()
				? "disabled" : GetConfiguredDistanceFieldMethodName(),
			compositeProfileMode,
			static_cast<UInt32>(sizeof(NativeFontGpuVertex)),
			candidate->renderer->m_kD3DCaps9.DeclTypes,
			candidate->renderer->m_kD3DCaps9.MaxVertexShaderConst);
		return true;
	}

	void HandleNativeFontShaderRendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeNativeAtlas)
			return;
		// The engine executes reset callbacks synchronously on the renderer/main
		// thread.  A pending before phase observed by a later MainGameLoop message
		// therefore cannot still be live: the reset attempt either failed or a
		// foreign callback aborted the chain before our after phase was reached.
		const auto recovery =
			ShaderState().resetLifecycle.ClaimForMainLoop();
		if (recovery.orphanedBefore)
		{
			NativeShaderGeneration* interrupted =
				ShaderState().publishedGeneration.load(std::memory_order_acquire);
			gLog.FormattedMessage(
				"tnvse_freetype_native: recovered orphaned renderer reset before phase source=main-loop generation=%u deviceEpoch=%u",
				interrupted ? interrupted->id : 0,
				ShaderState().deviceEpoch.load(std::memory_order_acquire));
		}
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		const bool deviceChanged = current && device
			&& (current->renderer != renderer || current->device != device);
		if (!GenerationMatchesCurrentDevice(current))
			InitializeNativeFontRenderer(
				recovery.recoveryPending || deviceChanged,
				recovery.recoveryPending);
	}

	void HandleNativeFontShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage)
			return;
		NativeShaderGeneration* before = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		const UInt32 beforeId = before ? before->id : 0;
		InitializeNativeFontRenderer(true, true);
		NativeShaderGeneration* after = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (!after || after->id == beforeId)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: shader refresh did not publish a complete generation; retained generation=%u",
				beforeId);
		}
	}

	bool IsNativeFontShaderRendererAvailable()
	{
		NativeShaderGeneration* generation = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (GenerationMatchesCurrentDevice(generation))
			return true;
		if (!InitializeNativeFontRenderer(false, false))
			return false;
		return GenerationMatchesCurrentDevice(ShaderState().publishedGeneration.load(
			std::memory_order_acquire));
	}

	bool GetNativeFontRendererReadinessFast(
		NativeFontRendererReadinessView& view)
	{
		view = {};
		NativeShaderGeneration* generation = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (!generation
			|| ShaderState().resetLifecycle.InProgress()
			|| generation->runtimeFault.load(std::memory_order_acquire)
			|| !generation->renderer || !generation->device
			|| !generation->declaration || !generation->d3dDeclaration
			|| !generation->vertexShader)
		{
			return false;
		}
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer
			? renderer->GetD3DDevice() : nullptr;
		if (generation->renderer != renderer || generation->device != device)
			return false;
		view.renderer = renderer;
		view.device = device;
		view.generation = generation->id;
		view.ready = view.generation != 0;
		return view.ready;
	}

	UInt32 GetNativeFontShaderGeneration()
	{
		NativeShaderGeneration* generation = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		return GenerationMatchesCurrentDevice(generation) ? generation->id : 0;
	}

	IDirect3DVertexDeclaration9* GetNativeFontD3DDeclaration(UInt32 generation)
	{
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		return current && current->id == generation
			&& GenerationMatchesCurrentDevice(current)
			? current->d3dDeclaration : nullptr;
	}

	bool IsNativeFontShaderGenerationCurrent(UInt32 generation)
	{
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		return current && current->id == generation
			&& !ShaderState().resetLifecycle.InProgress()
			&& !current->runtimeFault.load(std::memory_order_acquire);
	}

}
