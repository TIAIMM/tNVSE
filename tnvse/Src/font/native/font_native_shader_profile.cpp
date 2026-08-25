#include "font_native_shader_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	namespace implementation::font_native_shader
	{
		NativeProfileKey MakeProfileKey(const NativeFontPacketTemplate& packet,
			NativeFontSampling sampling, bool writeEffectAlpha,
			NativeFontVanillaLayoutKind vanillaLayoutKind)
		{
			NativeProfileKey key;
			key.shaderClass = packet.shaderClass;
			key.sampling = sampling;
			key.quality = packet.quality;
			key.distanceFieldMethod = packet.distanceFieldMethod;
			key.staticCompositeLayerMask =
				packet.staticCompositeLayerMask;
			key.compositeShiftedShadow =
				packet.compositeShiftedShadow;
			key.writeEffectAlpha = writeEffectAlpha;
			key.usesLiveTileRgb = packet.usesLiveTileRgb;
			key.vanillaLayoutKind = vanillaLayoutKind;
			std::memcpy(key.constantBits.data(), packet.constants.data(),
				key.constantBits.size() * sizeof(UInt32));
			if (vanillaLayoutKind == NativeFontVanillaLayoutKind::Uniform40)
			{
				std::memcpy(&key.constantBits[6],
					&packet.uniformSdfSpread, sizeof(UInt32));
				std::memcpy(&key.uniformDistanceParameterScaleBits,
					&packet.uniformDistanceParameterScale, sizeof(UInt32));
			}
			if (!UsesNativeFontVanillaLayout(vanillaLayoutKind)
				&& packet.sampling == sampling)
			{
				key.precomputedHash =
					packet.profileHashes[writeEffectAlpha ? 1u : 0u];
			}
			return key;
		}

		NativeFontSampling ResolveEffectiveSampling(
			const NativeFontPacketTemplate& packet,
			bool scaledFillSampling)
		{
			static_cast<void>(scaledFillSampling);
			// Distance-field atlas pages are explicitly level-zero-only.
			return NativeFontSampling::LinearLod0;
		}

		NiTexturingProperty::FilterMode ResolveFilterMode(
			NativeFontSampling sampling)
		{
			switch (sampling)
			{
			case NativeFontSampling::LinearMipmapped:
				return NiTexturingProperty::FILTER_TRILERP;
			case NativeFontSampling::LinearLod0:
				return NiTexturingProperty::FILTER_BILERP;
			default:
				return NiTexturingProperty::FILTER_NEAREST;
			}
		}

		NiD3DPixelShader* ResolveProfilePixelShader(
			NativeShaderGeneration& generation,
			const NativeFontPacketTemplate& packet,
			NativeFontVanillaLayoutKind vanillaLayoutKind)
		{
			if (UsesNativeFontVanillaLayout(vanillaLayoutKind))
			{
				const size_t qualityIndex =
					static_cast<size_t>(packet.quality);
				const bool supportedDistanceField =
					packet.distanceFieldMethod == DistanceFieldMethod::TrueSdf
					|| packet.distanceFieldMethod == DistanceFieldMethod::Mtsdf;
				if (!IsVanillaLayoutGenerationReady(
						&generation, vanillaLayoutKind)
					|| !supportedDistanceField
					|| generation.distanceFieldMethod
						!= packet.distanceFieldMethod
					|| packet.shaderClass != NativeFontShaderClass::Composite
					|| (packet.compositeShiftedShadow
						&& !(packet.staticCompositeLayerMask & 1u))
					|| packet.staticCompositeLayerMask
						< kStaticCompositeLayerMaskFirst
					|| packet.staticCompositeLayerMask
						>= kStaticCompositeLayerMaskFirst
							+ kStaticCompositeLayerMaskCount
					|| qualityIndex
						>= generation.vanillaLayoutCompositeShaders.size())
				{
					return nullptr;
				}
				const size_t maskIndex = packet.staticCompositeLayerMask
					- kStaticCompositeLayerMaskFirst;
				const size_t shiftedIndex = packet.compositeShiftedShadow
					? 1u : 0u;
				NiD3DPixelShaderPtr& slot =
					generation.vanillaLayoutCompositeShaders[qualityIndex]
						[maskIndex][shiftedIndex];
				bool& attempted =
					generation.vanillaLayoutCompositeAttempts[qualityIndex]
						[maskIndex][shiftedIndex];
				if (!HasShaderHandle(slot) && !attempted
					&& generation.createPixelShader)
				{
					attempted = true;
					const char* qualityNames[] = {
						"fast", "balanced", "high"
					};
					const char* shiftedSuffix =
						packet.compositeShiftedShadow ? "_shift" : "";
					const char* distanceFieldName =
						packet.distanceFieldMethod == DistanceFieldMethod::TrueSdf
							? "sdf" : "mtsdf";
					char shaderName[128] = {};
					sprintf_s(shaderName,
						"tnvse_freetype_native_%s_vanilla_layout_%s_m%u%s.pso",
						distanceFieldName, qualityNames[qualityIndex],
						static_cast<UInt32>(
							packet.staticCompositeLayerMask), shiftedSuffix);
					slot = generation.createPixelShader(shaderName);
				}
				return HasShaderHandle(slot) ? slot.m_pObject : nullptr;
			}
			if (packet.shaderClass != NativeFontShaderClass::Coverage
				&& packet.shaderClass != NativeFontShaderClass::Argb
				&& packet.distanceFieldMethod != generation.distanceFieldMethod)
				return nullptr;
			switch (packet.shaderClass)
			{
			case NativeFontShaderClass::Body:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				return index < generation.distanceFieldFillShaders.size()
					? generation.distanceFieldFillShaders[index].m_pObject : nullptr;
			}
			case NativeFontShaderClass::Effect:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				return index < generation.effectShaders.size()
					? generation.effectShaders[index].m_pObject : nullptr;
			}
			case NativeFontShaderClass::Composite:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				if (generation.distanceFieldMethod
						== DistanceFieldMethod::Mtsdf
					&& index
						< generation.mtsdfCompositeProfileShaders.size()
					&& packet.staticCompositeLayerMask
						>= kStaticCompositeLayerMaskFirst
					&& packet.staticCompositeLayerMask
						< kStaticCompositeLayerMaskFirst
							+ kStaticCompositeLayerMaskCount)
				{
					const size_t maskIndex =
						packet.staticCompositeLayerMask
							- kStaticCompositeLayerMaskFirst;
					const size_t shiftIndex =
						packet.compositeShiftedShadow ? 1u : 0u;
					NiD3DPixelShaderPtr& specializedSlot =
						generation.mtsdfCompositeProfileShaders[index]
							[maskIndex][shiftIndex];
					bool& attempted =
						generation.mtsdfCompositeProfileAttempts[index]
							[maskIndex][shiftIndex];
					if (!HasShaderHandle(specializedSlot) && !attempted
						&& generation.createPixelShader)
					{
						// ResolveNativeFontPacketShader serializes profile creation
						// with profileMutex, so optional shader publication needs no
						// second lock and allocates only profiles actually observed.
						attempted = true;
						const char* qualityNames[] = {
							"fast", "balanced", "high"
						};
						char shaderName[128] = {};
						sprintf_s(shaderName,
							"tnvse_freetype_native_mtsdf_composite_%s_m%u%s.pso",
							qualityNames[index],
							static_cast<UInt32>(
								packet.staticCompositeLayerMask),
							packet.compositeShiftedShadow
								? "_shift" : "");
						specializedSlot =
							generation.createPixelShader(shaderName);
					}
					NiD3DPixelShader* specialized =
						specializedSlot.m_pObject;
					if (specialized && specialized->GetShaderHandle())
						return specialized;
				}
				return index < generation.compositeShaders.size()
					? generation.compositeShaders[index].m_pObject : nullptr;
			}
			case NativeFontShaderClass::Coverage:
				return generation.coverageShader.m_pObject;
			case NativeFontShaderClass::Argb:
				return generation.argbShader.m_pObject;
			default:
				return nullptr;
			}
		}

		void SetPassRenderState(NiD3DPass* pass,
			D3DRENDERSTATETYPE state, DWORD value)
		{
			ThisStdCall<void>(kNiD3DPassSetRenderState, pass, state,
				static_cast<UInt32>(value), true);
		}

		void ConfigureProfilePass(NativeShaderGeneration& generation,
			NativeShaderProfile& profile, NiD3DPass& pass)
		{
			const NiTexturingProperty::FilterMode filter =
				ResolveFilterMode(profile.key.sampling);
			for (UInt32 stageIndex = 0;
				stageIndex < std::min<UInt32>(pass.m_uiStageCount, 2); ++stageIndex)
			{
				NiD3DTextureStage* stage = pass.GetStage(stageIndex);
				if (!stage)
					continue;
				CdeclCall<void>(kNiD3DTextureStageSetProperties, stage, stageIndex,
					NiTexturingProperty::CLAMP_S_CLAMP_T,
					static_cast<UInt32>(filter), false);
				ThisStdCall<void>(kNiD3DTextureStageSetFilterMode, stage, filter);
			}

			SetPassRenderState(&pass, D3DRS_ZENABLE, FALSE);
			SetPassRenderState(&pass, D3DRS_ZWRITEENABLE, FALSE);
			SetPassRenderState(&pass, D3DRS_ALPHATESTENABLE, FALSE);
			if (profile.effectPass)
			{
				SetPassRenderState(&pass, D3DRS_STENCILWRITEMASK, 0);
				const DWORD rgbWrite = D3DCOLORWRITEENABLE_RED
					| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
				SetPassRenderState(&pass, D3DRS_COLORWRITEENABLE,
					rgbWrite | (profile.key.writeEffectAlpha
						? D3DCOLORWRITEENABLE_ALPHA : 0));
				if (generation.supportsSeparateAlpha
					&& profile.key.writeEffectAlpha)
				{
					SetPassRenderState(&pass, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
					SetPassRenderState(&pass, D3DRS_DESTBLENDALPHA,
						D3DBLEND_INVSRCALPHA);
					SetPassRenderState(&pass, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
					SetPassRenderState(&pass, D3DRS_SEPARATEALPHABLENDENABLE,
						TRUE);
				}
			}
			else
			{
				const DWORD colorWrite = D3DCOLORWRITEENABLE_RED
					| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE
					| D3DCOLORWRITEENABLE_ALPHA;
				SetPassRenderState(&pass, D3DRS_COLORWRITEENABLE, colorWrite);
			}
		}

		NativeShaderProfile* CreateProfile(NativeShaderGeneration& generation,
			const NativeFontPacketTemplate& packet, const NativeProfileKey& key)
		{
			NiD3DPixelShader* pixelShader = ResolveProfilePixelShader(generation,
				packet, key.vanillaLayoutKind);
			if (!pixelShader || !pixelShader->GetShaderHandle())
				return nullptr;
			NiD3DVertexShader* vertexShader =
				UsesNativeFontVanillaLayout(key.vanillaLayoutKind)
					? ResolveVanillaLayoutVertexShader(
						generation, key.vanillaLayoutKind)
					: generation.vertexShader.m_pObject;
			if (!vertexShader || !vertexShader->GetShaderHandle())
				return nullptr;
			NiDX9ShaderDeclaration* declaration =
				UsesNativeFontVanillaLayout(key.vanillaLayoutKind)
					? ResolveVanillaLayoutDeclaration(
						generation, key.vanillaLayoutKind)
					: generation.declaration.m_pObject;
			if (!declaration)
				return nullptr;

			BSShader* createdShader =
				CdeclCall<BSShader*>(kTileShaderCreateShader);
			NiPointer<TileShader> shaderGuard =
				static_cast<TileShader*>(createdShader);
			TileShader* shader = shaderGuard.m_pObject;
			if (!shader)
				return nullptr;
			NiD3DPass* pass = shader->spPasses[0].m_pObject;
			void** vanillaVtable = *reinterpret_cast<void***>(shader);
			if (!pass || pass->m_uiStageCount < 1 || !vanillaVtable)
				return nullptr;

			// Preserve the complete immutable private c176-c183 packet ABI.
			// COLOR0 now owns only the shared per-glyph base modifier, so
			// replacing c176 with the historical identity value would turn every
			// fixed effect layer white.
			auto profileConstants = packet.constants;
			if (key.vanillaLayoutKind
				== NativeFontVanillaLayoutKind::Uniform40)
				profileConstants[6] = packet.uniformSdfSpread;
			const float uniformSdfSpread = key.vanillaLayoutKind
				== NativeFontVanillaLayoutKind::Uniform40
					? packet.uniformSdfSpread : 0.0f;
			const float uniformDistanceParameterScale = key.vanillaLayoutKind
				== NativeFontVanillaLayoutKind::Uniform40
					? packet.uniformDistanceParameterScale : 1.0f;
			auto* profile = new NativeShaderProfile(generation, key,
				profileConstants, uniformSdfSpread,
				uniformDistanceParameterScale);
			profile->shader = shader;
			profile->effectPass =
				packet.shaderClass != NativeFontShaderClass::Composite
				&& packet.layer != 3;

			profile->shaderOwner = shaderGuard;
			shader->m_spShaderDecl = declaration;
			shader->spShaderDeclarations[0] = declaration;
			shader->spShaderDeclarations[1] = declaration;
			for (NiD3DVertexShaderPtr& slot : shader->spVertexShaders)
				slot = vertexShader;
			for (NiD3DPixelShaderPtr& slot : shader->spPixelShaders)
				slot = pixelShader;
			pass->m_spVertexShader = vertexShader;
			pass->m_spPixelShader = pixelShader;
			// The TileShader factory already pairs the base pass with spPasses[0].
			// Avoid an unnecessary NiD3DPass smart-pointer ref-count transition;
			// CommonLib intentionally does not link those pool ref-count wrappers.
			//

			ConfigureProfilePass(generation, *profile, *pass);

			auto* vtable = new NativeTileVtableBlock();
			vtable->profile = profile;
			vtable->rttiPrefix = vanillaVtable[-1];
			std::copy_n(vanillaVtable, vtable->slots.size(), vtable->slots.begin());
			vtable->vanillaSetupGeometryConstants =
				reinterpret_cast<VanillaSetupGeometryConstantsFn>(
					vanillaVtable[kSetupGeometryConstantsVtableSlot]);
			vtable->slots[kSetupGeometryConstantsVtableSlot] =
				reinterpret_cast<void*>(&NativeSetupGeometryConstants);
			// The native FreeType shader owns its blend contract. Global TileShader
			// vtables remain untouched, so vanilla and third-party geometry continue
			// through their live callback chains without becoming a native proof.
			vtable->slots[kSetupGeometryAlphaBlendingVtableSlot] =
				reinterpret_cast<void*>(&NativeSetupGeometryAlphaBlending);
			profile->vtable = vtable;
			*reinterpret_cast<void***>(shader) = vtable->slots.data();
			NativeFontCompiledPacketCommand& program =
				profile->retainedProgram;
			program.profile = profile;
			program.shader = shader;
			program.shaderVtable = vtable->slots.data();
			program.device = generation.device;
			program.vertexShader = vertexShader->GetShaderHandle();
			program.pixelShader = pixelShader->GetShaderHandle();
			program.prepareGeometryForRendering = vtable->slots[27];
			program.setupGeometryTextures = vtable->slots[30];
			program.setupGeometryConstants = vtable->slots[31];
			program.setupGeometryAlphaBlending =
				vtable->slots[kSetupGeometryAlphaBlendingVtableSlot];
			program.setupGeometryAlphaTesting = vtable->slots[33];
			program.setupGeometryRenderStates = vtable->slots[34];
			program.postGeometry = vtable->slots[35];
			program.setupNonFirstPass = vtable->slots[68];
			program.standardV2SlotProofs = 0;
			program.standardBlendSemantics =
				ClassifyStandardBlendCallback(
					program.setupGeometryAlphaBlending);
			if (program.setupGeometryTextures == reinterpret_cast<void*>(
					kTileShaderSetupGeometryTextures))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot30Proof;
			}
			if (vtable->vanillaSetupGeometryConstants
				== reinterpret_cast<VanillaSetupGeometryConstantsFn>(
					kTileShaderSetupGeometryConstants))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot31Proof;
			}
			if (HasPredictableNativeFontBlendSemantics(
					program.standardBlendSemantics))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot32Proof;
			}
			if (program.setupGeometryAlphaTesting == reinterpret_cast<void*>(
					kBSShaderSetupGeometryAlphaTesting))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot33Proof;
			}
			if (program.setupGeometryRenderStates == reinterpret_cast<void*>(
					kBSShaderSetupGeometryRenderStates))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot34Proof;
			}
			if (program.postGeometry == reinterpret_cast<void*>(
					kTileShaderPostGeometry))
			{
				program.standardV2SlotProofs |=
					NativeFontCompiledPacketCommand::kStandardSlot35Proof;
			}
			program.directDrawLiteReady =
				program.prepareGeometryForRendering == reinterpret_cast<void*>(
					kNiD3DShaderPrepareGeometryForRendering)
				&& vtable->slots[36] == reinterpret_cast<void*>(
					kNiD3DShaderFirstPass);
			program.generation = generation.id;
			program.simpleColor =
				packet.shaderClass == NativeFontShaderClass::Coverage
					|| packet.shaderClass == NativeFontShaderClass::Argb;
			program.active = program.device && program.vertexShader
				&& program.pixelShader && program.setupGeometryTextures;
			const UInt32 previousProofGeneration =
				ShaderState().standardV2ProofLogGeneration.exchange(
					generation.id, std::memory_order_relaxed);
			if (previousProofGeneration != generation.id)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: standard-v2 slot proofs generation=%u mask=%02X required=%02X blend=%s slot32=%p ready=%u directDrawLite=%u prepare=%p firstPass=%p",
					generation.id,
					static_cast<UInt32>(
						program.standardV2SlotProofs),
					static_cast<UInt32>(
						NativeFontCompiledPacketCommand::
							kStandardV2RequiredProofs),
					StandardBlendSemanticsName(
						program.standardBlendSemantics),
					program.setupGeometryAlphaBlending,
					program.standardV2SlotProofs
							== NativeFontCompiledPacketCommand::
								kStandardV2RequiredProofs
						? 1u : 0u,
					program.directDrawLiteReady ? 1u : 0u,
					program.prepareGeometryForRendering,
					vtable->slots[36]);
			}
			return profile;
		}

		bool CreateNativeDeclaration(NativeShaderGeneration& generation,
			const char*& failure)
		{
			// Runtime object size is 0x38. The 0xD4 CommonLib declaration includes
			// static lookup tables incorrectly; never allocate/copy it with sizeof.
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kNiDX9ShaderDeclarationCreate,
					generation.renderer, 5u, 1u);
			if (!declaration)
			{
				failure = "declaration-factory";
				return false;
			}

			using Parameter = NiShaderDeclaration::ShaderParameter;
			using ParameterType = NiShaderDeclaration::ShaderParameterType;
			const bool entriesReady =
				declaration->SetEntry(0, 0, Parameter::SHADERPARAM_NI_POSITION,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(1, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD0,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(2, 0,
					Parameter::SHADERPARAM_NI_COLOR,
					ParameterType::SPTYPE_UBYTECOLOR, 0)
				&& declaration->SetEntry(3, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD1,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(4, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD2,
					ParameterType::SPTYPE_FLOAT4, 0);
			if (!entriesReady)
			{
				failure = "declaration-entries";
				return false;
			}

			IDirect3DVertexDeclaration9* d3dDeclaration =
				declaration->GetD3DDeclaration();
			if (!d3dDeclaration)
			{
				D3DCAPS9 caps = {};
				const HRESULT capsResult = generation.device
					? generation.device->GetDeviceCaps(&caps) : E_POINTER;
				IDirect3DVertexDeclaration9* retryDeclaration = nullptr;
				const HRESULT retryResult = generation.device
					&& declaration->m_pkElements
					? generation.device->CreateVertexDeclaration(
						declaration->m_pkElements, &retryDeclaration)
					: E_POINTER;
				gLog.FormattedMessage(
					"tnvse_freetype_native: vertex declaration creation failed format=float4 stride=%u capsHr=0x%08X declTypes=0x%08X retryHr=0x%08X elements=%p",
					static_cast<UInt32>(sizeof(NativeFontGpuVertex)),
					static_cast<UInt32>(capsResult), caps.DeclTypes,
					static_cast<UInt32>(retryResult),
					declaration->m_pkElements);
				if (declaration->m_pkElements)
				{
					const D3DVERTEXELEMENT9* elements =
						declaration->m_pkElements;
					gLog.FormattedMessage(
						"tnvse_freetype_native: declaration elements e0=%u/%u/%u/%u/%u/%u e1=%u/%u/%u/%u/%u/%u e2=%u/%u/%u/%u/%u/%u e3=%u/%u/%u/%u/%u/%u e4=%u/%u/%u/%u/%u/%u",
						elements[0].Stream, elements[0].Offset,
						elements[0].Type, elements[0].Method,
						elements[0].Usage, elements[0].UsageIndex,
						elements[1].Stream, elements[1].Offset,
						elements[1].Type, elements[1].Method,
						elements[1].Usage, elements[1].UsageIndex,
						elements[2].Stream, elements[2].Offset,
						elements[2].Type, elements[2].Method,
						elements[2].Usage, elements[2].UsageIndex,
						elements[3].Stream, elements[3].Offset,
						elements[3].Type, elements[3].Method,
						elements[3].Usage, elements[3].UsageIndex,
						elements[4].Stream, elements[4].Offset,
						elements[4].Type, elements[4].Method,
						elements[4].Usage, elements[4].UsageIndex);
				}
				if (SUCCEEDED(retryResult) && retryDeclaration)
				{
					// GetD3DDeclaration hides the original HRESULT. A direct retry
					// with its generated element table can recover a transient
					// device failure while preserving Gamebryo ownership.
					declaration->m_hVertexDecl = retryDeclaration;
					declaration->m_bModified = false;
					d3dDeclaration = retryDeclaration;
				}
				else
				{
					if (retryDeclaration)
						retryDeclaration->Release();
					failure = "d3d-declaration";
					return false;
				}
			}
			generation.declaration = declaration;
			generation.d3dDeclaration = d3dDeclaration;
			return true;
		}

		bool CreateVanillaLayoutDeclaration(
			NativeShaderGeneration& generation, const char*& status)
		{
			if (!generation.renderer || !generation.device
				|| generation.renderer->m_kD3DCaps9.MaxVertexShaderConst
					<= kNativeFontVanillaLayoutGlyphConstantRegister)
			{
				status = "constant-registers";
				return false;
			}
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kNiDX9ShaderDeclarationCreate,
					generation.renderer, 5u, 1u);
			if (!declaration)
			{
				status = "declaration-factory";
				return false;
			}

			using Parameter = NiShaderDeclaration::ShaderParameter;
			using ParameterType = NiShaderDeclaration::ShaderParameterType;
			const bool entriesReady =
				declaration->SetEntry(0, 0,
					Parameter::SHADERPARAM_NI_POSITION,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(1, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD0,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(2, 0,
					Parameter::SHADERPARAM_NI_COLOR,
					ParameterType::SPTYPE_UBYTECOLOR, 0)
				&& declaration->SetEntry(3, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD1,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(4, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD2,
					ParameterType::SPTYPE_FLOAT2, 0);
			if (!entriesReady)
			{
				status = "declaration-entries";
				return false;
			}
			IDirect3DVertexDeclaration9* d3dDeclaration =
				declaration->GetD3DDeclaration();
			if (!d3dDeclaration)
			{
				status = "d3d-declaration";
				return false;
			}
			generation.vanillaLayoutDeclaration = declaration;
			generation.vanillaLayoutD3DDeclaration = d3dDeclaration;
			status = "ready";
			return true;
		}

		bool CreateVanillaParametricLayoutDeclaration(
			NativeShaderGeneration& generation, const char*& status)
		{
			if (!generation.renderer || !generation.device
				|| generation.renderer->m_kD3DCaps9.MaxVertexShaderConst
					<= kNativeFontVanillaLayoutGlyphConstantRegister)
			{
				status = "constant-registers";
				return false;
			}
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kNiDX9ShaderDeclarationCreate,
					generation.renderer, 6u, 1u);
			if (!declaration)
			{
				status = "declaration-factory";
				return false;
			}

			using Parameter = NiShaderDeclaration::ShaderParameter;
			using ParameterType = NiShaderDeclaration::ShaderParameterType;
			// Keep every additional texture semantic FLOAT2. Retail GetTextureSet
			// returns one NiPoint2 source for every requested set; wider source types
			// would make the asynchronous native pack read beyond that allocation.
			const bool entriesReady =
				declaration->SetEntry(0, 0,
					Parameter::SHADERPARAM_NI_POSITION,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(1, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD0,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(2, 0,
					Parameter::SHADERPARAM_NI_COLOR,
					ParameterType::SPTYPE_UBYTECOLOR, 0)
				&& declaration->SetEntry(3, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD1,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(4, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD2,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(5, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD3,
					ParameterType::SPTYPE_FLOAT2, 0);
			if (!entriesReady)
			{
				status = "declaration-entries";
				return false;
			}
			IDirect3DVertexDeclaration9* d3dDeclaration =
				declaration->GetD3DDeclaration();
			if (!d3dDeclaration)
			{
				status = "d3d-declaration";
				return false;
			}
			generation.vanillaParametricLayoutDeclaration = declaration;
			generation.vanillaParametricLayoutD3DDeclaration = d3dDeclaration;
			status = "ready";
			return true;
		}

		NativeShaderGeneration* BuildGeneration(CreateVertexShaderFn createVS,
			CreatePixelShaderFn createPS, NiDX9Renderer* renderer,
			IDirect3DDevice9* device, const char*& failure)
		{
			if (!createVS || !createPS || !renderer || !device)
			{
				failure = "loader-or-device";
				return nullptr;
			}

			auto generation = std::make_unique<NativeShaderGeneration>();
			generation->deviceEpoch = ShaderState().deviceEpoch.load(
				std::memory_order_acquire);
			generation->renderer = renderer;
			generation->device = device;
			generation->createPixelShader = createPS;
			generation->supportsSeparateAlpha =
				(renderer->m_kD3DCaps9.PrimitiveMiscCaps
					& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
			generation->distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();

			generation->vertexShader = createVS("tnvse_freetype_native_vs.vso");
			const bool vanillaLayoutRequested =
				IsVanillaLayoutEnabled(generation->distanceFieldMethod)
				&& (generation->distanceFieldMethod
						== DistanceFieldMethod::TrueSdf
					|| generation->distanceFieldMethod
						== DistanceFieldMethod::Mtsdf);
			if (vanillaLayoutRequested)
			{
				generation->vanillaLayoutVertexShader = createVS(
					"tnvse_freetype_native_vanilla_layout_vs.vso");
				generation->vanillaParametricLayoutVertexShader = createVS(
					"tnvse_freetype_native_vanilla_parametric_vs.vso");
			}
			if (UsesBakedEffectRoute())
			{
				generation->coverageShader =
					createPS("tnvse_freetype_native_coverage.pso");
				generation->argbShader =
					createPS("tnvse_freetype_native_argb.pso");
			}
			else
			{
				const char* mtsdfFillNames[] = {
					"tnvse_freetype_native_mtsdf_fill_fast.pso",
					"tnvse_freetype_native_mtsdf_fill_balanced.pso",
					"tnvse_freetype_native_mtsdf_fill_high.pso"
				};
				const char* trueSdfFillNames[] = {
					"tnvse_freetype_native_sdf_fill_fast.pso",
					"tnvse_freetype_native_sdf_fill_balanced.pso",
					"tnvse_freetype_native_sdf_fill_high.pso"
				};
				const char* const* fillNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfFillNames : trueSdfFillNames;
				for (size_t index = 0;
					index < generation->distanceFieldFillShaders.size(); ++index)
				{
					generation->distanceFieldFillShaders[index] =
						createPS(fillNames[index]);
				}
				const char* mtsdfEffectNames[] = {
					"tnvse_freetype_native_mtsdf_effects_fast.pso",
					"tnvse_freetype_native_mtsdf_effects_balanced.pso",
					"tnvse_freetype_native_mtsdf_effects_high.pso"
				};
				const char* trueSdfEffectNames[] = {
					"tnvse_freetype_native_sdf_effects_fast.pso",
					"tnvse_freetype_native_sdf_effects_balanced.pso",
					"tnvse_freetype_native_sdf_effects_high.pso"
				};
				const char* const* effectNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfEffectNames : trueSdfEffectNames;
				for (size_t index = 0;
					index < generation->effectShaders.size(); ++index)
				{
					generation->effectShaders[index] =
						createPS(effectNames[index]);
				}
				const char* mtsdfCompositeNames[] = {
					"tnvse_freetype_native_mtsdf_composite_fast.pso",
					"tnvse_freetype_native_mtsdf_composite_balanced.pso",
					"tnvse_freetype_native_mtsdf_composite_high.pso"
				};
				const char* trueSdfCompositeNames[] = {
					"tnvse_freetype_native_sdf_composite_fast.pso",
					"tnvse_freetype_native_sdf_composite_balanced.pso",
					"tnvse_freetype_native_sdf_composite_high.pso"
				};
				const char* const* compositeNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfCompositeNames : trueSdfCompositeNames;
				for (size_t index = 0;
					index < generation->compositeShaders.size(); ++index)
				{
					generation->compositeShaders[index] =
						createPS(compositeNames[index]);
				}
			}

			if (!HasShaderHandle(generation->vertexShader))
			{
				failure = "base-shader-set";
				return nullptr;
			}
			if (UsesBakedEffectRoute()
				&& (!HasShaderHandle(generation->coverageShader)
					|| !HasShaderHandle(generation->argbShader)))
			{
				// Treat an incomplete aggressive deployment exactly like a missing
				// Shader Loader. No A8 coverage facade may be published unless its
				// pixel program is present, so shape routing remains on the vanilla
				// ARGB32 TileShader fallback.
				failure = "aggressive-shader-set";
				return nullptr;
			}
			if (!UsesBakedEffectRoute())
			{
				for (const NiD3DPixelShaderPtr& shader
					: generation->distanceFieldFillShaders)
				{
					if (!HasShaderHandle(shader))
					{
						failure = "fill-shader-set";
						return nullptr;
					}
				}
				for (const NiD3DPixelShaderPtr& shader
					: generation->effectShaders)
				{
					if (!HasShaderHandle(shader))
					{
						failure = "effect-shader-set";
						return nullptr;
					}
				}
			}

			if (!CreateNativeDeclaration(*generation, failure))
				return nullptr;
			generation->vertexShader->m_hDecl = generation->d3dDeclaration;
			const char* vanillaLayoutStatus = vanillaLayoutRequested
				? "vertex-shader" : "not-applicable";
			if (vanillaLayoutRequested
				&& HasShaderHandle(generation->vanillaLayoutVertexShader)
				&& CreateVanillaLayoutDeclaration(
					*generation, vanillaLayoutStatus))
			{
				generation->vanillaLayoutVertexShader->m_hDecl =
					generation->vanillaLayoutD3DDeclaration;
				generation->vanillaLayoutReady = true;
				for (NativeShaderGeneration* previous : ShaderState().processGenerations)
				{
					if (!previous || !previous->vanillaLayoutReady
						|| previous->deviceEpoch != generation->deviceEpoch
						|| previous->renderer != renderer
						|| previous->device != device
						|| !previous->vanillaLayoutD3DDeclaration)
					{
						continue;
					}
					IDirect3DVertexDeclaration9* declaration =
						previous->vanillaLayoutD3DDeclaration;
					if (declaration != generation->vanillaLayoutD3DDeclaration
						&& std::find(
							generation->compatibleVanillaLayoutD3DDeclarations.begin(),
							generation->compatibleVanillaLayoutD3DDeclarations.end(),
							declaration)
							== generation->compatibleVanillaLayoutD3DDeclarations.end())
					{
						generation->compatibleVanillaLayoutD3DDeclarations.push_back(
							declaration);
					}
				}
			}
			const char* vanillaParametricLayoutStatus = vanillaLayoutRequested
				? "vertex-shader" : "not-applicable";
			if (vanillaLayoutRequested
				&& HasShaderHandle(
					generation->vanillaParametricLayoutVertexShader)
				&& CreateVanillaParametricLayoutDeclaration(
					*generation, vanillaParametricLayoutStatus))
			{
				generation->vanillaParametricLayoutVertexShader->m_hDecl =
					generation->vanillaParametricLayoutD3DDeclaration;
				generation->vanillaParametricLayoutReady = true;
				for (NativeShaderGeneration* previous : ShaderState().processGenerations)
				{
					if (!previous || !previous->vanillaParametricLayoutReady
						|| previous->deviceEpoch != generation->deviceEpoch
						|| previous->renderer != renderer
						|| previous->device != device
						|| !previous->vanillaParametricLayoutD3DDeclaration)
					{
						continue;
					}
					IDirect3DVertexDeclaration9* declaration =
						previous->vanillaParametricLayoutD3DDeclaration;
					if (declaration
							!= generation->vanillaParametricLayoutD3DDeclaration
						&& std::find(generation->
								compatibleVanillaParametricLayoutD3DDeclarations.begin(),
							generation->
								compatibleVanillaParametricLayoutD3DDeclarations.end(),
							declaration)
							== generation->
								compatibleVanillaParametricLayoutD3DDeclarations.end())
					{
						generation->
							compatibleVanillaParametricLayoutD3DDeclarations.push_back(
								declaration);
					}
				}
			}
			gLog.FormattedMessage(
				"tnvse_freetype_native: vanilla-layout distance-field generation uniformStatus=%s uniformReady=%u uniformStride=%u uniformDeclaration=%p uniformCompatiblePrevious=%u parametricStatus=%s parametricReady=%u parametricStride=%u parametricDeclaration=%p parametricCompatiblePrevious=%u vertexConstants=c%u-c%u deviceEpoch=%u",
				vanillaLayoutStatus,
				generation->vanillaLayoutReady ? 1u : 0u,
				kVanillaLayoutVertexStride,
				generation->vanillaLayoutD3DDeclaration,
				static_cast<UInt32>(
					generation->compatibleVanillaLayoutD3DDeclarations.size()),
				vanillaParametricLayoutStatus,
				generation->vanillaParametricLayoutReady ? 1u : 0u,
				kVanillaParametricVertexStride,
				generation->vanillaParametricLayoutD3DDeclaration,
				static_cast<UInt32>(generation->
					compatibleVanillaParametricLayoutD3DDeclarations.size()),
				kNativeFontVertexAaConstantRegister,
				kNativeFontVanillaLayoutGlyphConstantRegister,
				generation->deviceEpoch);
			return generation.release();
		}

		bool ResolveShaderLoader(CreateVertexShaderFn& createVS,
			CreatePixelShaderFn& createPS, const char*& failure)
		{
			createVS = nullptr;
			createPS = nullptr;
			if (!g_cmdTableInterface
				|| !g_cmdTableInterface->GetPluginInfoByDLLName)
			{
				failure = "nvse-plugin-query";
				return false;
			}
			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
				dependencies::kShaderLoaderDllName);
			if (!dependencies::IsPluginInfoValid(info)
				|| info->version < dependencies::kShaderLoaderMinVersion)
			{
				failure = "shader-loader-version";
				return false;
			}
			HMODULE module = GetModuleHandleA(dependencies::kShaderLoaderDllName);
			if (!module)
			{
				failure = "shader-loader-module";
				return false;
			}
			createVS = reinterpret_cast<CreateVertexShaderFn>(
				GetProcAddress(module, "CreateVertexShader"));
			createPS = reinterpret_cast<CreatePixelShaderFn>(
				GetProcAddress(module, "CreatePixelShader"));
			if (!createVS || !createPS)
			{
				failure = "shader-loader-exports";
				return false;
			}
			return true;
		}

		bool NativeRendererResetCallback(bool beforeReset, void*)
		{
			if (beforeReset)
			{
				ShaderState().resetSequence.fetch_add(
					1u, std::memory_order_acq_rel);
				ShaderState().resetPhaseEnteredAt.store(
					GetTickCount64(), std::memory_order_release);
				ShaderState().resetPhase.store(
					NativeRendererResetDiagnosticPhase::ReleasingResources,
					std::memory_order_release);
				ShaderState().resetLifecycle.Begin();
				const UInt32 deviceEpoch = ShaderState().deviceEpoch.fetch_add(
					1u, std::memory_order_acq_rel) + 1u;
				ResetFreeTypeGpuTiming();
				InvalidateNativeFontRingResources(
					NativeFontFallbackReason::DeviceReset);
				NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
					std::memory_order_acquire);
				if (current)
				{
					current->runtimeFault.store(true, std::memory_order_release);
					NotifyNativeFontCommandExternalMutation(
						NativeFontCommandFallback::Generation);
				}
				gLog.FormattedMessage(
					"tnvse_freetype_native: generation-invalidated reason=device-reset generation=%u deviceEpoch=%u phase=release; dynamic VB/IB ring released",
					current ? current->id : 0, deviceEpoch);
				ShaderState().resetPhaseEnteredAt.store(
					GetTickCount64(), std::memory_order_release);
				ShaderState().resetPhase.store(
					NativeRendererResetDiagnosticPhase::AwaitingDeviceReset,
					std::memory_order_release);
				return true;
			}

			ShaderState().resetPhaseEnteredAt.store(
				GetTickCount64(), std::memory_order_release);
			ShaderState().resetPhase.store(
				NativeRendererResetDiagnosticPhase::RebuildingResources,
				std::memory_order_release);
			ShaderState().resetLifecycle.Complete();
			if (!InitializeNativeFontRenderer(true, true))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: initialization unavailable reason=device-reset phase=rebuild; no complete native generation available");
			}
			ShaderState().resetPhaseEnteredAt.store(
				GetTickCount64(), std::memory_order_release);
			ShaderState().resetPhase.store(
				NativeRendererResetDiagnosticPhase::Complete,
				std::memory_order_release);
			return true;
		}
	}

}
