#include "font_native_shader_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	namespace implementation::font_native_shader
	{
		HRESULT EnsureNativeSamplerState(IDirect3DDevice9* device,
			bool& changed, const char*& operation)
		{
			changed = false;
			operation = "none";
			NiD3DRenderState* renderState =
				ResolveSortedRenderState(device);
			if (!renderState)
			{
				operation = "resolve-engine-render-state(sampler)";
				return D3DERR_INVALIDCALL;
			}
			NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
			if (batch.depth && batch.device == device
				&& batch.samplerReady
				&& IsNativeMipFilterReady(renderState))
			{
				return D3D_OK;
			}

			// The retail reverse map has no SRGBTEXTURE slot; routing that enum
			// through NiD3DRenderState is a no-op. Fallout's Tile texture path
			// never publishes SRGBTEXTURE and D3D9 defaults it to FALSE, so do
			// not pay for a redundant virtual call that cannot affect state.
			// MIPFILTER is mirrored. Read that zero-driver-cost value first and
			// enter the engine setter only when normalization is actually needed.
			if (!IsNativeMipFilterReady(renderState))
			{
				renderState->SetSamplerState(
					0, D3DSAMP_MIPFILTER, D3DTEXF_NONE, false);
				if (!IsNativeMipFilterReady(renderState))
				{
					operation = "SetSamplerState(mip-shadow)";
					return E_FAIL;
				}
				changed = true;
			}
			if (batch.depth)
				batch.samplerReady = true;
			return D3D_OK;
		}

		HRESULT EnsureNativeTexture0(IDirect3DDevice9* device,
			const void* atlasTexture, const char*& operation)
		{
			operation = "none";
			if (!device || !atlasTexture)
			{
				operation = "validate-command-texture";
				return D3DERR_INVALIDCALL;
			}
			auto* texture = const_cast<IDirect3DBaseTexture9*>(
				static_cast<const IDirect3DBaseTexture9*>(atlasTexture));
			NiD3DRenderState* renderState =
				ResolveSortedRenderState(device);
			if (!renderState)
			{
				operation = "resolve-engine-render-state(texture)";
				return D3DERR_INVALIDCALL;
			}
			// The engine mirror is authoritative for texture stage 0. Requiring
			// a second traversal-local ready bit caused every command to call
			// SetTexture when a vanilla bootstrap had already installed this page.
			if (renderState->m_apkTextureStageTextures[0] == texture)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTextureBindReuse);
				return D3D_OK;
			}
			// Direct IDirect3DDevice9::SetTexture left Gamebryo believing the
			// previous page was still bound. A later vanilla setup could then
			// suppress a required rebind. Publish through NiD3DRenderState so
			// both the device and its software mirror advance together.
			renderState->SetTexture(0, texture);
			if (renderState->m_apkTextureStageTextures[0] != texture)
			{
				operation = "SetTexture(command-page-shadow)";
				return E_FAIL;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTextureBindSet);
			return D3D_OK;
		}

		HRESULT PublishRetainedPacketConstants(
			IDirect3DDevice9* device, NativeShaderGeneration* generation,
			NativeShaderProfile* profile, const char*& operation)
		{
			operation = "none";
			if (!device || !generation || !profile)
			{
				operation = "validate-command-packet-constants";
				return D3DERR_INVALIDCALL;
			}
			NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
			const bool batchActive = batch.depth != 0;
			if (batchActive && (batch.device != device
				|| batch.generation != generation->id))
			{
				ResetSortedShaderIdentity(
					batch, device, generation->id);
			}

			HRESULT result = D3D_OK;
			const size_t targetRegisterCount =
				profile->privateRegisterCount;
			if (!batchActive || !batch.packetProfile
				|| !batch.packetRegisterCount)
			{
				result = device->SetPixelShaderConstantF(
					kNativeFontPixelConstantBaseRegister,
					profile->constants.data(), static_cast<UINT>(
						targetRegisterCount));
				if (FAILED(result))
				{
					operation =
						"SetPixelShaderConstantF(command-private-prefix)";
					return result;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantFullUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantRegisterUpload,
					targetRegisterCount);
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantFullTailElided,
					kNativeFontPacketConstantRegisterCount
						- targetRegisterCount);
			}
			else
			{
				size_t firstChanged =
					targetRegisterCount;
				size_t lastChanged = 0;
				if (batch.packetProfile != profile
					|| batch.packetRegisterCount < targetRegisterCount)
				{
					for (size_t packetRegister = 0;
						packetRegister
							< targetRegisterCount;
						++packetRegister)
					{
						const size_t firstFloat =
							packetRegister * 4u;
						if (packetRegister
								>= batch.packetRegisterCount
							|| std::memcmp(
								profile->constants.data() + firstFloat,
								batch.packetProfile->constants.data()
									+ firstFloat,
								4u * sizeof(float)) != 0)
						{
							firstChanged = std::min(
								firstChanged, packetRegister);
							lastChanged = packetRegister;
						}
					}
				}
				if (firstChanged
					< targetRegisterCount)
				{
					result = device->SetPixelShaderConstantF(
						static_cast<UINT>(
							kNativeFontPixelConstantBaseRegister
								+ firstChanged),
						profile->constants.data()
							+ firstChanged * 4u,
						static_cast<UINT>(
							lastChanged - firstChanged + 1u));
					if (FAILED(result))
					{
						operation =
							"SetPixelShaderConstantF(command-partial)";
						return result;
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantPartialUpload);
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantRegisterUpload,
						lastChanged - firstChanged + 1u);
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantReuse);
				}
			}
			if (batchActive)
			{
				batch.packetProfile = profile;
				batch.packetRegisterCount =
					profile->privateRegisterCount;
			}
			return D3D_OK;
		}

	}

	bool BindNativeFontCommandPacket(
		const NativeFontCompiledPacketCommand& command,
		const void* atlasTexture, bool publishPrograms,
		const NiPropertyState* properties,
		const NativeFontCommandBindState& bindState,
		const char*& operation, HRESULT& result)
	{
		operation = "none";
		result = D3D_OK;
		auto* profile = static_cast<NativeShaderProfile*>(command.profile);
		NativeShaderGeneration* generation =
			profile ? profile->owner : nullptr;
		IDirect3DDevice9* device = command.device;
		void** shaderVtable = command.shader
			? *reinterpret_cast<void***>(command.shader) : nullptr;
		if (!command.active || !profile || !generation || !device
			|| profile->shader != command.shader
			|| !shaderVtable
			|| shaderVtable[30] != command.setupGeometryTextures
			|| shaderVtable[32] != command.setupGeometryAlphaBlending
			|| shaderVtable[33] != command.setupGeometryAlphaTesting
			|| shaderVtable[34] != command.setupGeometryRenderStates
			|| generation->id != command.generation
			|| generation->device != device
			|| !GenerationMatchesCurrentDevice(generation)
			|| !atlasTexture)
		{
			operation = "validate-command-packet";
			result = D3DERR_INVALIDCALL;
			return false;
		}

		NativeSortedShaderBatch& sortedBatch = ShaderThread().sortedShaderBatch;
		if (sortedBatch.depth
			&& (sortedBatch.device != device
				|| sortedBatch.generation != generation->id))
		{
			ResetSortedShaderIdentity(
				sortedBatch, device, generation->id);
		}
		NiD3DRenderState* renderState =
			ResolveSortedRenderState(device);
		if (!renderState)
		{
			operation = "resolve-engine-render-state(program)";
			result = D3DERR_INVALIDCALL;
			return false;
		}
		const bool programsReady =
			renderState->m_hCurrentVertexShader == command.vertexShader
			&& renderState->m_hCurrentPixelShader == command.pixelShader;
		// A profile change can also change pass render state even when two
		// profiles share shader handles, so it still requires
		// SetupGeometryTextures. If the
		// caller retained the same profile but an unexpected program mutation
		// occurred, repair it here instead of trusting a stale local pointer.
		const bool setupRequired = publishPrograms || !programsReady;
		if (setupRequired)
		{
			if (!command.setupGeometryTextures || !properties)
			{
				operation = "validate-command-pass-state";
				result = D3DERR_INVALIDCALL;
				return false;
			}
			using SetupGeometryTexturesFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			reinterpret_cast<SetupGeometryTexturesFn>(
				command.setupGeometryTextures)(command.shader, properties);
			using SetupStateFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			using SetupGeometryRenderStatesFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*, bool);
			if (bindState.applyBlend)
			{
				if (!command.setupGeometryAlphaBlending)
				{
					operation = "validate-command-blend-state";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupStateFn>(
					command.setupGeometryAlphaBlending)(
						command.shader, properties);
			}
			if (bindState.applyAlphaTest)
			{
				if (!command.setupGeometryAlphaTesting)
				{
					operation = "validate-command-alpha-state";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupStateFn>(
					command.setupGeometryAlphaTesting)(
						command.shader, properties);
			}
			if (bindState.applyRenderStates)
			{
				if (!command.setupGeometryRenderStates)
				{
					operation = "validate-command-render-states";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupGeometryRenderStatesFn>(
					command.setupGeometryRenderStates)(
						command.shader, properties,
						bindState.firstPass);
			}
			// Reverse-verified TileShader::SetupGeometryTextures publishes both
			// programs through NiD3DRenderState before configuring stage 0.
			// Reissuing SetVertexShader/SetPixelShader here was a guaranteed
			// duplicate driver submission. Validate the software mirror instead.
			renderState = ResolveSortedRenderState(device);
			if (!renderState
				|| renderState->m_hCurrentVertexShader
					!= command.vertexShader
				|| renderState->m_hCurrentPixelShader
					!= command.pixelShader)
			{
				operation = "validate-command-program-shadow";
				result = E_FAIL;
				return false;
			}
			if (sortedBatch.depth)
			{
				// SetupGeometryTextures may publish MIPFILTER. Preserve a ready
				// result when the engine mirror already contains the native
				// level-zero contract instead of invalidating it unconditionally.
				sortedBatch.samplerReady =
					IsNativeMipFilterReady(renderState);
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandProgramSetup);
		}
		else
		{
			// Both engine program handles already match the retained command.
			// Count the two VS/PS publications avoided by the profile-local fast
			// path; no driver query or setup callback is needed.
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandProgramBindElided, 2);
		}

		if (!command.simpleColor)
		{
			const bool packetStateReady = sortedBatch.depth
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id
				&& sortedBatch.packetProfile == profile
				&& sortedBatch.packetRegisterCount
					>= profile->privateRegisterCount
				&& sortedBatch.vertexAa.aaConstantReady
				&& sortedBatch.samplerReady
				&& IsNativeMipFilterReady(renderState);
			const bool publishPacketState = publishPrograms
				|| !programsReady || !packetStateReady;
			if (publishPacketState)
			{
				result = PublishRetainedPacketConstants(
					device, generation, profile, operation);
				if (FAILED(result))
					return false;
				const char* vertexOperation = "none";
				NativeVertexAaState* vertexCache =
					sortedBatch.depth
						? &sortedBatch.vertexAa : nullptr;
				result = PublishNativeVertexAaConstant(device,
					profile->constants[7], vertexCache, false, nullptr,
					vertexOperation);
				if (FAILED(result))
				{
					operation = vertexOperation;
					return false;
				}
				bool samplerChanged = false;
				const char* samplerOperation = "none";
				result = EnsureNativeSamplerState(
					device, samplerChanged, samplerOperation);
				if (FAILED(result))
				{
					operation = samplerOperation;
					return false;
				}
				RecordFreeTypePerf(samplerChanged
					? FreeTypePerfCounter::SamplerStateSet
					: FreeTypePerfCounter::SamplerStateReuse);
				if (sortedBatch.depth)
					sortedBatch.samplerReady = true;
			}
			else
			{
				// Same retained profile and unchanged engine mirrors prove that
				// pixel c176-c183, vertex c208 and MIPFILTER all remain current.
				// Account the implicit fast path without invoking three helper
				// binders.
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandPacketConstantReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::VertexAaConstantReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SamplerStateReuse);
			}
		}

		result = EnsureNativeTexture0(device, atlasTexture, operation);
		if (FAILED(result))
			return false;
		return true;
	}
}
