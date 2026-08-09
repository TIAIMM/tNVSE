#include "font_native_shape_internal.h"

#include "hook_identity.h"
#include "load_config.h"
#include "tnvse.h"

#include "NiD3DRenderState.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9RenderState.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		inline constexpr UInt32 kBSBatchRendererRenderPassImmediatelyStandard =
			0xB98E80;
		inline constexpr UInt32 kBSBatchRendererBeginPass = 0xB99390;
		inline constexpr UInt32 kBSRenderStateSetAlphaToCoverageEnable = 0xB98540;
		inline constexpr UInt32 kNiDX9RendererIsHardwareSkinned = 0xE72C20;
		inline constexpr UInt32 kBSBatchRendererPassSuppressesBlendAlpha = 0xB630F0;
		inline constexpr UInt32 kTileImageSetSourceTexture = 0xBB7A10;
		inline constexpr UInt32 kNiTriShapeOnlyRenderImmediate = 0xA74600;
		inline constexpr UInt32 kBSShaderManager_pCurrentRenderPass = 0x11F91E0;
		inline constexpr UInt32 kBSShaderManager_eCurrentPass = 0x11F91E4;
		inline constexpr UInt32 kBSBatchRenderer_uiLastPass = 0x11FFE30;
		inline constexpr UInt32 kBSBatchRenderer_pLastShader = 0x11FFE2C;
		inline constexpr UInt32 kBSShaderManager_bTransparencyMultisampling =
			0x11F9421;
		inline constexpr UInt32 kBSBatchRenderer_bFirstPass = 0x11AD8EC;
		inline constexpr UInt32 kBSShaderManager_pRenderer = 0x11F9508;
		inline constexpr UInt32 kForcedShaderSelectionPass = 758;
		inline constexpr UInt32 kGeometrySegmentedPredicateSlot = 10;
		inline constexpr UInt32 kGeometryResizablePredicateSlot = 11;
		inline constexpr UInt32 kGeometrySpecialPredicateSlot = 12;
		inline constexpr UInt32 kGeometryAlternatePredicateSlot = 13;
		// Official NiTriShape vtable slots 12/13 both point at ACBB70,
		// the shared predicate thunk that returns false. E68810 is instead
		// the positive cast thunk used by slots 6/7/9 and returns this.
		inline constexpr UInt32 kNiObjectNullGeometryCastPredicate = 0xACBB70;
		const hook_identity::Rel32InstructionImage
			s_renderPassImmediatelyHookImage =
				hook_identity::MakeRel32InstructionImage(
					kRenderPassImmediatelyCallSite,
					hook_identity::Rel32Opcode::Call,
					reinterpret_cast<SIZE_T>(&NativeFontRenderPassImmediately));

		struct NativeFontMetadataHotEntry
		{
			const NiTriShape* shape = nullptr;
			UInt64 generation = 0;
			std::weak_ptr<const NativeFontShapeMetadata> metadata;
		};

		inline constexpr size_t kMetadataHotWayCount = 8;
		inline constexpr size_t kMetadataHotSetCount = 2048;
		static_assert((kMetadataHotSetCount & (kMetadataHotSetCount - 1)) == 0,
			"metadata hot-cache set count must remain a power of two");

		struct NativeFontMetadataHotSet
		{
			std::array<NativeFontMetadataHotEntry, kMetadataHotWayCount> ways;
			UInt8 nextVictim = 0;
		};

		// Eight ways keep allocator-neighbouring facades from evicting one another,
		// while weak ownership preserves menu/shape destruction semantics. 16384
		// entries cover the observed multi-page Pip-Boy working set without adding
		// any process-wide locks to the render path.
		thread_local std::unique_ptr<
			std::array<NativeFontMetadataHotSet, kMetadataHotSetCount>>
			s_metadataHotSets;

		NativeFontMetadataHotSet& GetMetadataHotSet(const NiTriShape* shape)
		{
			if (!s_metadataHotSets)
			{
				s_metadataHotSets = std::make_unique<
					std::array<NativeFontMetadataHotSet, kMetadataHotSetCount>>();
			}
			const size_t index = HashMetadataShapeAddress(shape)
				& (kMetadataHotSetCount - 1);
			return (*s_metadataHotSets)[index];
		}

		NativeFontMetadataHotEntry& SelectMetadataHotVictim(
			NativeFontMetadataHotSet& set)
		{
			for (NativeFontMetadataHotEntry& entry : set.ways)
			{
				if (!entry.shape)
					return entry;
			}
			for (NativeFontMetadataHotEntry& entry : set.ways)
			{
				if (entry.metadata.expired())
				{
					entry = {};
					return entry;
				}
			}
			NativeFontMetadataHotEntry& victim =
				set.ways[set.nextVictim % kMetadataHotWayCount];
			set.nextVictim = static_cast<UInt8>(
				(set.nextVictim + 1) % kMetadataHotWayCount);
			victim = {};
			return victim;
		}

		// D3D9 shader constants are draw-program inputs rather than renderer state
		// that must survive a pass boundary. The test-build symbols prove that
		// RenderPassImmediately_Standard calls SetupGeometryConstants before every
		// geometry draw. NiD3DShaderConstantMap::SetShaderConstants then submits
		// each live reflected entry. TileShader publishes PS c0 and VS c0-c4;
		// tNVSE uses the non-overlapping SM3 reserved ranges PS c176-c183 and
		// VS c208.
		// Shipped constant tables, including relative arrays, end at PS c24 and
		// VS c120; shader-local pixel literals extend only to c30.
		//
		// Treat the native range as pass-local ownership instead of snapshotting
		// and restoring the global D3D register file. This removes one pixel and
		// one vertex Get* plus their matching Set* calls per isolation segment.
		class NativePassConstantScope
		{
		public:
			explicit NativePassConstantScope(IDirect3DDevice9* device)
			{
				if (!device)
				{
					m_result = D3DERR_INVALIDCALL;
					m_operation = "acquire-pass-constant-ownership";
					return;
				}
				m_owned = true;
				m_result = D3D_OK;
				m_operation = "none";
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantOwnershipSegment);
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantSnapshotGetElided, 2);
			}

			~NativePassConstantScope()
			{
				if (m_owned && !m_released)
					Release();
			}

			bool Owned() const { return m_owned; }
			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }

			bool Release()
			{
				if (!m_owned)
					return false;
				if (m_released)
					return true;
				m_released = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantOwnershipRelease);
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantRestoreSetElided, 2);
				m_operation = "none";
				m_result = D3D_OK;
				return true;
			}

		private:
			HRESULT m_result = D3DERR_INVALIDCALL;
			const char* m_operation = "acquire-pass-constant-ownership";
			SInt32 m_mismatchRegister = -1;
			bool m_owned = false;
			bool m_released = false;
		};

		class NativePassConstantBatch
		{
		public:
			void BeginFrame()
			{
				if (m_owned)
					Release();
				m_frameActive = true;
			}

			bool FrameActive() const
			{
				return m_frameActive;
			}

			bool EnsureOwned(IDirect3DDevice9* device)
			{
				if (!m_frameActive || !device)
					return SetFailure("acquire-pass-constant-ownership",
						D3DERR_INVALIDCALL);
				if (m_owned)
				{
					if (m_device == device)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::ConstantOwnershipReuse);
						return true;
					}
					if (!Release())
						return false;
				}
				return Acquire(device);
			}

			bool Release()
			{
				if (!m_owned)
					return true;
				m_owned = false;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantOwnershipRelease);
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantRestoreSetElided, 2);
				return ResetAfterRelease();
			}

			void EndFrame()
			{
				Release();
				m_frameActive = false;
			}

			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }
			UInt32 Generation() const { return m_generation; }

		private:
			bool Acquire(IDirect3DDevice9* device)
			{
				m_device = device;
				m_generation = GetNativeFontShaderGeneration();
				m_mismatchRegister = -1;
				m_owned = true;
				m_result = D3D_OK;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantOwnershipSegment);
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantSnapshotGetElided, 2);
				m_operation = "none";
				return true;
			}

			bool ResetAfterRelease()
			{
				m_device = nullptr;
				m_operation = "none";
				m_result = D3D_OK;
				m_mismatchRegister = -1;
				return true;
			}

			bool SetFailure(const char* operation, HRESULT result)
			{
				m_operation = operation;
				m_result = result;
				m_device = nullptr;
				m_owned = false;
				return false;
			}

			IDirect3DDevice9* m_device = nullptr;
			HRESULT m_result = D3D_OK;
			const char* m_operation = "none";
			SInt32 m_mismatchRegister = -1;
			UInt32 m_generation = 0;
			bool m_frameActive = false;
			bool m_owned = false;
		};

		thread_local NativePassConstantBatch s_constantOwnershipBatch;

		bool ReleaseNativeConstantOwnershipBatch(const char* phase)
		{
			if (s_constantOwnershipBatch.Release())
				return true;
			MarkNativeFontGenerationFault(s_constantOwnershipBatch.Generation(),
				s_constantOwnershipBatch.Operation(),
				s_constantOwnershipBatch.Result());
			gLog.FormattedMessage(
				"tnvse_freetype_native: pass-constant ownership release fault phase=%s operation=%s hr=0x%08X register=%d generation=%u",
				phase ? phase : "unknown",
				s_constantOwnershipBatch.Operation(),
				static_cast<UInt32>(s_constantOwnershipBatch.Result()),
				s_constantOwnershipBatch.MismatchRegister(),
				s_constantOwnershipBatch.Generation());
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
				EndNativeFontRingSubmission(submission);
			}

			NativeFontRingSubmission submission;
		};

		class NativeDirectShapeSubmissionScope
		{
		public:
			~NativeDirectShapeSubmissionScope()
			{
				EndNativeFontDirectShapeSubmission(submission);
			}

			NativeFontDirectShapeSubmission submission;
		};

		using NativeImmediateContinuationFn =
			bool(*)(void*, NiRenderer*, bool);
		struct NativeDirectDrawLiteSubmission;

		enum class NativeImmediateCommandKind : UInt8
		{
			None = 0,
			SpanPacket,
			SinglePacket,
			DirectFacadeSinglePacket
		};

		struct NativeDirectImmediateContext
		{
			NiTriShape* shape = nullptr;
			void* continuation = nullptr;
			NativeImmediateContinuationFn continueImmediate = nullptr;
			UInt32 commandSpanIndex = kInvalidNativeFontCommandIndex;
			UInt32 commandOffset = kInvalidNativeFontCommandIndex;
			NativeImmediateCommandKind commandKind =
				NativeImmediateCommandKind::None;
			bool strictValidation = false;
			bool packetStatePrevalidated = false;
			bool invoked = false;
			bool validationPassed = true;
			bool drew = false;
			bool continuationSucceeded = true;
			const NativeDirectDrawLiteSubmission* directDrawLite = nullptr;
		};

		thread_local NativeDirectImmediateContext*
			s_nativeDirectImmediateContext = nullptr;

		class NativeDirectImmediateScope
		{
		public:
			explicit NativeDirectImmediateScope(NiTriShape* shape,
				UInt32 commandSpanIndex = kInvalidNativeFontCommandIndex,
				UInt32 commandOffset = kInvalidNativeFontCommandIndex,
				bool strictValidation = false,
				void* continuation = nullptr,
				NativeImmediateContinuationFn continueImmediate = nullptr,
				NativeImmediateCommandKind commandKind =
					NativeImmediateCommandKind::SpanPacket,
				bool packetStatePrevalidated = false)
				: m_previous(s_nativeDirectImmediateContext)
			{
				m_context.shape = shape;
				m_context.commandSpanIndex = commandSpanIndex;
				m_context.commandOffset = commandOffset;
				m_context.commandKind = commandSpanIndex
						!= kInvalidNativeFontCommandIndex
					? commandKind
					: NativeImmediateCommandKind::None;
				m_context.strictValidation = strictValidation;
				m_context.packetStatePrevalidated =
					packetStatePrevalidated;
				m_context.continuation = continuation;
				m_context.continueImmediate = continueImmediate;
				s_nativeDirectImmediateContext = &m_context;
			}

			~NativeDirectImmediateScope()
			{
				s_nativeDirectImmediateContext = m_previous;
			}

			bool Invoked() const
			{
				return m_context.invoked;
			}

			bool ValidationPassed() const
			{
				return m_context.validationPassed;
			}

			bool Drew() const
			{
				return m_context.drew;
			}

			bool ContinuationSucceeded() const
			{
				return m_context.continuationSucceeded;
			}

		private:
			NativeDirectImmediateContext m_context;
			NativeDirectImmediateContext* m_previous = nullptr;
		};

		bool ValidateNativeImmediateCommand(
			const NativeDirectImmediateContext& context,
			NiTriShape* shape, NiRenderer* renderer)
		{
			if (context.packetStatePrevalidated)
			{
				switch (context.commandKind)
				{
				case NativeImmediateCommandKind::SpanPacket:
					return context.commandOffset
							!= kInvalidNativeFontCommandIndex
						&& GuardNativeFontCommand(
							context.commandSpanIndex,
							context.commandOffset, shape, renderer);
				case NativeImmediateCommandKind::SinglePacket:
					return GuardNativeFontSinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				case NativeImmediateCommandKind::DirectFacadeSinglePacket:
					return GuardNativeFontDirectFacadeSinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				default:
					return true;
				}
			}
			switch (context.commandKind)
			{
			case NativeImmediateCommandKind::SpanPacket:
				return context.commandOffset
						!= kInvalidNativeFontCommandIndex
					&& ValidateNativeFontCommand(
						context.commandSpanIndex,
						context.commandOffset, shape, renderer);
			case NativeImmediateCommandKind::SinglePacket:
				return ValidateNativeFontSinglePacketCommand(
					context.commandSpanIndex, shape, renderer);
			case NativeImmediateCommandKind::DirectFacadeSinglePacket:
				return ValidateNativeFontDirectFacadeSinglePacketCommand(
					context.commandSpanIndex, shape, renderer);
			default:
				return true;
			}
		}

		class NativeImmediateHookVtableScope
		{
		public:
			explicit NativeImmediateHookVtableScope(NiTriShape* shape)
				: m_shape(shape)
			{
				if (!m_shape || !State().originalRenderImmediate
					|| !State().originalRenderImmediateAlt)
				{
					return;
				}
				m_original = *reinterpret_cast<void***>(m_shape);
				void** hook = &State().triShapeVtable[1];
				if (!m_original || !hook)
					return;
				if (m_original != hook)
				{
					*reinterpret_cast<void***>(m_shape) = hook;
					m_changed = true;
				}
				m_active =
					*reinterpret_cast<void***>(m_shape) == hook;
			}

			~NativeImmediateHookVtableScope()
			{
				if (m_changed && m_shape)
					*reinterpret_cast<void***>(m_shape) = m_original;
			}

			bool Active() const
			{
				return m_active;
			}

		private:
			NiTriShape* m_shape = nullptr;
			void** m_original = nullptr;
			bool m_changed = false;
			bool m_active = false;
		};

		class VanillaLayoutOriginalVtableScope
		{
		public:
			explicit VanillaLayoutOriginalVtableScope(NiTriShape* shape)
				: m_shape(shape)
			{
				NativeFontShapeState& state = State();
				if (!m_shape || !state.originalTriShapeVtable)
					return;
				m_original = *reinterpret_cast<void***>(m_shape);
				if (m_original != &state.vanillaLayoutTriShapeVtable[1])
					return;
				*reinterpret_cast<void***>(m_shape) =
					state.originalTriShapeVtable;
				m_changed = true;
				m_active = true;
			}

			~VanillaLayoutOriginalVtableScope()
			{
				if (m_changed && m_shape)
					*reinterpret_cast<void***>(m_shape) = m_original;
			}

			bool Active() const
			{
				return m_active;
			}

		private:
			NiTriShape* m_shape = nullptr;
			void** m_original = nullptr;
			bool m_changed = false;
			bool m_active = false;
		};

		class NativeFacadeShaderBatchScope
		{
		public:
			NativeFacadeShaderBatchScope()
			{
				BeginNativeFontFacadeShaderBatch();
			}

			~NativeFacadeShaderBatchScope()
			{
				EndNativeFontFacadeShaderBatch();
			}
		};

		void ApplyNativeGeometryOrigin(NiTransform& destination,
			const NiTransform& source, const NiPoint3& origin)
		{
			destination = source;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				destination.m_Translate = source * origin;
		}

		struct DirectTileShaderPropertyView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			bool byte90 = false;
			bool rotates = false;
			bool hasVertexColors = false;
			bool noTexture = false;
			BSStringT<char> texturePath;
			RECT scissorRect = {};
			bool useScissorTest = false;
		};
		static_assert(sizeof(DirectTileShaderPropertyView) == 0xB0);
		static_assert(offsetof(
			DirectTileShaderPropertyView, fAlpha) == 0x28);
		static_assert(offsetof(
			DirectTileShaderPropertyView, fFadeAlpha) == 0x2C);
		static_assert(offsetof(
			DirectTileShaderPropertyView, sourceTexture) == 0x60);
		static_assert(offsetof(
			DirectTileShaderPropertyView, alphaTexture) == 0x64);
		static_assert(offsetof(
			DirectTileShaderPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(
			DirectTileShaderPropertyView, tileAlpha) == 0x78);
		static_assert(offsetof(
			DirectTileShaderPropertyView, textureTransform) == 0x7C);
		static_assert(offsetof(
			DirectTileShaderPropertyView, clampMode) == 0x8C);
		static_assert(offsetof(
			DirectTileShaderPropertyView, byte90) == 0x90);
		static_assert(offsetof(
			DirectTileShaderPropertyView, rotates) == 0x91);
		static_assert(offsetof(
			DirectTileShaderPropertyView, hasVertexColors) == 0x92);
		static_assert(offsetof(
			DirectTileShaderPropertyView, noTexture) == 0x93);
		static_assert(offsetof(
			DirectTileShaderPropertyView, scissorRect) == 0x9C);
		static_assert(offsetof(
			DirectTileShaderPropertyView, useScissorTest) == 0xAC);
		// B98E80 tests byte [RenderPass+7] before slots 32-34. Keep the
		// first-pass predicate tied to the reverse-verified field rather than
		// the adjacent no-fog byte.
		static_assert(offsetof(
			BSShaderProperty::RenderPass, bIsFirst) == 0x07);
		static_assert(offsetof(
			BSShaderProperty::RenderPass, bNoFog) == 0x08);

		DirectTileShaderPropertyView* GetDirectTileProperty(
			NiTriShape* shape)
		{
			NiShadeProperty* shade =
				shape ? shape->GetShadeProperty() : nullptr;
			return shade
					&& shade->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<DirectTileShaderPropertyView*>(shade)
				: nullptr;
		}

		struct NativeSegmentPassStateKey
		{
			const NativeFontCompiledPacketCommand* program = nullptr;
			const NiTexture* sourceTexture = nullptr;
			const NiTexture* alphaTexture = nullptr;
			const void* atlasTexture = nullptr;
			NiTexturingProperty::ClampMode effectiveClampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			UInt8 textureModeFlags = 0;
		};

		using NativeSegmentBlendStateKey = NativeFontBlendState;

		struct NativeSegmentAlphaTestStateKey
		{
			UInt8 testFunction = 0;
			UInt8 alphaTestRef = 0;
		};

		struct NativeSegmentRenderStatesKey
		{
			bool drawBoth = false;
			bool alphaTestEnabled = false;
		};

		struct NativeSegmentGeometryBindingKey
		{
			IDirect3DVertexDeclaration9* declaration = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			UInt32 stride = 0;
		};

		// Slot 31's constant prefix can be retained independently of its paired
		// transient suffix. The key intentionally stores the exact
		// vanilla inputs rather than derived WVP/color values: identical inputs
		// preserve both the constant-map output and SetModelTransform's renderer
		// mirror side effects without doing floating-point work in the hot path.
		struct NativeSegmentConstantsStateKey
		{
			const NativeFontCompiledPacketCommand* program = nullptr;
			NiTransform world;
			D3DXMATRIX view;
			D3DXMATRIX projection;
			D3DXMATRIX viewProjection;
			NiPoint3 cameraRight;
			NiPoint3 cameraUp;
			NiColorA overlayColor;
			NiPoint4 textureTransform;
			float tileAlpha = 1.0f;
			float materialAlpha = 1.0f;
			float nearDepth = 0.0f;
			float depthRange = 0.0f;
			bool rotates = false;
		};

		struct NativeSegmentDeviceStateCache
		{
			NativeFontSegmentDeviceStateStamp stamp;
			NativeSegmentPassStateKey pass;
			NativeSegmentConstantsStateKey constants;
			NativeSegmentBlendStateKey blend;
			NativeSegmentAlphaTestStateKey alphaTest;
			NativeSegmentRenderStatesKey renderStates;
			NativeSegmentGeometryBindingKey geometryBinding;
			bool stampReady = false;
			bool passReady = false;
			bool constantsReady = false;
			bool blendReady = false;
			bool alphaTestReady = false;
			bool renderStatesReady = false;
			bool geometryBindingReady = false;

			void Reset()
			{
				stamp = {};
				pass = {};
				constants = {};
				blend = {};
				alphaTest = {};
				renderStates = {};
				geometryBinding = {};
				stampReady = false;
				passReady = false;
				constantsReady = false;
				blendReady = false;
				alphaTestReady = false;
				renderStatesReady = false;
				geometryBindingReady = false;
			}

			void InvalidateStates()
			{
				passReady = false;
				constantsReady = false;
				blendReady = false;
				alphaTestReady = false;
				renderStatesReady = false;
			}
		};

		thread_local NativeSegmentDeviceStateCache
			s_segmentDeviceStateCache;

		bool SameSegmentDeviceStateStamp(
			const NativeFontSegmentDeviceStateStamp& left,
			const NativeFontSegmentDeviceStateStamp& right)
		{
			return left.ready && right.ready
				&& left.renderer == right.renderer
				&& left.device == right.device
				&& left.renderTargetGroup == right.renderTargetGroup
				&& left.validationToken == right.validationToken
				&& left.generation == right.generation
				&& left.atlasTextureEpoch == right.atlasTextureEpoch
				&& left.resourceSerial == right.resourceSerial
				&& left.uploadEpoch == right.uploadEpoch
				&& left.executionSegmentEpoch
					== right.executionSegmentEpoch
				&& left.externalMutationEpoch
					== right.externalMutationEpoch
				&& std::memcmp(&left.viewport, &right.viewport,
					sizeof(left.viewport)) == 0;
		}

		NativeSegmentDeviceStateCache*
			EnterSegmentDeviceStateCache(
				const NativeFontSegmentDeviceStateStamp* stamp)
		{
			NativeSegmentDeviceStateCache& cache =
				s_segmentDeviceStateCache;
			if (!stamp || !stamp->ready || !stamp->renderer
				|| !stamp->device
				|| !stamp->renderTargetGroup
				|| !stamp->validationToken || !stamp->generation
				|| !stamp->atlasTextureEpoch
				|| !stamp->resourceSerial
				|| !stamp->executionSegmentEpoch
				|| !stamp->externalMutationEpoch
				|| !stamp->viewport.Width
				|| !stamp->viewport.Height)
			{
				cache.Reset();
				return nullptr;
			}
			if (!cache.stampReady
				|| !SameSegmentDeviceStateStamp(cache.stamp, *stamp))
			{
				cache.Reset();
				cache.stamp = *stamp;
				cache.stampReady = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDeviceStateStart);
			}
			else
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDeviceStateReuse);
			}
			return &cache;
		}

		void InvalidateSegmentDeviceStateCache()
		{
			s_segmentDeviceStateCache.Reset();
		}

		bool SameSegmentPassState(
			const NativeSegmentPassStateKey& left,
			const NativeSegmentPassStateKey& right)
		{
			return left.program == right.program
				&& left.sourceTexture == right.sourceTexture
				&& left.alphaTexture == right.alphaTexture
				&& left.atlasTexture == right.atlasTexture
				&& left.effectiveClampMode
					== right.effectiveClampMode
				&& left.textureModeFlags
					== right.textureModeFlags;
		}

		bool BuildSegmentPassStateKey(NiTriShape* geometry,
			const NativeFontDrawCommand& command,
			NativeSegmentPassStateKey& key)
		{
			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!tile || !command.program || !command.atlasTexture
				|| !tile->sourceTexture.m_pObject
				|| tile->alphaTexture.m_pObject
				|| tile->noTexture)
			{
				return false;
			}
			key = {};
			key.program = command.program;
			key.sourceTexture = tile->sourceTexture.m_pObject;
			key.alphaTexture = tile->alphaTexture.m_pObject;
			key.atlasTexture = command.atlasTexture;
			// Official TileShader::SetupGeometryTextures (BCA760) reads
			// rotates, hasVertexColors, noTexture, both texture pointers and
			// clampMode. byte90 and every RenderPass flag are irrelevant.
			key.effectiveClampMode = tile->rotates
				? NiTexturingProperty::WRAP_S_WRAP_T
				: tile->clampMode;
			key.textureModeFlags =
				(tile->rotates ? 1u : 0u)
				| (tile->hasVertexColors ? 2u : 0u);
			return true;
		}

		bool BuildSegmentConstantsStateKey(NiTriShape* geometry,
			NiDX9Renderer* renderer,
			const NativeFontDrawCommand& command,
			NativeSegmentConstantsStateKey& key,
			bool& cleanupRequired)
		{
			key = {};
			cleanupRequired = true;
			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!geometry || !renderer || !tile || !command.program)
				return false;

			const NiStencilProperty* stencil =
				geometry->GetStencilProperty();
			cleanupRequired = tile->useScissorTest
				|| (stencil && stencil->IsEnabled());
			const NiMaterialProperty* material =
				geometry->GetMaterialProperty();
			key.program = command.program;
			key.world = geometry->m_kWorld;
			key.view = renderer->m_kD3DView;
			key.projection = renderer->m_kD3DProj;
			key.viewProjection = renderer->m_kViewProj;
			key.cameraRight = renderer->m_kCamRight;
			key.cameraUp = renderer->m_kCamUp;
			key.overlayColor = tile->overlayColor;
			key.tileAlpha = tile->tileAlpha;
			key.materialAlpha = material ? material->m_fAlpha : 1.0f;
			key.nearDepth = renderer->m_fNearDepth;
			key.depthRange = renderer->m_fDepthRange;
			key.rotates = tile->rotates;
			if (tile->rotates)
				key.textureTransform = tile->textureTransform;
			return true;
		}

		enum class NativeSegmentConstantsStateRelation : UInt8
		{
			Different = 0,
			Exact,
			TranslationOnly
		};

		NativeSegmentConstantsStateRelation CompareSegmentConstantsState(
			const NativeSegmentConstantsStateKey& left,
			const NativeSegmentConstantsStateKey& right)
		{
			// Preserve the original short-circuit order and record exactly one
			// diagnostic per failed comparison. This identifies the first field
			// blocking reuse. Translation-only world changes continue through the
			// remaining fields because the light path must prove that no later input
			// changed, but they retain world as the first-mismatch diagnostic.
			if (left.program != right.program)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchProgram);
				return NativeSegmentConstantsStateRelation::Different;
			}
			if (left.rotates != right.rotates)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchRotates);
				return NativeSegmentConstantsStateRelation::Different;
			}
			bool translationOnly = false;
			if (std::memcmp(&left.world, &right.world,
				sizeof(left.world)) != 0)
			{
				// Classify the complete transform with one counter write. The
				// seven buckets retain overlap information while avoiding as many
				// as three relaxed atomic increments for one failed key comparison.
				UInt8 mismatchMask = 0;
				if (std::memcmp(&left.world.m_Rotate,
					&right.world.m_Rotate,
					sizeof(left.world.m_Rotate)) != 0)
				{
					mismatchMask |= 1u;
				}
				if (std::memcmp(&left.world.m_Translate,
					&right.world.m_Translate,
					sizeof(left.world.m_Translate)) != 0)
				{
					mismatchMask |= 2u;
				}
				if (std::memcmp(&left.world.m_fScale,
					&right.world.m_fScale,
					sizeof(left.world.m_fScale)) != 0)
				{
					mismatchMask |= 4u;
				}
				switch (mismatchMask)
				{
				case 1u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationOnly);
					break;
				case 2u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchTranslationOnly);
					break;
				case 3u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationTranslation);
					break;
				case 4u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchScaleOnly);
					break;
				case 5u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationScale);
					break;
				case 6u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchTranslationScale);
					break;
				case 7u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationTranslationScale);
					break;
				default:
					// NiTransform is currently the exact concatenation of these
					// three fields. Keep a conservative diagnostic bucket if that
					// representation ever changes.
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsFirstMismatchWorld);
					break;
				}
				if (mismatchMask != 2u)
					return NativeSegmentConstantsStateRelation::Different;
				translationOnly = true;
			}
			const auto rejectLater = [translationOnly](
				FreeTypePerfCounter counter)
			{
				if (!translationOnly)
					RecordFreeTypePerf(counter);
				return NativeSegmentConstantsStateRelation::Different;
			};
			if (std::memcmp(&left.view, &right.view,
				sizeof(left.view)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchView);
			}
			if (std::memcmp(&left.projection, &right.projection,
				sizeof(left.projection)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchProjection);
			}
			if (std::memcmp(&left.viewProjection,
				&right.viewProjection,
				sizeof(left.viewProjection)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchViewProjection);
			}
			if (std::memcmp(&left.cameraRight, &right.cameraRight,
				sizeof(left.cameraRight)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchCameraRight);
			}
			if (std::memcmp(&left.cameraUp, &right.cameraUp,
				sizeof(left.cameraUp)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchCameraUp);
			}
			if (std::memcmp(&left.overlayColor, &right.overlayColor,
				sizeof(left.overlayColor)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchOverlayColor);
			}
			if (left.rotates
				&& std::memcmp(&left.textureTransform,
					&right.textureTransform,
					sizeof(left.textureTransform)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchTextureTransform);
			}
			if (std::memcmp(&left.tileAlpha, &right.tileAlpha,
				sizeof(left.tileAlpha)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchTileAlpha);
			}
			if (std::memcmp(&left.materialAlpha, &right.materialAlpha,
				sizeof(left.materialAlpha)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchMaterialAlpha);
			}
			if (std::memcmp(&left.nearDepth, &right.nearDepth,
				sizeof(left.nearDepth)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchNearDepth);
			}
			if (std::memcmp(&left.depthRange, &right.depthRange,
				sizeof(left.depthRange)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchDepthRange);
			}
			return translationOnly
				? NativeSegmentConstantsStateRelation::TranslationOnly
				: NativeSegmentConstantsStateRelation::Exact;
		}

		bool BuildSegmentBlendStateKey(NiTriShape* geometry,
			NativeFontStandardBlendSemantics semantics,
			NativeSegmentBlendStateKey& key)
		{
			if (!geometry
				|| !HasPredictableNativeFontBlendSemantics(semantics))
			{
				return false;
			}
			if (semantics == NativeFontStandardBlendSemantics::NativeOwned)
			{
				key = ComputeNativeFontOwnedBlendState(
					&geometry->m_kProperties);
				return true;
			}

			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!tile
				|| semantics != NativeFontStandardBlendSemantics::Retail)
				return false;
			const NiAlphaProperty* alpha =
				geometry->GetAlphaProperty();
			key = {};
			const UInt16 flags = alpha
				? alpha->m_usFlags.Get() : 0;
			const bool propertyBlend = alpha
				&& (flags & NiAlphaProperty::ALPHA_BLEND_MASK) != 0;
			// BE1FF0/BSShader::SetupGeometryAlphaBlending compares both
			// values only against 1.0. Preserve its NaN behavior by negating
			// the pair of >= comparisons instead of using <.
			const bool opacityBlend = !(tile->fAlpha >= 1.0f
				&& tile->fFadeAlpha >= 1.0f);
			key.enabled = propertyBlend || opacityBlend;
			if (propertyBlend)
			{
				key.sourceFunction = static_cast<UInt8>(
					(flags & NiAlphaProperty::SRC_BLEND_MASK)
						>> NiAlphaProperty::SRC_BLEND_POS);
				key.destinationFunction = static_cast<UInt8>(
					(flags & NiAlphaProperty::DEST_BLEND_MASK)
						>> NiAlphaProperty::DEST_BLEND_POS);
			}
			else
			{
				key.sourceFunction = static_cast<UInt8>(
					NiAlphaProperty::ALPHA_SRCALPHA);
				key.destinationFunction = static_cast<UInt8>(
					NiAlphaProperty::ALPHA_INVSRCALPHA);
			}
			return true;
		}

		bool SameSegmentBlendState(
			const NativeSegmentBlendStateKey& left,
			const NativeSegmentBlendStateKey& right)
		{
			return left.enabled == right.enabled
				&& (!left.enabled
					|| (left.sourceFunction == right.sourceFunction
						&& left.destinationFunction
							== right.destinationFunction));
		}

		bool BuildSegmentAlphaTestStateKey(NiTriShape* geometry,
			NativeSegmentAlphaTestStateKey& key)
		{
			const NiAlphaProperty* alpha =
				geometry ? geometry->GetAlphaProperty() : nullptr;
			if (!alpha)
				return false;
			const UInt16 flags = alpha->m_usFlags.Get();
			key.testFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::TEST_FUNC_MASK)
					>> NiAlphaProperty::TEST_FUNC_POS);
			key.alphaTestRef = alpha->m_ucAlphaTestRef;
			return true;
		}

		bool SameSegmentAlphaTestState(
			const NativeSegmentAlphaTestStateKey& left,
			const NativeSegmentAlphaTestStateKey& right)
		{
			return left.testFunction == right.testFunction
				&& left.alphaTestRef == right.alphaTestRef;
		}

		bool BuildSegmentRenderStatesKey(NiTriShape* geometry,
			UInt32 currentPass, bool firstPass,
			NativeSegmentRenderStatesKey& key)
		{
			if (!geometry)
				return false;
			const NiStencilProperty* stencil =
				geometry->GetStencilProperty();
			const NiAlphaProperty* alpha =
				geometry->GetAlphaProperty();
			key = {};
			// BE20E0/BSShader::SetupGeometryRenderStates publishes only two
			// effective outputs. Other stencil/alpha bits do not participate.
			key.drawBoth = (currentPass >= 86u && currentPass <= 87u)
				|| (stencil
					&& ((stencil->m_usFlags.Get()
							& NiStencilProperty::DRAWMODE_MASK)
						>> NiStencilProperty::DRAWMODE_POS)
						== NiStencilProperty::DRAW_BOTH);
			key.alphaTestEnabled = firstPass
				&& alpha && alpha->HasAlphaTest();
			return true;
		}

		bool SameSegmentRenderStates(
			const NativeSegmentRenderStatesKey& left,
			const NativeSegmentRenderStatesKey& right)
		{
			return left.drawBoth == right.drawBoth
				&& left.alphaTestEnabled
					== right.alphaTestEnabled;
		}

		bool SameSegmentGeometryBinding(
			const NativeSegmentGeometryBindingKey& left,
			const NativeSegmentGeometryBindingKey& right)
		{
			return left.declaration == right.declaration
				&& left.vertexBuffer == right.vertexBuffer
				&& left.indexBuffer == right.indexBuffer
				&& left.stride == right.stride;
		}

		bool BuildSegmentDeviceStateStamp(
			const NativeFontFrameStamp* frame,
			UInt32 executionSegmentEpoch,
			UInt32 externalMutationEpoch,
			NativeFontSegmentDeviceStateStamp& stamp)
		{
			stamp = {};
			if (!frame || !frame->renderer || !frame->device
				|| !frame->renderTargetReady
				|| !frame->viewportReady
				|| !frame->renderTargetGroup
				|| !frame->validationToken
				|| !frame->generation
				|| !frame->atlasTextureEpoch
				|| !frame->resourceSerial
				|| !executionSegmentEpoch
				|| !externalMutationEpoch)
			{
				return false;
			}
			stamp.renderer = frame->renderer;
			stamp.device = frame->device;
			stamp.renderTargetGroup = frame->renderTargetGroup;
			stamp.viewport = frame->viewport;
			stamp.validationToken = frame->validationToken;
			stamp.generation = frame->generation;
			stamp.atlasTextureEpoch =
				frame->atlasTextureEpoch;
			stamp.resourceSerial = frame->resourceSerial;
			stamp.uploadEpoch = frame->uploadEpoch;
			stamp.executionSegmentEpoch =
				executionSegmentEpoch;
			stamp.externalMutationEpoch =
				externalMutationEpoch;
			stamp.ready = true;
			return true;
		}

		enum class NativeDirectShapeBindingFailure : UInt8
		{
			None = 0,
			Input,
			Topology,
			Atlas,
			FacadeModelData,
			FacadeAlphaProperty,
			FacadeBufferData,
			FacadeTileProperty,
			FacadeStreamCount,
			FacadeVertexStride,
			FacadeVertexChipArray,
			FacadeVertexChip,
			Property,
			Texture,
			Shader,
		};

		class NativeDirectShapeBinding
		{
		public:
			NativeDirectShapeBinding(NiTriShape* shape,
				NativeFontShapePayload& payload,
				const NativeFontDirectShapeSubmission& submission)
				: m_shape(shape)
			{
				Initialize(payload, submission.vertexBuffer,
					submission.indexBuffer, submission.declaration,
					submission.baseVertex, submission.vertexCount,
					submission.indexBytes);
			}

			NativeDirectShapeBinding(NiTriShape* shape,
				NativeFontShapePayload& payload,
				const NativeFontFramePacketBinding& binding)
				: m_shape(shape)
			{
				if (!binding.active)
					return;
				Initialize(payload, binding.vertexBuffer,
					binding.indexBuffer, binding.declaration,
					binding.baseVertex, binding.vertexCount,
					binding.indexBytes);
			}

			NativeDirectShapeBinding(
				const NativeDirectShapeBinding&) = delete;
			NativeDirectShapeBinding& operator=(
				const NativeDirectShapeBinding&) = delete;

			~NativeDirectShapeBinding()
			{
				if (m_active)
					Restore();
			}

			bool Active() const
			{
				return m_active;
			}

			NativeDirectShapeBindingFailure Failure() const
			{
				return m_failure;
			}

			NiGeometryBufferData* Buffer() const
			{
				return m_active ? m_buffer : nullptr;
			}

		private:
			bool AttachSyntheticBuffer()
			{
				if (!m_data || m_data->m_pkBuffData
					|| m_syntheticBufferConstructed)
				{
					return false;
				}

				m_originalBuffer = m_data->m_pkBuffData;
				m_buffer = reinterpret_cast<NiGeometryBufferData*>(
					m_syntheticBufferStorage.data());
				ThisStdCall<void>(
					kNiGeometryBufferDataConstructor, m_buffer);
				m_syntheticBufferConstructed = true;
				std::memset(&m_syntheticChip, 0,
					sizeof(m_syntheticChip));
				m_syntheticChips[0] = &m_syntheticChip;
				m_syntheticStride = sizeof(NativeFontGpuVertex);
				m_buffer->m_uiStreamCount = 1;
				m_buffer->m_puiVertexStride =
					&m_syntheticStride;
				m_buffer->m_ppkVBChip = m_syntheticChips.data();
				m_buffer->m_eType = D3DPT_TRIANGLELIST;
				m_buffer->m_uiNumArrays = 1;
				m_buffer->m_pusArrayLengths = nullptr;
				m_buffer->m_pusIndexArray = nullptr;
				m_syntheticChip.m_uiIndex = 0;
				m_data->m_pkBuffData = m_buffer;
				if (m_data->m_pkBuffData != m_buffer)
				{
					DestroySyntheticBuffer();
					return false;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SinglePacketDirectSyntheticBuffer);
				return true;
			}

			void DestroySyntheticBuffer()
			{
				if (!m_syntheticBufferConstructed || !m_buffer)
					return;
				if (m_data && m_data->m_pkBuffData == m_buffer)
					m_data->m_pkBuffData = m_originalBuffer;

				// E8F0F0 owns declaration, IB, stride, and chip arrays. This
				// stack-local descriptor borrows all of them, so detach every
				// owned field before invoking the non-deleting retail destructor.
				m_buffer->m_pkGeometryGroup = nullptr;
				m_buffer->m_hDeclaration = nullptr;
				m_buffer->m_uiStreamCount = 0;
				m_buffer->m_puiVertexStride = nullptr;
				m_buffer->m_ppkVBChip = nullptr;
				m_buffer->m_pkIB = nullptr;
				ThisStdCall<void>(
					kNiGeometryBufferDataDestructor, m_buffer);
				m_buffer = m_originalBuffer;
				m_chip = nullptr;
				m_syntheticBufferConstructed = false;
			}

			void Initialize(NativeFontShapePayload& payload,
				IDirect3DVertexBuffer9* vertexBuffer,
				IDirect3DIndexBuffer9* indexBuffer,
				IDirect3DVertexDeclaration9* declaration,
				UInt32 baseVertex, UInt32 vertexCount,
				UInt32 indexBytes)
			{
				m_failure = NativeDirectShapeBindingFailure::Input;
				if (!m_shape || m_shape->GetSkinInstance()
					|| !payload.payloadTemplate
					|| payload.packetShaders.size() != 1
					|| !payload.packetShaders[0]
					|| !vertexBuffer || !indexBuffer
					|| !declaration || !vertexCount || !indexBytes
					|| (vertexCount & 3u))
				{
					return;
				}
				const NativeFontPayloadTemplate& artifact =
					*payload.payloadTemplate;
				const std::vector<NativeFontPacketTemplate>& packets =
					GetNativeFontPackets(artifact,
						payload.useCompositePackets);
				if (packets.size() != 1
					|| packets[0].vertexCount != vertexCount)
				{
					m_failure =
						NativeDirectShapeBindingFailure::Topology;
					return;
				}
				const NativeFontPacketTemplate& packet = packets[0];
				const UInt64 vertexEnd =
					static_cast<UInt64>(packet.firstVertex)
						+ packet.vertexCount;
				if (vertexEnd > artifact.gpuVertices.size()
					|| packet.atlasPage
						>= artifact.atlasProperties.size()
					|| packet.atlasPage
						>= artifact.atlasTextures.size()
					|| !artifact.atlasProperties[packet.atlasPage]
					|| !artifact.atlasTextures[packet.atlasPage])
				{
					m_failure = NativeDirectShapeBindingFailure::Atlas;
					return;
				}

				m_data = m_shape->GetModelData();
				m_alpha = m_shape->GetAlphaProperty();
				m_buffer = m_data ? m_data->m_pkBuffData : nullptr;
				m_tile = GetDirectTileProperty(m_shape);
				if (!m_data)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeModelData;
					return;
				}
				if (!m_alpha)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeAlphaProperty;
					return;
				}
				if (!m_tile)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeTileProperty;
					return;
				}
				if (!m_buffer && !AttachSyntheticBuffer())
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeBufferData;
					return;
				}
				if (!m_buffer->m_uiStreamCount)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeStreamCount;
					return;
				}
				if (!m_buffer->m_puiVertexStride)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexStride;
					return;
				}
				if (!m_buffer->m_ppkVBChip)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexChipArray;
					return;
				}
				if (!m_buffer->m_ppkVBChip[0])
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexChip;
					return;
				}
				m_chip = m_buffer->m_ppkVBChip[0];

				m_local = m_shape->m_kLocal;
				m_world = m_shape->m_kWorld;
				m_bound = m_data->m_kBound;
				m_shader = m_shape->GetShader();
				m_alphaFlags = m_alpha->m_usFlags;
				m_alphaTestRef = m_alpha->m_ucAlphaTestRef;
				m_atlasProperty = m_shape->GetTexturingProperty();
				m_atlasTexture = m_tile->sourceTexture;

				m_bufferFlags = m_buffer->m_uiFlags;
				m_geometryGroup = m_buffer->m_pkGeometryGroup;
				m_fvf = m_buffer->m_uiFVF;
				m_declaration = m_buffer->m_hDeclaration;
				m_softwareVertexProcessing =
					m_buffer->m_bSoftwareVP;
				m_vertexCount = m_buffer->m_uiVertCount;
				m_maxVertexCount = m_buffer->m_uiMaxVertCount;
				m_streamCount = m_buffer->m_uiStreamCount;
				m_stride = m_buffer->m_puiVertexStride[0];
				m_indexCount = m_buffer->m_uiIndexCount;
				m_indexBytes = m_buffer->m_uiIBSize;
				m_indexBuffer = m_buffer->m_pkIB;
				m_baseVertex = m_buffer->m_uiBaseVertexIndex;
				m_primitiveType = m_buffer->m_eType;
				m_triangleCount = m_buffer->m_uiTriCount;
				m_maxTriangleCount = m_buffer->m_uiMaxTriCount;
				m_arrayCount = m_buffer->m_uiNumArrays;
				m_arrayLengths = m_buffer->m_pusArrayLengths;
				m_indexArray = m_buffer->m_pusIndexArray;

				m_chipIndex = m_chip->m_uiIndex;
				m_vertexBuffer = m_chip->m_pkVB;
				m_chipOffset = m_chip->m_uiOffset;
				m_chipLockFlags = m_chip->m_uiLockFlags;
				m_chipSize = m_chip->m_uiSize;
				m_stateCaptured = true;

				NiTexturingProperty* desiredProperty =
					artifact.atlasProperties[
						packet.atlasPage].m_pObject;
				NiTexture* desiredTexture =
					artifact.atlasTextures[
						packet.atlasPage].m_pObject;
				if (m_atlasProperty.m_pObject != desiredProperty)
				{
					m_atlasPropertyChanged = true;
					m_shape->RemoveProperty(NiProperty::TEXTURING);
					m_shape->AddProperty(desiredProperty);
					m_shape->UpdateProperties();
					if (m_shape->GetTexturingProperty()
						!= desiredProperty)
					{
						m_failure =
							NativeDirectShapeBindingFailure::Property;
						Restore();
						return;
					}
				}
				if (m_atlasTexture.m_pObject != desiredTexture)
				{
					m_atlasTextureChanged = true;
					ThisStdCall<void>(kTileImageSetSourceTexture,
						m_tile, desiredTexture);
					if (m_tile->sourceTexture.m_pObject
						!= desiredTexture)
					{
						m_failure =
							NativeDirectShapeBindingFailure::Texture;
						Restore();
						return;
					}
				}

				const UInt32 quadCount = vertexCount / 4u;
				ApplyNativeGeometryOrigin(m_shape->m_kLocal, m_local,
					payload.geometryOrigin);
				ApplyNativeGeometryOrigin(m_shape->m_kWorld, m_world,
					payload.geometryOrigin);
				m_data->m_kBound = packet.bound;
				m_alpha->SetAlphaTesting(false);
				m_shape->SetShader(payload.packetShaders[0]);

				m_buffer->m_uiFlags = 0;
				m_buffer->m_pkGeometryGroup = nullptr;
				m_buffer->m_uiFVF = 0;
				m_buffer->m_hDeclaration = declaration;
				m_buffer->m_bSoftwareVP = false;
				m_buffer->m_uiVertCount = vertexCount;
				m_buffer->m_uiMaxVertCount = vertexCount;
				m_buffer->m_uiStreamCount = 1;
				m_buffer->m_puiVertexStride[0] =
					sizeof(NativeFontGpuVertex);
				m_buffer->m_uiIndexCount = quadCount * 6u;
				m_buffer->m_uiIBSize = indexBytes;
				m_buffer->m_pkIB = indexBuffer;
				m_buffer->m_uiBaseVertexIndex = baseVertex;
				m_buffer->m_eType = D3DPT_TRIANGLELIST;
				m_buffer->m_uiTriCount = quadCount * 2u;
				m_buffer->m_uiMaxTriCount = quadCount * 2u;
				m_buffer->m_uiNumArrays = 1;
				m_buffer->m_pusArrayLengths = nullptr;
				m_buffer->m_pusIndexArray = nullptr;

				m_chip->m_uiIndex = 0;
				m_chip->m_pkVB = vertexBuffer;
				m_chip->m_uiOffset = 0;
				m_chip->m_uiLockFlags = 0;
				m_chip->m_uiSize =
					vertexCount * sizeof(NativeFontGpuVertex);

				if (m_shape->GetShader() != payload.packetShaders[0])
				{
					m_failure =
						NativeDirectShapeBindingFailure::Shader;
					Restore();
					return;
				}
				m_failure = NativeDirectShapeBindingFailure::None;
				m_active = true;
			}

			void Restore()
			{
				if (!m_stateCaptured || !m_shape || !m_data
					|| !m_alpha || !m_buffer || !m_chip)
					return;
				if (m_atlasPropertyChanged)
				{
					m_shape->RemoveProperty(NiProperty::TEXTURING);
					if (m_atlasProperty)
						m_shape->AddProperty(m_atlasProperty.m_pObject);
					m_shape->UpdateProperties();
				}
				if (m_atlasTextureChanged && m_tile)
				{
					ThisStdCall<void>(kTileImageSetSourceTexture,
						m_tile, m_atlasTexture.m_pObject);
				}

				m_chip->m_uiIndex = m_chipIndex;
				m_chip->m_pkVB = m_vertexBuffer;
				m_chip->m_uiOffset = m_chipOffset;
				m_chip->m_uiLockFlags = m_chipLockFlags;
				m_chip->m_uiSize = m_chipSize;

				m_buffer->m_uiFlags = m_bufferFlags;
				m_buffer->m_pkGeometryGroup = m_geometryGroup;
				m_buffer->m_uiFVF = m_fvf;
				m_buffer->m_hDeclaration = m_declaration;
				m_buffer->m_bSoftwareVP =
					m_softwareVertexProcessing;
				m_buffer->m_uiVertCount = m_vertexCount;
				m_buffer->m_uiMaxVertCount = m_maxVertexCount;
				m_buffer->m_uiStreamCount = m_streamCount;
				m_buffer->m_puiVertexStride[0] = m_stride;
				m_buffer->m_uiIndexCount = m_indexCount;
				m_buffer->m_uiIBSize = m_indexBytes;
				m_buffer->m_pkIB = m_indexBuffer;
				m_buffer->m_uiBaseVertexIndex = m_baseVertex;
				m_buffer->m_eType = m_primitiveType;
				m_buffer->m_uiTriCount = m_triangleCount;
				m_buffer->m_uiMaxTriCount = m_maxTriangleCount;
				m_buffer->m_uiNumArrays = m_arrayCount;
				m_buffer->m_pusArrayLengths = m_arrayLengths;
				m_buffer->m_pusIndexArray = m_indexArray;

				m_shape->SetShader(m_shader);
				m_alpha->m_usFlags = m_alphaFlags;
				m_alpha->m_ucAlphaTestRef = m_alphaTestRef;
				m_data->m_kBound = m_bound;
				m_shape->m_kLocal = m_local;
				m_shape->m_kWorld = m_world;
				DestroySyntheticBuffer();
				m_active = false;
				m_stateCaptured = false;
			}

			NiTriShape* m_shape = nullptr;
			NiTriShapeData* m_data = nullptr;
			NiAlphaProperty* m_alpha = nullptr;
			DirectTileShaderPropertyView* m_tile = nullptr;
			NiGeometryBufferData* m_buffer = nullptr;
			NiVBChip* m_chip = nullptr;
			NiTransform m_local;
			NiTransform m_world;
			NiBound m_bound;
			BSShader* m_shader = nullptr;
			NiTexturingPropertyPtr m_atlasProperty;
			NiTexturePtr m_atlasTexture;
			Bitfield16 m_alphaFlags;
			UInt8 m_alphaTestRef = 0;
			UInt32 m_bufferFlags = 0;
			NiGeometryGroup* m_geometryGroup = nullptr;
			UInt32 m_fvf = 0;
			void* m_declaration = nullptr;
			bool m_softwareVertexProcessing = false;
			UInt32 m_vertexCount = 0;
			UInt32 m_maxVertexCount = 0;
			UInt32 m_streamCount = 0;
			UInt32 m_stride = 0;
			UInt32 m_indexCount = 0;
			UInt32 m_indexBytes = 0;
			IDirect3DIndexBuffer9* m_indexBuffer = nullptr;
			UInt32 m_baseVertex = 0;
			D3DPRIMITIVETYPE m_primitiveType = D3DPT_FORCE_DWORD;
			UInt32 m_triangleCount = 0;
			UInt32 m_maxTriangleCount = 0;
			UInt32 m_arrayCount = 0;
			const UInt16* m_arrayLengths = nullptr;
			const UInt16* m_indexArray = nullptr;
			UInt32 m_chipIndex = 0;
			IDirect3DVertexBuffer9* m_vertexBuffer = nullptr;
			UInt32 m_chipOffset = 0;
			UInt32 m_chipLockFlags = 0;
			UInt32 m_chipSize = 0;
			alignas(NiGeometryBufferData)
				std::array<UInt8, sizeof(NiGeometryBufferData)>
					m_syntheticBufferStorage = {};
			std::array<NiVBChip*, 1> m_syntheticChips = {};
			NiVBChip m_syntheticChip = {};
			UInt32 m_syntheticStride = 0;
			NiGeometryBufferData* m_originalBuffer = nullptr;
			bool m_atlasPropertyChanged = false;
			bool m_atlasTextureChanged = false;
			bool m_stateCaptured = false;
			NativeDirectShapeBindingFailure m_failure =
				NativeDirectShapeBindingFailure::Input;
			bool m_syntheticBufferConstructed = false;
			bool m_active = false;
		};

		bool SameNativePacketBinding(
			const NativeFontFramePacketBinding& command,
			const NativeFontDirectFacadePacketBinding& live)
		{
			return command.active && live.active
				&& command.vertexBuffer == live.vertexBuffer
				&& command.indexBuffer == live.indexBuffer
				&& command.declaration == live.declaration
				&& command.baseVertex == live.baseVertex
				&& command.vertexCount == live.vertexCount
				&& command.indexBytes == live.indexBytes
				&& command.generation == live.generation
				&& command.resourceSerial == live.resourceSerial
				&& command.uploadEpoch == live.uploadEpoch
				&& command.staticResident == live.staticResident;
		}

		FreeTypePerfCounter DirectShapeBindingFallbackCounter(
			NativeDirectShapeBindingFailure failure)
		{
			switch (failure)
			{
			case NativeDirectShapeBindingFailure::Topology:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingTopology;
			case NativeDirectShapeBindingFailure::Atlas:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingAtlas;
			case NativeDirectShapeBindingFailure::FacadeModelData:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeModelData;
			case NativeDirectShapeBindingFailure::FacadeAlphaProperty:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty;
			case NativeDirectShapeBindingFailure::FacadeBufferData:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeBufferData;
			case NativeDirectShapeBindingFailure::FacadeTileProperty:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeTileProperty;
			case NativeDirectShapeBindingFailure::FacadeStreamCount:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeStreamCount;
			case NativeDirectShapeBindingFailure::FacadeVertexStride:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexStride;
			case NativeDirectShapeBindingFailure::FacadeVertexChipArray:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray;
			case NativeDirectShapeBindingFailure::FacadeVertexChip:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChip;
			case NativeDirectShapeBindingFailure::Property:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingProperty;
			case NativeDirectShapeBindingFailure::Texture:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingTexture;
			case NativeDirectShapeBindingFailure::Shader:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingShader;
			case NativeDirectShapeBindingFailure::None:
			case NativeDirectShapeBindingFailure::Input:
			default:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingInput;
			}
		}

		void RecordSinglePacketDirectFallback(
			FreeTypePerfCounter stage)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectFallback);
			RecordFreeTypePerf(stage);
			switch (stage)
			{
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeModelData:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeAlphaProperty:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeBufferData:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeTileProperty:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeStreamCount:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexStride:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexChipArray:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexChip:
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SinglePacketDirectFallbackBindingFacade);
				break;
			default:
				break;
			}
		}

		enum class NativeDirectDrawLiteFallback : UInt8
		{
			None = 0,
			Program,
			Renderer,
			Geometry,
			Binding,
			Declaration,
		};

		struct NativeDirectDrawLiteSubmission
		{
			NiTriShapeData* data = nullptr;
			NiDX9RenderState* renderState = nullptr;
			IDirect3DDevice9* device = nullptr;
			NativeSegmentDeviceStateCache* deviceState = nullptr;
			NativeSegmentGeometryBindingKey binding;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 triangleCount = 0;
		};

		void RecordNativeDirectDrawLiteFallback(
			NativeDirectDrawLiteFallback failure)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteFallback);
			switch (failure)
			{
			case NativeDirectDrawLiteFallback::Renderer:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackRenderer);
				break;
			case NativeDirectDrawLiteFallback::Geometry:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackGeometry);
				break;
			case NativeDirectDrawLiteFallback::Binding:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackBinding);
				break;
			case NativeDirectDrawLiteFallback::Declaration:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackDeclaration);
				break;
			case NativeDirectDrawLiteFallback::None:
			case NativeDirectDrawLiteFallback::Program:
			default:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackProgram);
				break;
			}
		}

		NativeDirectDrawLiteFallback BuildNativeDirectDrawLiteSubmission(
			NiTriShape* geometry, NiDX9Renderer* renderer,
			const NiPropertyState* properties,
			const NativeFontCompiledPacketCommand& program,
			const NativeFontDrawCommand& command,
			NiGeometryBufferData* buffer,
			NativeSegmentDeviceStateCache* deviceState,
			NativeDirectDrawLiteSubmission& submission)
		{
			submission = {};
			TileShader* shader = program.shader;
			void** geometryVtable = geometry
				? *reinterpret_cast<void***>(geometry) : nullptr;
			void** shaderVtable = shader
				? *reinterpret_cast<void***>(shader) : nullptr;
			if (!program.directDrawLiteReady || !geometry || !shader
				|| !shaderVtable || shaderVtable != program.shaderVtable
				|| command.program != &program
				|| State().originalRenderImmediateAlt
					!= reinterpret_cast<RenderImmediateFn>(
						kNiTriShapeOnlyRenderImmediate))
			{
				return NativeDirectDrawLiteFallback::Program;
			}

			if (!renderer || !program.device
				|| program.device != renderer->GetD3DDevice()
				|| shader->m_pkD3DDevice != program.device
				|| shader->m_pkD3DRenderer != renderer
				|| !shader->m_pkD3DRenderState
				|| shader->m_pkD3DRenderState != renderer->m_pkRenderState
				|| !renderer->GetInsideFrameState()
				|| !renderer->m_bRenderTargetGroupActive
				|| renderer->m_bDeviceLost)
			{
				return NativeDirectDrawLiteFallback::Renderer;
			}

			NiTriShapeData* data = geometry->GetModelData();
			if (!geometryVtable
				|| !IsNativeFontAtlasShape(geometry)
				|| geometryVtable[kGeometrySegmentedPredicateSlot]
					!= reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate)
				|| geometryVtable[kGeometryResizablePredicateSlot]
					!= reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate)
				|| geometry->GetSkinInstance() || geometry->GetControllers()
				|| geometry->GetShader() != shader || !data
				|| data->m_pkBuffData != buffer
				|| data->m_spAdditionalGeomData.m_pObject
				|| (data->m_usDirtyFlags
					& NiGeometryData::CONSISTENCY_MASK)
					!= NiGeometryData::STATIC
				|| !data->GetActiveVertexCount()
				|| properties != &geometry->m_kProperties
				|| renderer->m_pkCurrProp != properties
				|| renderer->m_pkCurrEffects
				|| shader->m_uiCurrentPass != 0)
			{
				return NativeDirectDrawLiteFallback::Geometry;
			}

			const NativeFontFramePacketBinding& expected = command.binding;
			NiVBChip* chip = buffer && buffer->m_uiStreamCount == 1
				&& buffer->m_ppkVBChip
				? buffer->m_ppkVBChip[0] : nullptr;
			const UInt32 vertexCount = expected.vertexCount;
			const UInt32 quadCount = vertexCount / 4u;
			const UInt32 triangleCount = quadCount * 2u;
			const UInt32 indexCount = quadCount * 6u;
			const UInt64 requiredVertexBytes =
				static_cast<UInt64>(vertexCount)
					* sizeof(NativeFontGpuVertex);
			const UInt64 requiredIndexBytes =
				static_cast<UInt64>(indexCount) * sizeof(UInt16);
			if (!expected.active || !expected.vertexBuffer
				|| !expected.indexBuffer || !expected.declaration
				|| !vertexCount || (vertexCount & 3u)
				|| !command.packet
				|| command.packet->vertexCount != vertexCount
				|| !buffer || buffer->m_uiFlags
				|| buffer->m_pkGeometryGroup || buffer->m_uiFVF
				|| buffer->m_hDeclaration != expected.declaration
				|| buffer->m_bSoftwareVP
				|| buffer->m_uiVertCount != vertexCount
				|| buffer->m_uiMaxVertCount != vertexCount
				|| buffer->m_uiStreamCount != 1
				|| !buffer->m_puiVertexStride
				|| buffer->m_puiVertexStride[0]
					!= sizeof(NativeFontGpuVertex)
				|| !chip || chip->m_uiIndex
				|| chip->m_pkVB != expected.vertexBuffer
				|| chip->m_uiOffset || chip->m_uiLockFlags
				|| chip->m_uiSize < requiredVertexBytes
				|| buffer->m_uiIndexCount != indexCount
				|| buffer->m_uiIBSize != expected.indexBytes
				|| requiredIndexBytes > buffer->m_uiIBSize
				|| buffer->m_pkIB != expected.indexBuffer
				|| buffer->m_uiBaseVertexIndex != expected.baseVertex
				|| buffer->m_eType != D3DPT_TRIANGLELIST
				|| buffer->m_uiTriCount != triangleCount
				|| buffer->m_uiMaxTriCount != triangleCount
				|| buffer->m_uiNumArrays != 1
				|| buffer->m_pusArrayLengths
				|| buffer->m_pusIndexArray)
			{
				return NativeDirectDrawLiteFallback::Binding;
			}

			NiD3DShaderDeclaration* shaderDeclaration =
				shader->m_spShaderDecl.m_pObject;
			if (!shaderDeclaration
				|| shaderDeclaration->GetD3DDeclaration()
					!= expected.declaration)
			{
				return NativeDirectDrawLiteFallback::Declaration;
			}

			submission.data = data;
			submission.renderState = shader->m_pkD3DRenderState;
			submission.device = program.device;
			submission.deviceState = deviceState;
			submission.binding.declaration = expected.declaration;
			submission.binding.vertexBuffer = expected.vertexBuffer;
			submission.binding.indexBuffer = expected.indexBuffer;
			submission.binding.stride = sizeof(NativeFontGpuVertex);
			submission.baseVertex = expected.baseVertex;
			submission.vertexCount = vertexCount;
			submission.triangleCount = triangleCount;
			return NativeDirectDrawLiteFallback::None;
		}

		class NativeDirectDrawLiteArmScope
		{
		public:
			NativeDirectDrawLiteArmScope(NiTriShape* geometry,
				const NativeDirectDrawLiteSubmission& submission)
			{
				m_context = s_nativeDirectImmediateContext;
				if (!m_context || m_context->shape != geometry
					|| m_context->directDrawLite)
				{
					m_context = nullptr;
					return;
				}
				m_context->directDrawLite = &submission;
			}

			~NativeDirectDrawLiteArmScope()
			{
				if (m_context)
					m_context->directDrawLite = nullptr;
			}

			bool Active() const
			{
				return m_context != nullptr;
			}

		private:
			NativeDirectImmediateContext* m_context = nullptr;
		};

		void ExecuteNativeDirectDrawLite(
			const NativeDirectDrawLiteSubmission& submission)
		{
			NativeSegmentDeviceStateCache* deviceState =
				submission.deviceState;
			const bool bindingReady = deviceState
				&& deviceState->geometryBindingReady
				&& SameSegmentGeometryBinding(
					deviceState->geometryBinding, submission.binding);
			HRESULT streamResult = D3D_OK;
			HRESULT indexResult = D3D_OK;
			if (bindingReady)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteBindingReuse);
			}
			else
			{
				if (deviceState)
					deviceState->geometryBindingReady = false;
				submission.renderState->vSetDeclaration(
					submission.binding.declaration, false);
				streamResult = submission.device->SetStreamSource(
					0, submission.binding.vertexBuffer, 0,
					submission.binding.stride);
				indexResult = submission.device->SetIndices(
					submission.binding.indexBuffer);
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteBindingSet);
				if (FAILED(streamResult) || FAILED(indexResult))
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeDirectDrawLiteBindingDeviceFailure);
				}
				else if (deviceState)
				{
					deviceState->geometryBinding = submission.binding;
					deviceState->geometryBindingReady = true;
				}
			}

			const HRESULT drawResult = submission.device->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST,
				static_cast<INT>(submission.baseVertex), 0,
				submission.vertexCount, 0, submission.triangleCount);
			// Formal E745A0 clears the low dirty/revision bits after the indexed
			// loop even when the D3D call fails. Preserve that exact side effect.
			submission.data->m_usDirtyFlags &= 0xF000u;
			if (FAILED(drawResult))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteDrawDeviceFailure);
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteReplay);
		}

		class SingletonFacadeTileStateScope
		{
		public:
			SingletonFacadeTileStateScope(NiTriShape* shape,
				const NativeFontShapePayload& payload,
				const NativeFontPacketTemplate& packet)
				: m_shape(shape)
			{
				m_data = m_shape ? m_shape->GetModelData() : nullptr;
				m_alpha = m_shape ? m_shape->GetAlphaProperty() : nullptr;
				if (!m_shape || !m_data || !m_alpha)
					return;
				m_local = m_shape->m_kLocal;
				m_world = m_shape->m_kWorld;
				m_bound = m_data->m_kBound;
				m_alphaFlags = m_alpha->m_usFlags;
				m_alphaTestRef = m_alpha->m_ucAlphaTestRef;
				ApplyNativeGeometryOrigin(
					m_shape->m_kLocal, m_local, payload.geometryOrigin);
				ApplyNativeGeometryOrigin(
					m_shape->m_kWorld, m_world, payload.geometryOrigin);
				m_data->m_kBound = packet.bound;
				m_alpha->SetAlphaTesting(false);
				m_active = true;
			}

			~SingletonFacadeTileStateScope()
			{
				if (!m_active)
					return;
				m_alpha->m_usFlags = m_alphaFlags;
				m_alpha->m_ucAlphaTestRef = m_alphaTestRef;
				m_data->m_kBound = m_bound;
				m_shape->m_kLocal = m_local;
				m_shape->m_kWorld = m_world;
			}

			bool Active() const
			{
				return m_active;
			}

		private:
			NiTriShape* m_shape = nullptr;
			NiTriShapeData* m_data = nullptr;
			NiAlphaProperty* m_alpha = nullptr;
			NiTransform m_local;
			NiTransform m_world;
			NiBound m_bound;
			Bitfield16 m_alphaFlags;
			UInt8 m_alphaTestRef = 0;
			bool m_active = false;
		};

		struct NativePacketDrawResult
		{
			bool runtimeFault = false;
			bool drewPacket = false;
			bool directShapeRoute = false;
			bool vanillaLikeBitmapRoute = false;
			bool constantStateFault = false;
			UInt32 drawnPacketCount = 0;
			NativeFontFallbackReason failure =
				NativeFontFallbackReason::RuntimeFault;
			const char* operation = "generation-changed-after-packet";
			HRESULT result = D3DERR_DEVICELOST;
			SInt32 mismatchRegister = -1;
		};

		const NativeFontDrawCommand* ResolveNativeCommand(
			const NativeFontCommandSpanView& view, UInt32 commandOffset)
		{
			if (!view.span || !view.commands
				|| commandOffset >= view.span->commandCount)
			{
				return nullptr;
			}
			return &view.commands[
				view.span->firstCommand + commandOffset];
		}

		bool IsDefaultNativeReplayPass(UInt32 pass)
		{
			switch (pass)
			{
			case 0xCA:
			case 0xD1:
			case 0xFC:
			case 0xFD:
			case 0x102:
				return false;
			default:
				return true;
			}
		}

		NativeFontCommandBindState MakeNativeCommandBindState(
			const BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool testAlpha,
			bool blendAlpha, bool setupRenderStates)
		{
			NativeFontCommandBindState state;
			state.firstPass = pass && pass->bIsFirst;
			state.applyBlend = state.firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass);
			state.applyAlphaTest = state.firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			state.applyRenderStates = setupRenderStates;
			return state;
		}

		bool CallGeometryPredicate(NiGeometry* geometry, UInt32 slot)
		{
			void** vtable = geometry
				? *reinterpret_cast<void***>(geometry) : nullptr;
			if (!vtable || !vtable[slot])
				return true;
			using PredicateFn = bool(__thiscall*)(NiGeometry*);
			return reinterpret_cast<PredicateFn>(vtable[slot])(geometry);
		}

		bool RendererUsesSpecialPass(NiGeometry* geometry)
		{
			void* rendererState =
				*reinterpret_cast<void**>(kBSShaderManager_pRenderer);
			if (!rendererState || !geometry)
				return true;
			// Retail B994F0 loads dword_11F9508 into ECX before calling
			// E72C20(rendererState, geometry, 0). Calling it as __cdecl leaves
			// ECX undefined and can classify every Tile as a multi-pass shape.
			using PredicateFn =
				bool(__thiscall*)(void*, NiGeometry*, UInt32);
			return reinterpret_cast<PredicateFn>(
				kNiDX9RendererIsHardwareSkinned)(
					rendererState, geometry, 0);
		}

		bool CanUseNativeReplayBase(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated)
		{
			if (!g_bEnableFreeTypeFontCommandBuffer
				|| !pass || !geometry || !command.program
				|| !command.program->active
				|| pass->pGeometry != geometry
				|| pass->usPassEnum != currentPass
				|| currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass)
				|| geometry->GetSkinInstance()
				|| pass->ucNumLights || pass->ppSceneLights)
			{
				return false;
			}
			if (!packetStatePrevalidated)
			{
				BSShader* shader = geometry->GetShader();
				if (!shader || shader != command.program->shader
					|| !shader->IsTileShader())
				{
					return false;
				}
			}
			return true;
		}

		bool CanUseGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated)
		{
			if (!CanUseNativeReplayBase(
					pass, currentPass, geometry, command,
					packetStatePrevalidated))
			{
				return false;
			}
			// These are the three mutually exclusive branches immediately before
			// Retail BSBatchRenderer::RenderPassImmediately reaches
			// BSBatchRenderer::RenderPassImmediately_Standard. E72C20 is a renderer-state
			// __thiscall, while the other two are NiGeometry virtual calls.
			// Treat an absent vtable predicate as unsafe instead of guessing.
			if (RendererUsesSpecialPass(geometry)
				|| CallGeometryPredicate(
					geometry, kGeometrySpecialPredicateSlot)
				|| CallGeometryPredicate(
					geometry, kGeometryAlternatePredicateSlot))
			{
				return false;
			}
			return true;
		}

		bool CanUseStandardPassLiteEnvelope(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated,
			const NativeFontStandardPassLiteDispatch*& retainedDispatch)
		{
			retainedDispatch = nullptr;
			const NativeFontCompiledPacketCommand* program =
				command.program;
			const NativeFontStandardPassLiteDispatch* dispatch =
				command.standardPassLite;
			const bool dispatchCurrent =
				dispatch && program
				&& (packetStatePrevalidated
					? dispatch->ready
						&& dispatch->geometry == geometry
						&& dispatch->program == program
					: IsNativeFontStandardPassLiteDispatchCurrent(
						*dispatch, geometry, program,
						program->generation));
			if (!dispatchCurrent)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						StandardPassLiteRetainedMiss);
				return false;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteRetainedHit);
			retainedDispatch = dispatch;

			// The retained dispatch has already proved the Tile-owned vtable,
			// null skin, model data, renderer/device, shader vtable, and complete
			// slot table. Only RenderPass fields are live per traversal.
			return g_bEnableFreeTypeFontCommandBuffer
				&& pass && geometry
				&& pass->pGeometry == geometry
				&& pass->usPassEnum == currentPass
				&& currentPass != kForcedShaderSelectionPass
				&& IsDefaultNativeReplayPass(currentPass)
				&& !pass->ucNumLights
				&& !pass->ppSceneLights
				&& (packetStatePrevalidated
					|| (program->active
						&& geometry->GetShader() == dispatch->shader
						&& dispatch->shader->IsTileShader()));
		}

		bool ShouldEnableNativeFontVendorAlphaToCoverage(
			NiTriShape* geometry)
		{
			if (!geometry)
				return false;
			const NiAlphaProperty* alpha = geometry->GetAlphaProperty();
			if (!alpha || !alpha->HasAlphaTest())
				return false;
			const BSShaderProperty* shade =
				geometry->GetShadeProperty<BSShaderProperty>();
			return (!shade || !shade->HasNoTMSAA())
				&& !geometry->IsParticlesGeom();
		}

		bool PrepareGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, BSShader* validatedShader)
		{
			if (!pass || !geometry)
				return false;
			BSShader* shader = validatedShader
				? validatedShader : geometry->GetShader();
			if (!shader
				|| (!validatedShader && !shader->IsTileShader()))
				return false;

			// The hook replaces the B994F0 call at B64FD1, so none of B994F0's
			// prelude has executed yet. Mirror the retail order before entering
			// the confirmed RenderPassImmediately_Standard branch.
			*reinterpret_cast<BSShaderProperty::RenderPass**>(
				kBSShaderManager_pCurrentRenderPass) = pass;
			*reinterpret_cast<UInt32*>(kBSShaderManager_eCurrentPass) =
				currentPass;

			const bool selectShader =
				*reinterpret_cast<UInt32*>(
					kBSBatchRenderer_uiLastPass) != currentPass
				|| *reinterpret_cast<BSShader**>(
					kBSBatchRenderer_pLastShader) != shader;
			if (selectShader)
			{
				// B99390 first tears down the previously selected shader and
				// invokes the new shader's pass callbacks. Those callbacks are
				// outside the four Standard-lite slots and may republish any of
				// their device-state categories. Do not carry a cache head
				// across that transition, even if the command stamp itself is
				// otherwise unchanged.
				InvalidateSegmentDeviceStateCache();
				CdeclCall<void>(kBSBatchRendererBeginPass,
					currentPass, shader);
				if (*reinterpret_cast<UInt32*>(
						kBSBatchRenderer_uiLastPass) != currentPass
					|| *reinterpret_cast<BSShader**>(
						kBSBatchRenderer_pLastShader) != shader)
				{
					return false;
				}
			}
			// B994F0 performs post-pass restoration only for shader types 1-5
			// and 17. Guarded replay accepts only TileShader (type 20), so the
			// retail default branch has no corresponding restoration call.

			if (*reinterpret_cast<UInt8*>(
					kBSShaderManager_bTransparencyMultisampling))
			{
				// B98540 is the PC vendor alpha-to-coverage publisher, not an
				// alpha-test-state owner. Publish the final Tile rule locally so
				// this private replay does not depend on a replacement inside the
				// skipped B994F0 wrapper. The formal build writes only render
				// state 154 with A2M0/A2M1 or state 181 with 0/ATOC. Slot 33
				// owns states 24/25, while slot 34 owns cull plus state 15, so
				// this mandatory per-pass call cannot invalidate either cached
				// output. Keep executing it to preserve the vendor extension's
				// nesting semantics, but retain all four Standard-lite proofs.
				CdeclCall<void>(kBSRenderStateSetAlphaToCoverageEnable,
					ShouldEnableNativeFontVendorAlphaToCoverage(geometry),
					false);
			}
			return true;
		}

		enum class StandardPassLiteFallback : UInt8
		{
			None = 0,
			Envelope,
			Program,
			Renderer,
			Geometry,
			Binding,
			Prelude
		};

		void RecordStandardPassLiteFallback(StandardPassLiteFallback fallback)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteVanillaFallback);
			switch (fallback)
			{
			case StandardPassLiteFallback::Envelope:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackEnvelope);
				break;
			case StandardPassLiteFallback::Program:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackProgram);
				break;
			case StandardPassLiteFallback::Renderer:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackRenderer);
				break;
			case StandardPassLiteFallback::Geometry:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackGeometry);
				break;
			case StandardPassLiteFallback::Binding:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackBinding);
				break;
			case StandardPassLiteFallback::Prelude:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackPrelude);
				break;
			default:
				break;
			}
		}

		StandardPassLiteFallback ResolveStandardPassLiteResidency(
			const NativeFontDrawCommand& command,
			NiGeometryBufferData* preparedBuffer,
			bool packetStatePrevalidated)
		{
			if (!preparedBuffer)
				return StandardPassLiteFallback::Geometry;

			if (!packetStatePrevalidated)
			{
				NiVBChip* chip = preparedBuffer->m_uiStreamCount
						&& preparedBuffer->m_ppkVBChip
					? preparedBuffer->m_ppkVBChip[0] : nullptr;
				if (!chip || !command.binding.active
					|| preparedBuffer->m_hDeclaration
							!= command.binding.declaration
					|| preparedBuffer->m_pkIB
							!= command.binding.indexBuffer
					|| chip->m_pkVB
							!= command.binding.vertexBuffer
					|| preparedBuffer->m_uiBaseVertexIndex
							!= command.binding.baseVertex
					|| preparedBuffer->m_uiVertCount
							!= command.binding.vertexCount)
				{
					return StandardPassLiteFallback::Binding;
				}
			}
			return StandardPassLiteFallback::None;
		}

		void ExecuteStandardPassLite(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry,
			const NativeFontStandardPassLiteDispatch& dispatch,
			const NativeFontDrawCommand& command,
			NiGeometryBufferData* preparedBuffer,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp)
		{
			NiDX9Renderer* renderer = dispatch.renderer;
			TileShader* shader = dispatch.shader;
			const NiPropertyState* properties = dispatch.properties;
			const NativeFontCompiledPacketCommand& program =
				*dispatch.program;
			// InvokeGuardedNativeReplay admits only a completely classified
			// callback table. Unknown injected callbacks return to vanilla B994F0
			// before its prelude or any draw has executed.
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassV2Replay);
			// Retail RenderPassImmediately_Standard first publishes the current
			// property/effect state. Its geometry-group helper is deliberately
			// absent here: formal E88DC0 and the symbolized test build both prove
			// that the exact (skin=null) call has no side effects when
			// modelData->m_pkBuffData is already resident, which stage 2 proved.
			renderer->m_pkCurrProp =
				const_cast<NiPropertyState*>(properties);
			renderer->m_pkCurrEffects = nullptr;

			NativeSegmentDeviceStateCache* deviceState =
				EnterSegmentDeviceStateCache(deviceStateStamp);

			using SetupStateFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			using SetupGeometryRenderStatesFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*, bool);
			using PrepareGeometryFn = NiGeometryBufferData* (__thiscall*)(
				TileShader*, NiGeometry*, UInt32,
				NiGeometryBufferData*, const NiPropertyState*);

			// The formal build and the symbolized test build agree on these
			// disjoint effects:
			//   slot 30: programs, declaration, texture stages and clamp;
			//   slot 32: blend enable/function;
			//   slot 33: alpha-test function/reference;
			//   slot 34: cull mode and alpha-test enable.
			// Slot 31 changes constants, scissor and stencil state only; slot 35
			// restores scissor/stencil only. Standard v2 retains slot 31 as a fifth
			// independent category for packets that require neither transient
			// state, and omits the verified no-op slot 35 for those packets. The
			// former all-or-nothing aggregate made a page texture or Tile alpha
			// change force every slot to run and therefore produced zero reuse in
			// real traversals.
			const bool firstPass = pass->bIsFirst;
			const bool blendApplicable = firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass);
			const bool alphaTestApplicable = firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			const bool renderStatesApplicable = setupRenderStates;

			NativeSegmentPassStateKey passState;
			NativeSegmentBlendStateKey blendState;
			NativeSegmentAlphaTestStateKey alphaState;
			NativeSegmentRenderStatesKey renderStatesKey;
			const bool passKeyReady =
				BuildSegmentPassStateKey(geometry, command, passState);
			const bool passStateReady = deviceState && passKeyReady
				&& deviceState->passReady
				&& SameSegmentPassState(
					deviceState->pass, passState);
			if (passStateReady)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDevicePassReuse);
			}
			else
			{
				reinterpret_cast<SetupStateFn>(
					program.setupGeometryTextures)(shader, properties);
				if (deviceState)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDevicePassSet);
				}
			}
			if (deviceState)
			{
				deviceState->passReady = passKeyReady;
				if (passKeyReady)
					deviceState->pass = passState;
			}

			NativeSegmentConstantsStateKey constantsState;
			bool cleanupRequired = true;
			const bool constantsKeyReady =
				dispatch.standardV2Ready
				&& BuildSegmentConstantsStateKey(
					geometry, renderer, command,
					constantsState, cleanupRequired);
			const NativeSegmentConstantsStateRelation constantsRelation =
				deviceState && constantsKeyReady
					&& deviceState->constantsReady
				? CompareSegmentConstantsState(
					deviceState->constants, constantsState)
				: NativeSegmentConstantsStateRelation::Different;
			const bool constantsStateReady = constantsRelation
				== NativeSegmentConstantsStateRelation::Exact;
			bool constantsLiteApplied = false;
			if (constantsStateReady && cleanupRequired)
			{
				const NativeTileConstantsLiteResult liteResult =
					ApplyNativeTileConstantsLite(geometry, properties);
				constantsLiteApplied = liteResult
					== NativeTileConstantsLiteResult::Applied;
				if (constantsLiteApplied)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::NativeTileConstantsLiteReplay);
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::NativeTileConstantsLiteFallback);
					if (liteResult
						== NativeTileConstantsLiteResult::ScaledScissor)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsLiteScaledScissorFallback);
					}
				}
			}
			bool constantsTranslationLiteApplied = false;
			if (constantsRelation
				== NativeSegmentConstantsStateRelation::TranslationOnly)
			{
				const NativeTileConstantsTranslationLiteResult liteResult =
					ApplyNativeTileConstantsTranslationLite(
						geometry, properties, renderer, program.device);
				constantsTranslationLiteApplied = liteResult
					== NativeTileConstantsTranslationLiteResult::Applied
					|| liteResult
						== NativeTileConstantsTranslationLiteResult::
							AppliedTransient;
				if (constantsTranslationLiteApplied)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeTileConstantsTranslationLiteReplay);
					if (liteResult
						== NativeTileConstantsTranslationLiteResult::
							AppliedTransient)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteTransientReplay);
					}
				}
				else
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeTileConstantsTranslationLiteFallback);
					switch (liteResult)
					{
					case NativeTileConstantsTranslationLiteResult::
						NotApplicable:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteNotApplicableFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::
						ScaledScissor:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteScaledScissorFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::NonFinite:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteNonFiniteFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::DeviceFailure:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteDeviceFailure);
						break;
					default:
						break;
					}
				}
			}
			if ((constantsStateReady && (!cleanupRequired
				|| constantsLiteApplied))
				|| constantsTranslationLiteApplied)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDeviceConstantsReuse);
			}
			else
			{
				reinterpret_cast<SetupStateFn>(
					program.setupGeometryConstants)(shader, properties);
				if (deviceState)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDeviceConstantsSet);
				}
			}
			if (deviceState)
			{
				deviceState->constantsReady =
					constantsKeyReady;
				if (constantsKeyReady)
				deviceState->constants = constantsState;
			}
			if (firstPass)
			{
				if (blendApplicable)
				{
					const bool blendKeyReady =
						BuildSegmentBlendStateKey(
							geometry,
							program.standardBlendSemantics,
							blendState);
					const bool blendStateReady =
						deviceState && blendKeyReady
						&& deviceState->blendReady
						&& SameSegmentBlendState(
							deviceState->blend, blendState);
					if (blendStateReady)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceBlendReuse);
					}
					else
					{
						reinterpret_cast<SetupStateFn>(
							program.setupGeometryAlphaBlending)(
								shader, properties);
						if (deviceState)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									SegmentDeviceBlendSet);
						}
					}
					if (deviceState)
					{
						deviceState->blendReady =
							blendKeyReady;
						if (blendKeyReady)
							deviceState->blend = blendState;
					}
				}
				if (alphaTestApplicable)
				{
					const bool alphaKeyReady =
						BuildSegmentAlphaTestStateKey(
							geometry, alphaState);
					const bool alphaStateReady =
						deviceState && alphaKeyReady
						&& deviceState->alphaTestReady
						&& SameSegmentAlphaTestState(
							deviceState->alphaTest, alphaState);
					if (alphaStateReady)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceAlphaTestReuse);
					}
					else
					{
						reinterpret_cast<SetupStateFn>(
							program.setupGeometryAlphaTesting)(
								shader, properties);
						if (deviceState)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									SegmentDeviceAlphaTestSet);
						}
					}
					if (deviceState)
					{
						deviceState->alphaTestReady =
							alphaKeyReady;
						if (alphaKeyReady)
						{
							deviceState->alphaTest =
								alphaState;
						}
					}
				}
			}
			else if (*reinterpret_cast<UInt8*>(kBSBatchRenderer_bFirstPass)
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass))
			{
				reinterpret_cast<SetupStateFn>(
					program.setupNonFirstPass)(shader, properties);
				*reinterpret_cast<UInt8*>(kBSBatchRenderer_bFirstPass) = 0;
				if (deviceState)
				{
					// Slot 68 changes blend, Z-write and Z-function state
					// after slot 30. The next pass cannot treat the resulting
					// aggregate as a reusable first-pass setup.
					deviceState->InvalidateStates();
				}
			}
			if (renderStatesApplicable)
			{
				const bool renderStatesKeyReady =
					BuildSegmentRenderStatesKey(
						geometry, currentPass, firstPass,
						renderStatesKey);
				const bool renderStatesStateReady =
					deviceState && renderStatesKeyReady
					&& deviceState->renderStatesReady
					&& SameSegmentRenderStates(
						deviceState->renderStates, renderStatesKey);
				if (renderStatesStateReady)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDeviceRenderStatesReuse);
				}
				else
				{
					reinterpret_cast<SetupGeometryRenderStatesFn>(
						program.setupGeometryRenderStates)(
							shader, properties, firstPass);
					if (deviceState)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceRenderStatesSet);
					}
				}
				if (deviceState)
				{
					deviceState->renderStatesReady =
						renderStatesKeyReady;
					if (renderStatesKeyReady)
						deviceState->renderStates = renderStatesKey;
				}
			}

			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteCandidate);
			NativeDirectDrawLiteSubmission directDrawLite;
			const NativeDirectDrawLiteFallback directDrawFailure =
				BuildNativeDirectDrawLiteSubmission(
					geometry, renderer, properties, program, command,
					preparedBuffer, deviceState, directDrawLite);
			bool directDrawArmed = false;
			if (directDrawFailure == NativeDirectDrawLiteFallback::None)
			{
				NativeDirectDrawLiteArmScope directDrawScope(
					geometry, directDrawLite);
				directDrawArmed = directDrawScope.Active();
				if (directDrawArmed)
					NativeFontRenderImmediateAlt(geometry, nullptr, renderer);
			}
			if (!directDrawArmed)
			{
				RecordNativeDirectDrawLiteFallback(
					directDrawFailure == NativeDirectDrawLiteFallback::None
						? NativeDirectDrawLiteFallback::Program
						: directDrawFailure);
				if (deviceState)
					deviceState->geometryBindingReady = false;
				reinterpret_cast<PrepareGeometryFn>(
					program.prepareGeometryForRendering)(
						shader, geometry, 0,
						preparedBuffer, properties);
				NativeFontRenderImmediateAlt(geometry, nullptr, renderer);
			}
			const bool verifiedPost =
				(program.standardV2SlotProofs
					& NativeFontCompiledPacketCommand::
						kStandardSlot35Proof) != 0;
			if (!verifiedPost || cleanupRequired)
			{
				reinterpret_cast<SetupStateFn>(
					program.postGeometry)(shader, properties);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDevicePostSet);
			}
			else
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDevicePostElision);
			}
		}

		bool InvokeGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry, const NativeFontDrawCommand* command,
			bool preferStandardPassLite,
			bool packetStatePrevalidated,
			NiGeometryBufferData* preparedBuffer,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp)
		{
			if (preferStandardPassLite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteCandidate);
			}

			bool guardedEligible = false;
			bool liteEnvelope = false;
			const NativeFontStandardPassLiteDispatch* liteDispatch = nullptr;
			if (preferStandardPassLite)
			{
				liteEnvelope = command
					&& CanUseStandardPassLiteEnvelope(
						pass, currentPass, geometry, *command,
						packetStatePrevalidated, liteDispatch);
				if (liteEnvelope)
				{
					guardedEligible = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::StandardPassLiteStage1Eligible);
					if (!liteDispatch
						|| !liteDispatch->standardV2Ready)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								StandardPassV2CompatibilityReplay);
						RecordStandardPassLiteFallback(
							StandardPassLiteFallback::Program);
						return false;
					}
				}
				else
				{
					RecordStandardPassLiteFallback(
						StandardPassLiteFallback::Envelope);
				}
			}
			if (!guardedEligible && command)
			{
				guardedEligible = CanUseGuardedNativeReplay(
					pass, currentPass, geometry, *command,
					packetStatePrevalidated);
			}
			if (!command || !guardedEligible)
			{
				if (g_bEnableFreeTypeFontCommandBuffer)
					RecordNativeFontCommandFallback(
					NativeFontCommandFallback::State);
				return false;
			}

			bool useStandardPassLite = false;
			if (liteEnvelope)
			{
				const StandardPassLiteFallback liteFailure =
					ResolveStandardPassLiteResidency(
						*command, preparedBuffer,
						packetStatePrevalidated);
				if (liteFailure == StandardPassLiteFallback::None)
				{
					useStandardPassLite = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							StandardPassLiteStage2Resident);
				}
				else
				{
					RecordStandardPassLiteFallback(liteFailure);
				}
			}

			if (!PrepareGuardedNativeReplay(
					pass, currentPass, geometry,
					packetStatePrevalidated && command
						? command->program->shader : nullptr))
			{
				if (useStandardPassLite)
				{
					RecordStandardPassLiteFallback(
						StandardPassLiteFallback::Prelude);
				}
				if (g_bEnableFreeTypeFontCommandBuffer)
					RecordNativeFontCommandFallback(
						NativeFontCommandFallback::State);
				return false;
			}

			if (useStandardPassLite)
			{
				ExecuteStandardPassLite(pass, currentPass,
					testAlpha, blendAlpha, setupRenderStates,
					geometry, *liteDispatch, *command,
					preparedBuffer, deviceStateStamp);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteStage3Replay);
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandNativeReplay);
				return true;
			}

			// The complete vanilla standard path may touch every device-state
			// category. A later dedicated one-packet Tile must establish a new
			// cache head even when it remains in the same validation segment.
			InvalidateSegmentDeviceStateCache();
			using DefaultPassFn = void(__cdecl*)(
				BSShaderProperty::RenderPass*, bool, bool, bool);
			reinterpret_cast<DefaultPassFn>(
				kBSBatchRendererRenderPassImmediatelyStandard)(
				pass, testAlpha, blendAlpha, setupRenderStates);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandNativeReplay);
			return true;
		}

		bool InvokeNativeCommandBootstrap(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry, const NativeFontDrawCommand* command,
			bool preferStandardPassLite = false,
			bool packetStatePrevalidated = false,
			NiGeometryBufferData* preparedBuffer = nullptr,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp = nullptr)
		{
			if (InvokeGuardedNativeReplay(pass, currentPass,
				testAlpha, blendAlpha, setupRenderStates,
				geometry, command, preferStandardPassLite,
				packetStatePrevalidated, preparedBuffer,
				deviceStateStamp))
			{
				return true;
			}
			InvalidateSegmentDeviceStateCache();
			State().originalRenderPassImmediately(pass, currentPass,
				testAlpha, blendAlpha, setupRenderStates);
			return false;
		}

		void RecordRetainedPacketDraw(
			const NativeFontDrawCommand& command, bool retainedExtra)
		{
			if (retainedExtra)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandRetainedBridgeDraw);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
			if (command.packet
				&& command.packet->shaderClass
					== NativeFontShaderClass::Composite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeDraw);
			}
		}

		struct NativeBridgeExecutionContext
		{
			NativeFontCommandSpanView view;
			NiTriShape* facade = nullptr;
			NativeFontShapePayload* payload = nullptr;
			NativeFontRingSubmission* ringSubmission = nullptr;
			UInt32 bootstrapCommandOffset = 0;
			UInt32 nextCommandOffset = 0;
			UInt32 endCommandOffset = 0;
			void* boundProfile = nullptr;
			NativeFontCommandBindState bindState;
			UInt32 drewPackets = 0;
			NativeFontFallbackReason failure =
				NativeFontFallbackReason::RuntimeFault;
			const char* operation = "retained-bridge";
			HRESULT result = E_FAIL;
			bool packetStatePrevalidated = false;
			bool failed = false;
			bool constantStateFault = false;
		};

		void FailRetainedBridge(NativeBridgeExecutionContext& context,
			NativeFontFallbackReason failure, const char* operation,
			HRESULT result, bool constantStateFault = false)
		{
			context.failure = failure;
			context.operation = operation;
			context.result = result;
			context.constantStateFault = constantStateFault;
			context.failed = true;
		}

		bool DrawRetainedBridgeCommand(
			NativeBridgeExecutionContext& context,
			const NativeFontDrawCommand& command, UInt32 commandOffset,
			NiRenderer* renderer, bool alternate)
		{
			NiTriShape* geometry = nullptr;
			if (!context.facade || !context.payload
				|| !context.ringSubmission
				|| command.packetIndex != commandOffset)
			{
				FailRetainedBridge(context,
					NativeFontFallbackReason::PacketBuild,
					"retained-command-order", E_FAIL);
				return false;
			}
			const NativeFontFallbackReason prepare =
				PrepareNativeFontRingPacket(context.facade,
					*context.payload, *context.ringSubmission,
					command.packetIndex, geometry);
			if (prepare != NativeFontFallbackReason::None || !geometry)
			{
				FailRetainedBridge(context,
					prepare != NativeFontFallbackReason::None
						? prepare
						: NativeFontFallbackReason::RuntimeFault,
					"retained-ring-packet", E_FAIL);
				return false;
			}

			auto drawGeometry = [&]() -> bool
			{
				const bool commandValid =
					context.packetStatePrevalidated
						? GuardNativeFontCommand(
							context.view.spanIndex, commandOffset,
							geometry, renderer)
						: ValidateNativeFontCommand(
							context.view.spanIndex, commandOffset,
							geometry, renderer);
				if (!commandValid)
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::PacketPrepare,
						"retained-command-binding", E_FAIL);
					return false;
				}
				const bool publishPrograms =
					context.boundProfile != command.program->profile;
				const char* operation = "none";
				HRESULT result = D3D_OK;
				if (!BindNativeFontCommandPacket(*command.program,
					command.atlasTexture, publishPrograms,
					&geometry->m_kProperties,
					context.bindState,
					operation, result))
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::RuntimeFault,
						operation, result, true);
					return false;
				}
				context.boundProfile = command.program->profile;
				RenderImmediateFn immediate = alternate
					? State().originalRenderImmediateAlt
					: State().originalRenderImmediate;
				if (!immediate)
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::RuntimeFault,
						"retained-immediate-missing", E_FAIL);
					return false;
				}
				immediate(geometry, renderer);
				++context.drewPackets;
				RecordRetainedPacketDraw(command, true);
				if (!IsNativeFontShaderGenerationCurrent(
					command.program->generation))
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::DeviceReset,
						"retained-generation-after-draw",
						D3DERR_DEVICELOST);
					return false;
				}
				return true;
			};

			return drawGeometry();
		}

		bool ContinueRetainedBridge(
			void* opaque, NiRenderer* renderer, bool alternate)
		{
			auto* context =
				static_cast<NativeBridgeExecutionContext*>(opaque);
			if (!context || context->failed)
				return false;
			while (context->nextCommandOffset
				< context->endCommandOffset)
			{
				const UInt32 commandOffset =
					context->nextCommandOffset++;
				const NativeFontDrawCommand* command =
					ResolveNativeCommand(context->view, commandOffset);
				if (!command || !DrawRetainedBridgeCommand(
					*context, *command, commandOffset,
					renderer, alternate))
				{
					return false;
				}
			}
			const NativeFontDrawCommand* bootstrap =
				ResolveNativeCommand(
					context->view, context->bootstrapCommandOffset);
			if (!bootstrap || !bootstrap->program)
			{
				FailRetainedBridge(*context,
					NativeFontFallbackReason::PacketBuild,
					"retained-bootstrap-command", E_FAIL);
				return false;
			}
			// B994F0's global selected-shader identity still names the bootstrap
			// profile. Restore the corresponding D3D program/texture before its
			// one vanilla slot-35 cleanup returns, otherwise the next Tile can skip
			// B99390 while the driver still has the final retained profile bound.
			const char* operation = "none";
			HRESULT result = D3D_OK;
			NiTriShape* bootstrapGeometry = context->ringSubmission
				? context->ringSubmission->proxyShape : nullptr;
			if (!BindNativeFontCommandPacket(*bootstrap->program,
				bootstrap->atlasTexture,
				context->boundProfile
					!= bootstrap->program->profile,
				bootstrapGeometry
					? &bootstrapGeometry->m_kProperties : nullptr,
				context->bindState,
				operation, result))
			{
				FailRetainedBridge(*context,
					NativeFontFallbackReason::RuntimeFault,
					operation, result, true);
				return false;
			}
			context->boundProfile = bootstrap->program->profile;
			return true;
		}

		bool ResolveNextBridgeGroup(
			const NativeFontCommandSpanView& view, UInt32& runCursor,
			UInt32& firstCommandOffset, UInt32& endCommandOffset)
		{
			if (!view.span || !view.runs
				|| runCursor >= view.span->runCount)
			{
				return false;
			}
			const UInt32 spanFirst = view.span->firstCommand;
			const UInt32 spanEnd =
				spanFirst + view.span->commandCount;
			const NativeFontFrameCommandRun& firstRun =
				view.runs[view.span->firstRun + runCursor];
			if (!firstRun.commandCount
				|| firstRun.firstCommand < spanFirst
				|| firstRun.firstCommand + firstRun.commandCount
					> spanEnd)
			{
				return false;
			}
			UInt32 end = firstRun.firstCommand
				+ firstRun.commandCount;
			++runCursor;
			while (runCursor < view.span->runCount)
			{
				const NativeFontFrameCommandRun& next =
					view.runs[view.span->firstRun + runCursor];
				if (!next.continuesBridgeSpan)
					break;
				if (!next.commandCount || next.firstCommand != end
					|| next.firstCommand + next.commandCount
						> spanEnd)
				{
					return false;
				}
				end = next.firstCommand + next.commandCount;
				++runCursor;
			}
			firstCommandOffset = firstRun.firstCommand - spanFirst;
			endCommandOffset = end - spanFirst;
			return firstCommandOffset < endCommandOffset;
		}

		bool TryDrawNativeRetainedSpan(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			UInt32 commandSpanIndex,
			NativePacketDrawResult& draw)
		{
			if (!g_bEnableFreeTypeFontCommandBuffer
				|| !pass || !facade
				|| commandSpanIndex == kInvalidNativeFontCommandIndex)
			{
				return false;
			}
			if (currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass))
			{
				RecordNativeFontCommandFallback(
					NativeFontCommandFallback::State);
				return false;
			}

			NativeFontCommandSpanView view;
			if (!BeginNativeFontCommandSpanExecution(
				commandSpanIndex, facade, view))
			{
				return false;
			}
			if (!view.span || view.span->payload != &payload
				|| view.span->commandCount < 2
				|| !view.span->bridgeEligible)
			{
				EndNativeFontCommandSpanExecution(
					commandSpanIndex, false, false);
				RecordNativeFontCommandFallback(
					NativeFontCommandFallback::Topology);
				return false;
			}

			FreeTypePerfScope submitPerf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit);
			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NativeRingSubmissionScope ringScope;
			const NativeFontFallbackReason ringFailure =
				BeginNativeFontRingSubmission(
					facade, payload, ringScope.submission);
			if (ringFailure != NativeFontFallbackReason::None)
			{
				EndNativeFontCommandSpanExecution(
					commandSpanIndex, false, false);
				return false;
			}

			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_constantOwnershipBatch.FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (isolatePacketConstants)
			{
				shaderBatch.emplace();
				if (!device)
				draw.runtimeFault = true;
				else if (batchedConstants)
				{
					if (!s_constantOwnershipBatch.EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation =
							s_constantOwnershipBatch.Operation();
						draw.result = s_constantOwnershipBatch.Result();
						draw.mismatchRegister =
							s_constantOwnershipBatch.MismatchRegister();
					}
				}
				else
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation =
							localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			NativeTilePacketScope packetScope(pass);
			UInt32 runCursor = 0;
			while (!draw.runtimeFault
				&& runCursor < view.span->runCount)
			{
				UInt32 firstOffset = 0;
				UInt32 endOffset = 0;
				if (!ResolveNextBridgeGroup(
					view, runCursor, firstOffset, endOffset))
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::PacketBuild;
					draw.operation = "retained-run-topology";
					draw.result = E_FAIL;
					break;
				}
				const NativeFontDrawCommand* first =
					ResolveNativeCommand(view, firstOffset);
				NiTriShape* proxy = nullptr;
				if (!first || first->packetIndex != firstOffset)
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::PacketBuild;
					draw.operation = "retained-first-command";
					draw.result = E_FAIL;
					break;
				}
				const NativeFontFallbackReason prepare =
					PrepareNativeFontRingPacket(facade, payload,
						ringScope.submission,
						first->packetIndex, proxy);
				if (prepare != NativeFontFallbackReason::None
					|| !proxy)
				{
					draw.runtimeFault = true;
					draw.failure =
						prepare != NativeFontFallbackReason::None
							? prepare
							: NativeFontFallbackReason::RuntimeFault;
					draw.operation = "retained-first-packet";
					draw.result = E_FAIL;
					break;
				}

				packetScope.Select(proxy);
				NativeImmediateHookVtableScope hookVtable(proxy);
				if (!hookVtable.Active())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::RuntimeFault;
					draw.operation = "retained-immediate-vtable";
					draw.result = E_FAIL;
					break;
				}

				NativeBridgeExecutionContext bridge;
				bridge.view = view;
				bridge.facade = facade;
				bridge.payload = &payload;
				bridge.ringSubmission = &ringScope.submission;
				bridge.bootstrapCommandOffset = firstOffset;
				bridge.nextCommandOffset = firstOffset + 1u;
				bridge.endCommandOffset = endOffset;
				bridge.boundProfile = first->program
					? first->program->profile : nullptr;
				bridge.bindState = MakeNativeCommandBindState(
					pass, currentPass, false, true,
					setupRenderStates);
				// PrepareNativeFontRingPacket just established the exact retained
				// packet binding on this private proxy. The immediate callback
				// only needs the mutation-epoch guard; rereading the full proxy
				// descriptor would duplicate the packet preparation proof.
				bridge.packetStatePrevalidated = true;
				const bool hasContinuation =
					bridge.nextCommandOffset
						< bridge.endCommandOffset;
				NativeDirectImmediateScope immediateScope(
					proxy, commandSpanIndex, firstOffset, true,
					hasContinuation ? &bridge : nullptr,
					hasContinuation
						? &ContinueRetainedBridge : nullptr,
					NativeImmediateCommandKind::SpanPacket, true);
				InvokeNativeCommandBootstrap(pass, currentPass,
					false, true, setupRenderStates, proxy, first,
					false, true);
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordRetainedPacketDraw(*first, false);
				}
				if (bridge.drewPackets)
				{
					draw.drewPacket = true;
					draw.drawnPacketCount += bridge.drewPackets;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandVanillaBootstrapSaved,
						bridge.drewPackets);
				}
				if (!immediateScope.Invoked()
					|| !immediateScope.ValidationPassed()
					|| !immediateScope.Drew())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::RuntimeFault;
					draw.operation =
						"retained-bootstrap-immediate";
					draw.result = E_FAIL;
					break;
				}
				if (!immediateScope.ContinuationSucceeded()
					|| bridge.failed)
				{
					draw.runtimeFault = true;
					draw.failure = bridge.failure;
					draw.operation = bridge.operation;
					draw.result = bridge.result;
					draw.constantStateFault =
						bridge.constantStateFault;
					break;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"retained-command-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}

			const bool success = !draw.runtimeFault
				&& draw.drawnPacketCount == view.span->commandCount;
			EndNativeFontCommandSpanExecution(
				commandSpanIndex, success, draw.drewPacket);
			if (success)
				return true;
			// No command reached the driver: the unmodified current path can
			// safely acquire a fresh proxy and render the complete payload.
			return draw.drewPacket || draw.constantStateFault;
		}

		bool TryDrawSingletonFacadePacket(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* shape,
			const NativeFontShapeMetadata& metadata, UInt64 validationToken,
			NativePacketDrawResult& draw,
			UInt32 directFacadeSinglePacketCommandIndex =
				kInvalidNativeFontCommandIndex)
		{
			SingletonFacadeState* singleton =
				GetSingletonFacadeState(metadata);
			if (!pass || !shape || !singleton
				|| validationToken == 0)
			{
				return false;
			}
			FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				directFacadeSinglePacketCommandIndex
						!= kInvalidNativeFontCommandIndex);

			const NativeFontShapePayload* payload = &metadata.nativePayload;
			const NativeFontPacketTemplate* packet = nullptr;
			NativeFontDirectFacadePacketBinding binding;
			NiGeometryBufferData* expectedBuffer = nullptr;
			NiVBChip* expectedChip = nullptr;
			TileShader* expectedShader = nullptr;
			UInt32 packetIndex = 0;
			auto captureBinding = [&](const SingletonFacadeBinding& slot)
			{
				if (!payload || !payload->buildComplete
					|| !payload->payloadTemplate)
				{
					return false;
				}
				const std::vector<NativeFontPacketTemplate>& packets =
					GetNativeFontPackets(*payload->payloadTemplate,
						payload->useCompositePackets);
				if (slot.shape != shape || slot.packetIndex >= packets.size()
					|| slot.packetIndex >= payload->packetShaders.size())
				{
					return false;
				}
				packet = &packets[slot.packetIndex];
				packetIndex = slot.packetIndex;
				expectedBuffer = slot.bindingBuffer;
				expectedChip = slot.bindingChip;
				expectedShader = payload->packetShaders[slot.packetIndex];
				binding.vertexBuffer = slot.bindingChip
					? slot.bindingChip->m_pkVB : nullptr;
				binding.indexBuffer = slot.bindingBuffer
					? slot.bindingBuffer->m_pkIB : nullptr;
				binding.declaration = slot.bindingBuffer
					? static_cast<IDirect3DVertexDeclaration9*>(
						slot.bindingBuffer->m_hDeclaration) : nullptr;
				binding.baseVertex = slot.baseVertex;
				binding.vertexCount = slot.vertexCount;
				binding.indexBytes = slot.bindingBuffer
					? slot.bindingBuffer->m_uiIBSize : 0;
				binding.generation = slot.generation;
				binding.resourceSerial = slot.resourceSerial;
				binding.uploadEpoch = slot.uploadEpoch;
				binding.atlasTextureEpoch = slot.atlasTextureEpoch;
				binding.staticResident = slot.staticResident;
				binding.active = slot.bound;
				return true;
			};
			if (singleton->slot.shape != shape
				|| singleton->preparedValidationToken != validationToken
				|| singleton->frameMode.load(std::memory_order_acquire)
					!= SingletonFacadeFrameMode::Direct)
			{
				return false;
			}
			if (!captureBinding(singleton->slot))
				return false;

			NativeFontDirectFacadeSinglePacketCommandView
				directFacadeCommandView;
			const NativeFontDrawCommand* command = nullptr;
			bool commandExecution = false;
			bool commandBegun = false;
			bool directFacadeCommandExecution = false;
			if (g_bEnableFreeTypeFontCommandBuffer
				&& directFacadeSinglePacketCommandIndex
					!= kInvalidNativeFontCommandIndex)
			{
				commandBegun =
					BeginNativeFontDirectFacadeSinglePacketCommandExecution(
						directFacadeSinglePacketCommandIndex,
						&metadata, shape, directFacadeCommandView);
				if (commandBegun
					&& directFacadeCommandView.command)
				{
					command =
						directFacadeCommandView.command->draw;
					commandExecution = command
						&& command->payload == payload
						&& command->expectedGeometry == shape
						&& command->packetIndex == packetIndex;
					directFacadeCommandExecution =
						commandExecution;
				}
				if (commandBegun && !commandExecution)
				{
					EndNativeFontDirectFacadeSinglePacketCommandExecution(
						directFacadeSinglePacketCommandIndex,
						false, false);
				}
			}
			draw.directShapeRoute = true;
			draw.vanillaLikeBitmapRoute = payload->vanillaLikeBitmapPackets;
			NiTriShapeData* data = shape->GetModelData();
			const bool bindingDescriptorCurrent =
				data && data->m_pkBuffData == expectedBuffer
				&& shape->GetShader() == expectedShader
				&& expectedBuffer && expectedChip
				&& expectedBuffer->m_hDeclaration == binding.declaration
				&& expectedBuffer->m_pkIB == binding.indexBuffer
				&& expectedBuffer->m_uiBaseVertexIndex
					== binding.baseVertex
				&& expectedBuffer->m_uiVertCount
					== binding.vertexCount
				&& expectedBuffer->m_uiMaxVertCount
					== binding.vertexCount
				&& expectedBuffer->m_uiStreamCount == 1
				&& expectedBuffer->m_puiVertexStride
				&& expectedBuffer->m_puiVertexStride[0]
					== sizeof(NativeFontGpuVertex)
				&& expectedBuffer->m_ppkVBChip
				&& expectedBuffer->m_ppkVBChip[0] == expectedChip
				&& expectedBuffer->m_uiIndexCount
					== packet->vertexCount / 4u * 6u
				&& expectedBuffer->m_uiIBSize == binding.indexBytes
				&& expectedBuffer->m_eType == D3DPT_TRIANGLELIST
				&& expectedBuffer->m_uiTriCount
					== packet->vertexCount / 4u * 2u
				&& expectedBuffer->m_uiMaxTriCount
					== packet->vertexCount / 4u * 2u
				&& expectedBuffer->m_uiNumArrays == 1
				&& expectedChip->m_pkVB == binding.vertexBuffer
				&& expectedChip->m_uiOffset == 0
				&& expectedChip->m_uiSize
					== binding.vertexCount
						* sizeof(NativeFontGpuVertex)
				&& binding.vertexCount == packet->vertexCount
				&& IsNativeFontDirectFacadePacketAtlasCurrent(
					shape, *payload, packetIndex);
			const bool bindingCurrent = bindingDescriptorCurrent
				&& (commandExecution
					? command
						&& SameNativePacketBinding(
							command->binding, binding)
					: validationToken
							== GetNativeFontSortedFrameValidationToken()
						&& IsNativeFontDirectFacadePacketBindingCurrent(
							binding));
			if (!bindingCurrent)
			{
				draw.runtimeFault = true;
				draw.failure = NativeFontFallbackReason::PacketPrepare;
				draw.operation = "singleton-facade-binding";
				draw.result = E_FAIL;
			}

			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.runtimeFault && !draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_constantOwnershipBatch.FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				if (batchedConstants)
				{
					if (!s_constantOwnershipBatch.EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_constantOwnershipBatch.Operation();
						draw.result = s_constantOwnershipBatch.Result();
						draw.mismatchRegister =
							s_constantOwnershipBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			if (!draw.runtimeFault)
			{
				SingletonFacadeTileStateScope tileState(
					shape, *payload, *packet);
				if (!tileState.Active())
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::PropertySync;
					draw.operation = "singleton-facade-tile-state";
					draw.result = E_FAIL;
				}
				else
				{
					bool usedNativeReplay = false;
					const UInt32 validationCommandIndex =
						commandExecution
							? directFacadeSinglePacketCommandIndex
							: kInvalidNativeFontCommandIndex;
					NativeDirectImmediateScope immediateScope(
						shape, validationCommandIndex,
						kInvalidNativeFontCommandIndex,
						commandExecution, nullptr, nullptr,
						NativeImmediateCommandKind::DirectFacadeSinglePacket,
						commandExecution && bindingCurrent);
					if (commandExecution)
					{
						NativeFontSegmentDeviceStateStamp
							segmentDeviceStateStamp;
						const NativeFontSegmentDeviceStateStamp*
							segmentDeviceState = nullptr;
						if (directFacadeCommandExecution
							&& directFacadeCommandView.stamp
							&& directFacadeCommandView.command
							&& BuildSegmentDeviceStateStamp(
								directFacadeCommandView.stamp,
								directFacadeCommandView.command->
									executionSegmentEpoch,
								directFacadeCommandView.command->
									executionExternalMutationEpoch,
								segmentDeviceStateStamp))
						{
							segmentDeviceState =
								&segmentDeviceStateStamp;
						}
						usedNativeReplay =
							InvokeNativeCommandBootstrap(pass,
							currentPass, false, true,
							setupRenderStates, shape, command,
							directFacadeCommandExecution,
							bindingCurrent,
							expectedBuffer,
							segmentDeviceState);
					}
					else
					{
						InvalidateSegmentDeviceStateCache();
						State().originalRenderPassImmediately(pass,
							currentPass, false, true,
							setupRenderStates);
					}
					if (immediateScope.Drew())
					{
						draw.drewPacket = true;
						draw.drawnPacketCount = 1;
						singleton->directDrawCount.fetch_add(
							1, std::memory_order_acq_rel);
						RecordFreeTypePerf(
							FreeTypePerfCounter::TilePass);
						if (usedNativeReplay
							&& directFacadeCommandExecution)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									CommandDirectFacadeSinglePacketReplay);
						}
						if (packet->shaderClass
							== NativeFontShaderClass::Composite)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::CompositeDraw);
						}
					}
					else
					{
						draw.runtimeFault = true;
						draw.failure =
							NativeFontFallbackReason::RuntimeFault;
						draw.operation = immediateScope.Invoked()
							? "singleton-facade-command-validation"
							: "singleton-facade-immediate-not-invoked";
						draw.result = E_FAIL;
					}
				}
				if (!IsNativeFontShaderGenerationCurrent(
					payload->preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::DeviceReset;
					draw.operation =
						"generation-changed-after-singleton-facade";
					draw.result = D3DERR_DEVICELOST;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"singleton-facade-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}
			if (directFacadeCommandExecution)
			{
				EndNativeFontDirectFacadeSinglePacketCommandExecution(
					directFacadeSinglePacketCommandIndex,
					!draw.runtimeFault && draw.drewPacket,
					draw.drewPacket);
			}
			if (draw.runtimeFault)
			{
				const bool facadeFallbackSafe =
					!draw.drewPacket && !draw.constantStateFault
					&& singleton->directDrawCount.load(
						std::memory_order_acquire) == 0;
				if (facadeFallbackSafe)
				{
					RestoreSingletonFacade(metadata, draw.failure);
				}
				else
				{
					if (singleton->frameMode.load(
							std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Retired)
					{
						singleton->frameMode.store(
							SingletonFacadeFrameMode::Fault,
							std::memory_order_release);
					}
					RecordFreeTypePerf(FreeTypePerfCounter::
						SingletonFacadeFallback);
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeDirectFrame);
			if (draw.runtimeFault && draw.drewPacket)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadePartialFault);
			}
			return true;
		}

		NativePacketDrawResult DrawNativePacketSet(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			UInt32 commandSpanIndex =
				kInvalidNativeFontCommandIndex)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeFontCommandIndex);
			InvalidateSegmentDeviceStateCache();
			NativePacketDrawResult draw;
			NativeRingSubmissionScope ringScope;
			NativeFontFallbackReason failure = BeginNativeFontRingSubmission(
				facade, payload, ringScope.submission);
			if (failure != NativeFontFallbackReason::None)
			{
				draw.runtimeFault = true;
				draw.failure = failure;
				draw.operation = "ring-submission";
			}
			NativeTilePacketScope packetScope(pass);
			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}
			// Retail NiDX9Renderer's indexed-array loop cannot express
			// triangle-list page boundaries: m_pusArrayLengths is interpreted as
			// strip lengths (primitiveCount = length - 2). Keep the page split at
			// the one vanilla sorted Tile callsite instead. Each packet selects a
			// FreeType text-shape proxy and then executes the untouched
			// RenderPassImmediately -> NiTriShape::RenderImmediate renderer path.
			//
			// Final ARGB and baked-coverage bitmaps use only vanilla c0. Skipping the
			// distance-field private-high-range ownership and facade bookkeeping
			// removes the only per-facade isolation work from this vanilla-like
			// multipage route.
			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_constantOwnershipBatch.FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				// Retail 0xB64F90 calls RenderPassImmediately once per sorted item
				// with no intervening draw. Own PS c176-c183 and VS c208 for the
				// contiguous native segment.
				if (batchedConstants)
				{
					if (!s_constantOwnershipBatch.EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_constantOwnershipBatch.Operation();
						draw.result = s_constantOwnershipBatch.Result();
						draw.mismatchRegister =
							s_constantOwnershipBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
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
					const NativeFontFallbackReason packetFailure =
						PrepareNativeFontRingPacket(facade, payload,
							ringScope.submission, packetIndex, proxyShape);
					if (packetFailure != NativeFontFallbackReason::None
						|| !proxyShape)
					{
						draw.runtimeFault = true;
						draw.failure =
							packetFailure != NativeFontFallbackReason::None
								? packetFailure
								: NativeFontFallbackReason::RuntimeFault;
						draw.operation = "ring-packet";
						break;
					}
					packetScope.Select(proxyShape);
					State().originalRenderPassImmediately(pass,
						currentPass, false, true,
						setupRenderStates);
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					const std::vector<NativeFontPacketTemplate>& activePackets =
						GetNativeFontPackets(*payload.payloadTemplate,
							payload.useCompositePackets);
					if (packetIndex < activePackets.size()
						&& activePackets[packetIndex].shaderClass
							== NativeFontShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
					if (!IsNativeFontShaderGenerationCurrent(
						payload.preparedGeneration))
					{
						draw.runtimeFault = true;
						break;
					}
				}
				if (isolatePacketConstants && !batchedConstants
					&& localConstants
					&& !localConstants->Release())
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
				&& !ReleaseNativeConstantOwnershipBatch("native-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}
			return draw;
		}

		bool TryDrawNativeSinglePacketDirect(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			NativePacketDrawResult& draw,
			UInt32 commandSpanIndex =
				kInvalidNativeFontCommandIndex,
			UInt32 singlePacketCommandIndex =
				kInvalidNativeFontCommandIndex)
		{
			if (!pass || !facade || !payload.buildComplete
				|| !payload.payloadTemplate
				|| payload.packetShaders.size() != 1)
			{
				return false;
			}
			const NativeFontPayloadTemplate& artifact =
				*payload.payloadTemplate;
			const std::vector<NativeFontPacketTemplate>& packets =
				GetNativeFontPackets(artifact, payload.useCompositePackets);
			if (packets.size() != 1 || !packets[0].vertexCount
				|| (packets[0].vertexCount & 3u)
				|| static_cast<UInt64>(packets[0].firstVertex)
					+ packets[0].vertexCount
						> artifact.gpuVertices.size()
				|| packets[0].atlasPage
					>= artifact.atlasProperties.size()
				|| packets[0].atlasPage
					>= artifact.atlasTextures.size())
			{
				return false;
			}
			const bool expandedDirectEligibility =
				artifact.pageCount != 1
				|| packets[0].atlasPage != 0
				|| packets[0].firstVertex != 0
				|| packets[0].vertexCount
					!= artifact.gpuVertices.size();

			FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeFontCommandIndex
					|| singlePacketCommandIndex
						!= kInvalidNativeFontCommandIndex);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectCandidate);

			const bool commandRequested =
				g_bEnableFreeTypeFontCommandBuffer
				&& (singlePacketCommandIndex
						!= kInvalidNativeFontCommandIndex
					|| commandSpanIndex
						!= kInvalidNativeFontCommandIndex);
			NativeFontCommandSpanView commandView;
			NativeFontSinglePacketCommandView singleCommandView;
			const NativeFontDrawCommand* command = nullptr;
			bool commandExecution = false;
			bool singleCommandExecution = false;
			if (commandRequested
				&& singlePacketCommandIndex
					!= kInvalidNativeFontCommandIndex
				&& BeginNativeFontSinglePacketCommandExecution(
					singlePacketCommandIndex, facade,
					singleCommandView))
			{
				if (singleCommandView.command
					&& singleCommandView.command->payload == &payload)
				{
					command = &singleCommandView.command->draw;
					commandExecution = command->program
						&& command->packet == &packets[0]
						&& command->binding.active
						&& command->binding.vertexCount
							== packets[0].vertexCount;
					singleCommandExecution = commandExecution;
				}
				if (!commandExecution)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandSinglePacketFallback);
					RecordNativeFontCommandFallback(
						NativeFontCommandFallback::Topology);
					EndNativeFontSinglePacketCommandExecution(
						singlePacketCommandIndex, false, false);
				}
			}
			else if (commandRequested
				&& commandSpanIndex
					!= kInvalidNativeFontCommandIndex
				&& BeginNativeFontCommandSpanExecution(
					commandSpanIndex, facade, commandView))
			{
				if (commandView.span
					&& commandView.span->payload == &payload
					&& commandView.span->commandCount == 1)
				{
					command = ResolveNativeCommand(commandView, 0);
					commandExecution = command && command->program
						&& command->packet == &packets[0]
						&& command->binding.active
						&& command->binding.vertexCount
							== packets[0].vertexCount;
				}
				if (!commandExecution)
				{
					EndNativeFontCommandSpanExecution(
						commandSpanIndex, false, false);
				}
			}

			auto endCommandExecution =
				[&](bool success, bool drewPacket)
			{
				if (singleCommandExecution)
				{
					EndNativeFontSinglePacketCommandExecution(
						singlePacketCommandIndex,
						success, drewPacket);
				}
				else
				{
					EndNativeFontCommandSpanExecution(
						commandSpanIndex, success, drewPacket);
				}
			};

			NativeDirectShapeSubmissionScope submissionScope;
			std::optional<NativeDirectShapeBinding> binding;
			UInt32 submittedVertexCount = 0;
			if (commandExecution)
			{
				binding.emplace(facade, payload, command->binding);
				submittedVertexCount = command->binding.vertexCount;
			}
			else
			{
				const NativeFontFallbackReason submissionFailure =
					BeginNativeFontDirectShapeSubmission(
						facade, payload, submissionScope.submission);
				if (submissionFailure != NativeFontFallbackReason::None)
				{
					RecordSinglePacketDirectFallback(
						commandRequested
							? FreeTypePerfCounter::
								SinglePacketDirectFallbackCommand
							: FreeTypePerfCounter::
								SinglePacketDirectFallbackSubmission);
					return false;
				}
				binding.emplace(facade, payload,
					submissionScope.submission);
				submittedVertexCount =
					submissionScope.submission.vertexCount;
			}
			if (!binding || !binding->Active())
			{
				if (commandExecution)
				{
					if (singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketFallback);
						RecordNativeFontCommandFallback(
							NativeFontCommandFallback::Resource);
					}
					endCommandExecution(false, false);
				}
				RecordSinglePacketDirectFallback(
					binding
						? DirectShapeBindingFallbackCounter(
							binding->Failure())
						: FreeTypePerfCounter::
							SinglePacketDirectFallbackBindingInput);
				return false;
			}

			draw.directShapeRoute = true;
			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_constantOwnershipBatch.FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				if (batchedConstants)
				{
					if (!s_constantOwnershipBatch.EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_constantOwnershipBatch.Operation();
						draw.result = s_constantOwnershipBatch.Result();
						draw.mismatchRegister =
							s_constantOwnershipBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			if (!draw.runtimeFault)
			{
				const UInt32 validationCommandIndex =
					commandExecution
						? (singleCommandExecution
							? singlePacketCommandIndex
							: commandSpanIndex)
						: kInvalidNativeFontCommandIndex;
				NativeDirectImmediateScope immediateScope(
					facade, validationCommandIndex,
					commandExecution && !singleCommandExecution
						? 0u : kInvalidNativeFontCommandIndex,
					commandExecution, nullptr, nullptr,
					singleCommandExecution
						? NativeImmediateCommandKind::SinglePacket
						: NativeImmediateCommandKind::SpanPacket,
					commandExecution);
				bool usedNativeReplay = false;
				if (commandExecution)
				{
					NativeFontSegmentDeviceStateStamp
						segmentDeviceStateStamp;
					const NativeFontSegmentDeviceStateStamp*
						segmentDeviceState = nullptr;
					if (singleCommandExecution
						&& singleCommandView.stamp
						&& singleCommandView.command
						&& BuildSegmentDeviceStateStamp(
							singleCommandView.stamp,
							singleCommandView.command->
								executionSegmentEpoch,
							singleCommandView.command->
								executionExternalMutationEpoch,
							segmentDeviceStateStamp))
					{
						segmentDeviceState =
							&segmentDeviceStateStamp;
					}
					usedNativeReplay =
						InvokeNativeCommandBootstrap(pass, currentPass,
						false, true, setupRenderStates,
						facade, command, singleCommandExecution,
						true, binding->Buffer(),
						segmentDeviceState);
					if (!usedNativeReplay
						&& singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketFallback);
					}
				}
				else
				{
					InvalidateSegmentDeviceStateCache();
					State().originalRenderPassImmediately(pass,
						currentPass, false, true,
						setupRenderStates);
				}
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					draw.drawnPacketCount = 1;
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectDraw);
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectVertex,
						submittedVertexCount);
					if (usedNativeReplay
						&& expandedDirectEligibility)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandDirectRangeReplay);
					}
					if (usedNativeReplay
						&& singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketReplay);
					}
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					if (packets[0].shaderClass
						== NativeFontShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
				}
				else
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::RuntimeFault;
					draw.operation = immediateScope.Invoked()
						? "direct-shape-command-validation"
						: "direct-shape-immediate-not-invoked";
					draw.result = E_FAIL;
				}
				if (!IsNativeFontShaderGenerationCurrent(
					payload.preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::RuntimeFault;
					draw.operation =
						"generation-changed-after-direct-shape";
					draw.result = D3DERR_DEVICELOST;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"direct-shape-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}
			if (draw.runtimeFault)
			{
				RecordSinglePacketDirectFallback(
					FreeTypePerfCounter::
						SinglePacketDirectFallbackRuntime);
			}
			if (commandExecution)
			{
				const bool success =
					!draw.runtimeFault && draw.drewPacket;
				endCommandExecution(success, draw.drewPacket);
				if (!success && !draw.drewPacket
					&& !draw.constantStateFault)
				{
					return false;
				}
			}
			// Once the vanilla Tile pass has been entered, this route owns the item
			// even if the immediate callback was unexpectedly skipped. Replaying
			// through a proxy could duplicate a draw whose driver result is opaque.
			return true;
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
			const NativeFontShapeMetadataPtr metadata = FindNativeFontShapeMetadata(shape);
			if (!metadata)
			{
				LogMissingMetadata(shape, phase);
				return;
			}
			RecordNativeFontSuppression(shape, *metadata,
				metadata->nativePayload.buildComplete
					? NativeFontFallbackReason::DirectImmediate
					: NativeFontFallbackReason::PacketBuild,
				phase);
		}

		struct NativeFontMetadataIdentitySnapshot
		{
			UInt64 allocationId = 0;
			const NativeFontShapeMetadata* selfIdentity = nullptr;
			const NiTriShape* shapeIdentity = nullptr;
			UInt32 fontId = 0;
			UInt32 backend = 0;
			bool buildComplete = false;
		};

		bool TryReadNativeFontMetadataIdentity(
			const NativeFontShapeMetadata* metadata,
			NativeFontMetadataIdentitySnapshot& snapshot)
		{
			if (!metadata)
				return false;
			__try
			{
				snapshot.allocationId = metadata->allocationId;
				snapshot.selfIdentity = metadata->selfIdentity;
				snapshot.shapeIdentity = metadata->shapeIdentity;
				snapshot.fontId = metadata->fontId;
				snapshot.backend =
					static_cast<UInt32>(metadata->backend);
				snapshot.buildComplete =
					metadata->nativePayload.buildComplete;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool LogNativeFontMetadataDeleteAudit(NiTriShape* shape,
			bool registryFound, const NativeFontShapeMetadataEntry& entry)
		{
			const NativeFontShapeMetadata* mappedMetadata =
				entry.metadata.get();
			NativeFontMetadataIdentitySnapshot snapshot;
			const bool readable = TryReadNativeFontMetadataIdentity(
				mappedMetadata, snapshot);
			const bool registryIdentityValid =
				registryFound && entry.allocationId
				&& entry.selfIdentity
				&& entry.shapeIdentity == shape;
			const bool pointerMatch =
				mappedMetadata == entry.selfIdentity;
			const bool allocationMatch = readable
				&& snapshot.allocationId == entry.allocationId;
			const bool selfMatch = readable
				&& snapshot.selfIdentity == entry.selfIdentity
				&& snapshot.selfIdentity == mappedMetadata;
			const bool shapeMatch = readable
				&& snapshot.shapeIdentity == entry.shapeIdentity
				&& snapshot.shapeIdentity == shape;
			const bool integrity = registryIdentityValid
				&& pointerMatch && allocationMatch
				&& selfMatch && shapeMatch;

			static std::atomic<UInt32> failureLogCount = 0;
			const UInt32 failureOrdinal = integrity ? 0
				: failureLogCount.fetch_add(
					1, std::memory_order_relaxed);
			const bool logFailure = !integrity
				&& failureOrdinal < 64;
			// Successful delete audits were useful while proving the lifetime
			// guard, but they dominate normal diagnostic logs. Keep the complete
			// record only for an actual integrity failure, even when verbose font
			// logging is enabled.
			if (!logFailure)
				return integrity;

			gLog.FormattedMessage(
				"tnvse_freetype_native: metadata-delete-integrity-failure shape=%p registry=%u mapped=%p expectedSelf=%p expectedShape=%p allocationId=%llu readable=%u objectAllocationId=%llu objectSelf=%p objectShape=%p font=%u backend=%u build=%u registryIdentity=%u pointer=%u allocation=%u self=%u shapeIdentity=%u integrity=%u",
				shape, registryFound ? 1u : 0u,
				mappedMetadata, entry.selfIdentity,
				entry.shapeIdentity,
				static_cast<unsigned long long>(entry.allocationId),
				readable ? 1u : 0u,
				static_cast<unsigned long long>(
					snapshot.allocationId),
				snapshot.selfIdentity, snapshot.shapeIdentity,
				snapshot.fontId, snapshot.backend,
				snapshot.buildComplete ? 1u : 0u,
				registryIdentityValid ? 1u : 0u,
				pointerMatch ? 1u : 0u,
				allocationMatch ? 1u : 0u,
				selfMatch ? 1u : 0u,
				shapeMatch ? 1u : 0u,
				integrity ? 1u : 0u);
			return integrity;
		}

		void __fastcall NativeFontDeleteThis(NiTriShape* shape, void*)
		{
			NativeFontShapeState& state = State();
			InvalidateNativeFontCommandGeometry(shape);
			NativeFontShapeMetadataEntry retiredEntry;
			bool registryFound = false;
			{
				std::lock_guard<std::mutex> lock(state.metadataMutex);
				const auto found = state.shapeMetadata.find(shape);
				if (found != state.shapeMetadata.end())
				{
					registryFound = true;
					state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(1,
						std::memory_order_release);
					retiredEntry = std::move(found->second);
					state.shapeMetadata.erase(found);
				}
			}
			const bool metadataIntegrity =
				LogNativeFontMetadataDeleteAudit(
				shape, registryFound, retiredEntry);
			NativeFontShapeMetadataPtr retiredMetadata =
				std::move(retiredEntry.metadata);
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->nativePayload.buildComplete)
			{
				InvalidateNativeFontTileRetainedText(
					retiredMetadata->nativePayload);
			}
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->backend
					== FreeTypeShapeBackend::SingletonFacade)
			{
				ReleaseSingletonFacadeBinding(
					shape, *retiredMetadata);
			}
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
		}
	}

	void BeginNativeFontSortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		s_constantOwnershipBatch.BeginFrame();
	}

	void EndNativeFontSortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		ReleaseNativeConstantOwnershipBatch("sorted-frame-end");
		s_constantOwnershipBatch.EndFrame();
	}

	NativeFontShapeMetadataPtr FindNativeFontShapeMetadata(const NiTriShape* shape)
	{
		if (!shape)
			return {};
		NativeFontShapeState& state = State();
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		NativeFontMetadataHotSet& hotSet = GetMetadataHotSet(shape);
		NativeFontMetadataHotEntry* replacement = nullptr;
		for (NativeFontMetadataHotEntry& hot : hotSet.ways)
		{
			if (hot.shape != shape)
				continue;
			if (hot.generation == generation)
			{
				NativeFontShapeMetadataPtr metadata = hot.metadata.lock();
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
		NativeFontMetadataHotEntry& hot = replacement
			? *replacement : SelectMetadataHotVictim(hotSet);
		hot.shape = shape;
		hot.generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_relaxed);
		hot.metadata = found->second.metadata;
		return found->second.metadata;
	}

	void AcquireNativeFontShapeMetadataBatch(
		const std::vector<NiTriShape*>& shapes,
		std::vector<NativeFontShapeMetadataPtr>& owners)
	{
		owners.clear();
		owners.resize(shapes.size());
		if (shapes.empty())
			return;

		NativeFontShapeState& state = State();
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		for (size_t index = 0; index < shapes.size(); ++index)
		{
			const NiTriShape* shape = shapes[index];
			if (!shape)
				continue;
			const auto found = state.shapeMetadata.find(shape);
			if (found == state.shapeMetadata.end())
				continue;

			const NativeFontShapeMetadataEntry& entry = found->second;
			const NativeFontShapeMetadataPtr& metadata = entry.metadata;
			if (!metadata || entry.allocationId == 0
				|| entry.allocationId != metadata->allocationId
				|| entry.selfIdentity != metadata.get()
				|| entry.selfIdentity != metadata->selfIdentity
				|| entry.shapeIdentity != shape
				|| entry.shapeIdentity != metadata->shapeIdentity)
			{
				continue;
			}
			owners[index] = metadata;
		}
	}

	RenderPassImmediatelyFn ReadRenderPassImmediatelyCallTarget()
	{
		SIZE_T target = 0;
		if (!hook_identity::ReadRel32Target(
			kRenderPassImmediatelyCallSite,
			hook_identity::Rel32Opcode::Call,
			target))
		{
			return nullptr;
		}
		return reinterpret_cast<RenderPassImmediatelyFn>(target);
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrent()
	{
		const RenderPassImmediatelyFn hook = &NativeFontRenderPassImmediately;
		const RenderPassImmediatelyFn predecessor =
			State().originalRenderPassImmediately;
		return predecessor && predecessor != hook
			&& hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(predecessor))
			&& ReadRenderPassImmediatelyCallTarget() == hook;
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrentFast()
	{
		const bool current =
			IsNativeFontRenderPassImmediatelyHookCurrentUnchecked();
		if (!current)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessImmediateMismatch);
		}
		return current;
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrentUnchecked()
	{
		return State().originalRenderPassImmediately
			&& State().originalRenderPassImmediately
				!= &NativeFontRenderPassImmediately
			&& hook_identity::MatchesRel32InstructionImageUnchecked(
				kRenderPassImmediatelyCallSite,
				s_renderPassImmediatelyHookImage);
	}

	void __cdecl NativeFontRenderPassImmediately(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupRenderStates)
	{
		NativeFontShapeState& state = State();
		if (!state.originalRenderPassImmediately)
			return;
		NiTriShape* shape = pass
			? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
		if (!IsNativeFontAtlasShape(shape))
		{
			if (s_constantOwnershipBatch.FrameActive())
				ReleaseNativeConstantOwnershipBatch(
					"before-foreign-render-pass");
			s_segmentDeviceStateCache.Reset();
			// Official B994F0/B98E80 and beta RenderPassImmediately dispatch
			// arbitrary attached-shader pass and geometry callbacks. None of those
			// callbacks owns tNVSE's c176-c183/c208/c209 contract, so an unrelated
			// pass is a hard private-constant boundary even when renderer and
			// viewport identity remain unchanged.
			InvalidateNativeFontSortedShaderStateForForeignRenderPass();
			state.originalRenderPassImmediately(pass, currentPass, testAlpha,
				blendAlpha, setupRenderStates);
			// Clear any state rebuilt by a nested callback without advancing the
			// command boundary twice. The next native submission must republish all
			// private pixel and Vanilla-layout vertex constants.
			s_segmentDeviceStateCache.Reset();
			InvalidateNativeFontSortedShaderStateWithinExecutionSegment();
			return;
		}

		FreeTypePerfScope dispatchRoutePerf(
			FreeTypePerfPhase::DispatchRoute);
		NativeFontSortedFrameEntryView frameEntry;
		const bool sortedFrameHit =
			FindNativeFontSortedFrameEntry(shape, frameEntry);
		if (sortedFrameHit
			&& (frameEntry.visibility.cull
					== NativeFontVisibilityCull::Clip
				|| frameEntry.visibility.cull
					== NativeFontVisibilityCull::Scissor))
		{
			// The sorted-frame clip proof is revalidated against the live
			// volatile inputs before it suppresses the dispatch. Honoring
			// must precede the singleton-facade direct-draw path so culled
			// singleton texts never arm a packet draw. Any drift revokes the cached
			// decision and falls open to the ordinary draw path.
			if (HonorNativeFontPreflightClipCull(shape,
				frameEntry.visibility))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipHonored);
				if (frameEntry.payload)
				{
					RecordNativeFontVisibilityCull(
						frameEntry.visibility.cull, *frameEntry.payload);
				}
				else
				{
					RecordNativeFontVisibilityCull(
						frameEntry.visibility.cull);
				}
				return;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::
				VisibilityPreflightClipRevoked);
		}
		if (sortedFrameHit
			&& frameEntry.visibility.cull
				== NativeFontVisibilityCull::ZeroAlpha
			&& EvaluateNativeFontSubmissionVisibility(shape)
				== NativeFontVisibilityCull::ZeroAlpha)
		{
			if (frameEntry.payload)
			{
				RecordNativeFontVisibilityCull(
					NativeFontVisibilityCull::ZeroAlpha,
					*frameEntry.payload);
			}
			else
			{
				RecordNativeFontVisibilityCull(
					NativeFontVisibilityCull::ZeroAlpha);
			}
			return;
		}
		if (IsVanillaLayoutShape(shape))
		{
			NativeFontVisibilityCull visibilityCull =
				EvaluateNativeFontSubmissionVisibility(shape);
			const bool reuseOverlap = sortedFrameHit
				&& ReuseNativeFontPreflightClipOverlap(
					frameEntry.visibility);
			if (visibilityCull == NativeFontVisibilityCull::None
				&& !reuseOverlap)
			{
				visibilityCull = EvaluateNativeFontPreflightClipVisibility(
					shape).cull;
			}
			if (visibilityCull != NativeFontVisibilityCull::None)
			{
				RecordNativeFontVisibilityCull(visibilityCull);
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutCull);
				return;
			}
		}
		NativeFontShapeMetadataPtr metadataOwner;
		const NativeFontShapeMetadata* metadata = nullptr;
		NativeFontShapePayload* payload = nullptr;
		if (sortedFrameHit && frameEntry.metadata)
		{
			metadata = frameEntry.metadata;
			payload = frameEntry.payload;
		}
		else
		{
			metadataOwner = FindNativeFontShapeMetadata(shape);
			metadata = metadataOwner.get();
			if (metadata && metadata->nativePayload.buildComplete)
				payload = &metadata->nativePayload;
		}
		if (!metadata)
		{
			LogMissingMetadata(shape, "tile-render-pass");
			return;
		}
		if (metadata->backend == FreeTypeShapeBackend::VanillaLayout)
		{
			bool shiftedVanillaLayout = false;
			if (metadata->nativePayload.buildComplete
				&& metadata->nativePayload.payloadTemplate
				&& metadata->nativePayload.payloadTemplate->compositePackets.size()
					== 1)
			{
				const NativeFontPacketTemplate& packet =
					metadata->nativePayload.payloadTemplate->
						compositePackets.front();
				const NativeFontVanillaLayoutKind layoutKind =
					GetVanillaLayoutKind(*metadata);
				shiftedVanillaLayout = packet.compositeShiftedShadow;
				TileShader* shader = ResolveNativeFontPacketShader(
					packet, shape, false, layoutKind);
				if (shader && shape->GetShader() != shader)
					shape->SetShader(shader);
				NativeFontVanillaLayoutDrawToken* drawToken =
					GetVanillaLayoutDrawToken(*metadata);
				if (shader && drawToken && EnsureNativeFontVanillaLayoutShapeReady(
					shape, shader, metadata->nativePayload, *drawToken))
				{
					if (s_constantOwnershipBatch.FrameActive())
					{
						ReleaseNativeConstantOwnershipBatch(
							"before-vanilla-layout");
					}
					s_segmentDeviceStateCache.Reset();
					UInt64 vanillaLayoutTransition = 0;
					bool vanillaLayoutDrawn = false;
					{
						NativeFacadeShaderBatchScope shaderBatch;
						VanillaLayoutOriginalVtableScope vanillaVtable(shape);
						if (vanillaVtable.Active())
						{
							vanillaLayoutTransition =
								BeginNativeFontVanillaLayoutShaderTransition(
									shader, currentPass);
							state.originalRenderPassImmediately(pass,
								currentPass, testAlpha, blendAlpha,
								setupRenderStates);
							vanillaLayoutDrawn = true;
							RecordFreeTypePerf(
								FreeTypePerfCounter::VanillaLayoutDraw);
							if (shiftedVanillaLayout)
							{
								RecordFreeTypePerf(FreeTypePerfCounter::
									VanillaLayoutShiftedDraw);
							}
						}
					}
					if (vanillaLayoutDrawn)
					{
						EndNativeFontVanillaLayoutShaderTransition(
							vanillaLayoutTransition, shader);
						return;
					}
					InvalidateNativeFontSortedShaderState();
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutRuntimeFallback);
			if (shiftedVanillaLayout)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutShiftedRuntimeFallback);
			}
		}
		if (metadata->backend
			== FreeTypeShapeBackend::SingletonFacade)
		{
			SingletonFacadeState* singleton =
				GetSingletonFacadeState(*metadata);
			if (!singleton || singleton->slot.shape != shape)
			{
				RecordNativeFontSuppression(shape, *metadata,
					NativeFontFallbackReason::PacketBuild,
					"singleton-facade-singleton-tile");
				return;
			}
			const SingletonFacadeFrameMode mode =
				singleton->frameMode.load(std::memory_order_acquire);
			if (mode == SingletonFacadeFrameMode::Culled
				|| mode == SingletonFacadeFrameMode::Fault
				|| mode == SingletonFacadeFrameMode::Retired)
			{
				return;
			}

			const UInt64 validationToken =
				GetNativeFontSortedFrameValidationToken();
			if (mode == SingletonFacadeFrameMode::Direct)
			{
				NativePacketDrawResult draw;
				const UInt64 commandToken =
					singleton->commandValidationToken.load(
						std::memory_order_acquire);
				const UInt32 directFacadeSinglePacketCommandIndex =
					singleton->commandDirectFacadeSinglePacketIndex.load(
						std::memory_order_acquire);
				const bool commandCurrent = g_bEnableFreeTypeFontCommandBuffer
					&& commandToken && commandToken == validationToken
					&& directFacadeSinglePacketCommandIndex
						!= kInvalidNativeFontCommandIndex;
				bool handled = TryDrawSingletonFacadePacket(pass, currentPass,
					setupRenderStates, shape, *metadata, validationToken, draw,
					commandCurrent ? directFacadeSinglePacketCommandIndex
						: kInvalidNativeFontCommandIndex);
				if (!handled)
				{
					if (validationToken
						&& singleton->directDrawCount.load(
							std::memory_order_acquire) == 0)
					{
						RestoreSingletonFacade(*metadata,
							NativeFontFallbackReason::PacketPrepare);
					}
					else
					{
						singleton->frameMode.store(
							SingletonFacadeFrameMode::Fault,
							std::memory_order_release);
						RecordFreeTypePerf(FreeTypePerfCounter::
							SingletonFacadeFallback);
					}
					if (singleton->frameMode.load(std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Facade)
					{
						return;
					}
				}
				else if (!draw.runtimeFault)
				{
					return;
				}
				else
				{
					InvalidateNativeFontSortedShaderState();
					NativeFontShapePayload* singletonPayload =
						&metadata->nativePayload;
					if (draw.constantStateFault)
					{
						MarkNativeFontGenerationFault(
							singletonPayload->preparedGeneration,
							draw.operation, draw.result);
						gLog.FormattedMessage(
							"tnvse_freetype_native: singleton-facade singleton pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-shape",
							draw.operation,
							static_cast<UInt32>(draw.result),
							draw.mismatchRegister, shape,
							metadata->fontId,
							singletonPayload->preparedGeneration,
							draw.drewPacket ? 1 : 0);
					}
					if (draw.drewPacket)
					{
						MarkNativeFontRuntimeFault(*metadata,
							*singletonPayload, draw.failure);
					}
					if (draw.drewPacket || draw.constantStateFault
						|| singleton->frameMode.load(
							std::memory_order_acquire)
							!= SingletonFacadeFrameMode::Facade)
					{
						return;
					}
				}
			}
			payload = &metadata->nativePayload;
		}
		if (payload)
		{
			const bool needsVisibilityCheck = !sortedFrameHit
				|| frameEntry.visibility.cull
					!= NativeFontVisibilityCull::None;
			if (needsVisibilityCheck)
			{
				const NativeFontVisibilityCull visibilityCull =
					EvaluateNativeFontSubmissionVisibility(shape, *payload);
				if (visibilityCull != NativeFontVisibilityCull::None)
				{
					RecordNativeFontVisibilityCull(
						visibilityCull, *payload);
					return;
				}
			}
		}
		NativeFontFallbackReason failure = NativeFontFallbackReason::None;
		if (!payload)
			failure = NativeFontFallbackReason::PacketBuild;
		else if (payload->suppressNextSubmit.exchange(false,
			std::memory_order_acq_rel))
		{
			failure = payload->stickyReason.exchange(
				NativeFontFallbackReason::None, std::memory_order_acq_rel);
			if (failure == NativeFontFallbackReason::None)
				failure = NativeFontFallbackReason::RuntimeFault;
		}
		else if (sortedFrameHit
			&& frameEntry.preflightResult == NativeFontFallbackReason::None
			&& frameEntry.validationToken
			&& frameEntry.generation == payload->preparedGeneration
			&& payload->preflightAtlasTextureEpoch
				== GetNativeFontAtlasTextureEpoch()
			)
		{
			// NativeFontRenderAlphaGeometry retained the metadata owner and validated this
			// exact payload immediately before the vanilla sorted Tile traversal.
			failure = NativeFontFallbackReason::None;
		}
		else
			failure = PrepareNativeFontFacade(shape, *metadata, *payload);

		if (failure == NativeFontFallbackReason::None)
		{
			NativeFontShapePayload* const sourcePayload = payload;
			NativePacketDrawResult draw;
			const UInt32 commandSpanIndex =
				sortedFrameHit
					? frameEntry.commandSpanIndex
					: kInvalidNativeFontCommandIndex;
			bool commandHandled = false;
			if (g_bEnableFreeTypeFontCommandBuffer
				&& commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				NativeFontCommandSpanView commandView;
				if (FindNativeFontCommandSpan(commandSpanIndex,
						frameEntry.validationToken, commandView)
					&& commandView.span
					&& commandView.span->commandCount > 1)
				{
					commandHandled = TryDrawNativeRetainedSpan(
						pass, currentPass, setupRenderStates,
						shape, *sourcePayload,
						commandSpanIndex, draw);
				}
			}
			if (commandHandled && metadata->backend
				== FreeTypeShapeBackend::SingletonFacade)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeSpanFrame);
			}
			if (!commandHandled)
			{
				draw = {};
				const bool directShapeHandled = sortedFrameHit
					&& TryDrawNativeSinglePacketDirect(
						pass, currentPass, setupRenderStates,
						shape, *sourcePayload, draw,
						commandSpanIndex,
						frameEntry.singlePacketCommandIndex);
				if (!directShapeHandled)
				{
					if (metadata->backend
						== FreeTypeShapeBackend::SingletonFacade)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							SingletonFacadePacketLoopFrame);
					}
					draw = DrawNativePacketSet(pass, currentPass,
						setupRenderStates, shape, *sourcePayload,
						kInvalidNativeFontCommandIndex);
				}
			}
			if (!draw.runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedRenderPassImmediatelyHit)
				{
					state.loggedRenderPassImmediatelyHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile facade route hit shape=%p font=%u pass=%u packets=%u ranges=%u route=%s",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(
							sourcePayload->packetShaders.size()),
						sourcePayload->payloadTemplate
							? sourcePayload->payloadTemplate->sourceRangeCount
							: 0u,
						draw.directShapeRoute
							? "direct-single-packet-shape"
							: (draw.vanillaLikeBitmapRoute
								? "vanilla-like-bitmap-pages"
								: "effect-packets"));
				}
				return;
			}
			InvalidateNativeFontSortedShaderState();
			if (draw.constantStateFault)
			{
				MarkNativeFontGenerationFault(
					sourcePayload->preparedGeneration,
					draw.operation, draw.result);
				gLog.FormattedMessage(
					"tnvse_freetype_native: pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-facade",
					draw.operation, static_cast<UInt32>(draw.result),
					draw.mismatchRegister, shape, metadata->fontId,
					sourcePayload->preparedGeneration,
					draw.drewPacket ? 1 : 0);
			}
			if (draw.drewPacket)
			{
				if (metadata->backend
					== FreeTypeShapeBackend::SingletonFacade)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						SingletonFacadePartialFault);
				}
				MarkNativeFontRuntimeFault(*metadata, *sourcePayload,
					draw.failure);
				return;
			}
			failure = draw.failure;
		}

		RecordNativeFontSuppression(shape, *metadata, failure, "tile-render-pass");
	}

	bool HookRenderPassImmediately()
	{
		RenderPassImmediatelyFn current = ReadRenderPassImmediatelyCallTarget();
		const RenderPassImmediatelyFn hook = &NativeFontRenderPassImmediately;
		if (current == hook)
		{
			const RenderPassImmediatelyFn predecessor =
				State().originalRenderPassImmediately;
			State().renderPassImmediatelyHookInstalled = predecessor
				&& predecessor != hook
				&& hook_identity::IsExecutableTarget(
					reinterpret_cast<SIZE_T>(predecessor));
			if (!State().renderPassImmediatelyHookInstalled)
				InvalidateAllSingletonFacadeBindings();
			return State().renderPassImmediatelyHookInstalled;
		}
		if (!current)
		{
			if (State().renderPassImmediatelyHookInstalled)
			{
				State().renderPassImmediatelyHookInstalled = false;
				InvalidateAllSingletonFacadeBindings();
			}
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: BSShaderAccumulator::RenderAlphaGeometry -> BSBatchRenderer::RenderPassImmediately call site is not CALL rel32; native route unavailable");
			}
			return false;
		}
		if (State().renderPassImmediatelyHookInstalled)
		{
			State().renderPassImmediatelyHookInstalled = false;
			InvalidateAllSingletonFacadeBindings();
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: RenderPassImmediately native route was replaced; marked facades will be suppressed");
			}
			return false;
		}
		if (reinterpret_cast<UInt32>(current)
			!= kBSBatchRendererRenderPassImmediately)
		{
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: RenderPassImmediately call site already has a non-vanilla target=%p; leaving it untouched",
					current);
			}
			return false;
		}

		State().originalRenderPassImmediately = current;
		// BSBatchRenderer immediate pass call (__cdecl).
		WriteRelCall(kRenderPassImmediatelyCallSite,
			&NativeFontRenderPassImmediately);
		const RenderPassImmediatelyFn observed =
			ReadRenderPassImmediatelyCallTarget();
		if (observed == hook)
		{
			State().renderPassImmediatelyHookInstalled = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed RenderPassImmediately native route original=%p vanilla=1",
					current);
			}
			return true;
		}

		State().renderPassImmediatelyHookInstalled = false;
		if (observed == current)
		{
			State().originalRenderPassImmediately = nullptr;
			gLog.FormattedMessage(
				"tnvse_freetype_native: RenderPassImmediately hook write did not publish; vanilla target remains=%p",
				current);
			return false;
		}

		// Do not replace an observed later owner with vanilla. It may already
		// chain through this hook, so retain the original target for any call
		// that still reaches tNVSE while the strict top-level route fails closed.
		State().loggedRenderPassImmediatelyHookConflict = true;
		gLog.FormattedMessage(
			"tnvse_freetype_native: RenderPassImmediately hook retained below observed target=%p original=%p; native route marked unavailable",
			observed,
			current);
		return false;
	}

	bool IsNativeFontAtlasShape(const NiTriShape* shape)
	{
		if (!shape)
			return false;
		void* const* vtable =
			*reinterpret_cast<void* const* const*>(shape);
		return vtable == &State().triShapeVtable[1]
			|| vtable == &State().vanillaLayoutTriShapeVtable[1];
	}

	bool IsVanillaLayoutShape(const NiTriShape* shape)
	{
		return shape && *reinterpret_cast<void* const* const*>(shape)
			== &State().vanillaLayoutTriShapeVtable[1];
	}

	void __fastcall NativeFontRenderImmediate(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (s_nativeDirectImmediateContext
			&& s_nativeDirectImmediateContext->shape == shape
			&& State().originalRenderImmediate)
		{
			NativeDirectImmediateContext& context =
				*s_nativeDirectImmediateContext;
			context.invoked = true;
			if (context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (!context.validationPassed
				&& context.strictValidation)
			{
				return;
			}
			State().originalRenderImmediate(shape, renderer);
			context.drew = true;
			if (!context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (context.continueImmediate)
			{
				context.continuationSucceeded =
					context.continueImmediate(
						context.continuation, renderer, false);
			}
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate");
	}

	void __fastcall NativeFontRenderImmediateAlt(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (s_nativeDirectImmediateContext
			&& s_nativeDirectImmediateContext->shape == shape
			&& State().originalRenderImmediateAlt)
		{
			NativeDirectImmediateContext& context =
				*s_nativeDirectImmediateContext;
			context.invoked = true;
			if (context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (!context.validationPassed
				&& context.strictValidation)
			{
				return;
			}
			if (context.directDrawLite)
			{
				ExecuteNativeDirectDrawLite(*context.directDrawLite);
			}
			else
			{
				State().originalRenderImmediateAlt(shape, renderer);
			}
			context.drew = true;
			if (!context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (context.continueImmediate)
			{
				context.continuationSucceeded =
					context.continueImmediate(
						context.continuation, renderer, true);
			}
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate-alt");
	}

	bool InitializeNativeFontTriShapeVtable(NiTriShape* shape)
	{
		void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
		if (!source)
			return false;
		if (source == &State().triShapeVtable[1]
			|| source == &State().vanillaLayoutTriShapeVtable[1])
			return true;
		if (State().originalTriShapeVtable)
			return source == State().originalTriShapeVtable;

		const SIZE_T sourceAddress = reinterpret_cast<SIZE_T>(source);
		constexpr SIZE_T kCopiedVtableBytes =
			(kCopiedTriShapeVtableEntries + 1u) * sizeof(void*);
		if (sourceAddress < sizeof(void*)
			|| !hook_identity::IsAccessibleRegion(
				sourceAddress - sizeof(void*), kCopiedVtableBytes, false))
		{
			return false;
		}

		const RenderImmediateFn originalRenderImmediate =
			reinterpret_cast<RenderImmediateFn>(
				source[kRenderImmediateSlot]);
		const RenderImmediateFn originalRenderImmediateAlt =
			reinterpret_cast<RenderImmediateFn>(
				source[kRenderImmediateAltSlot]);
		const DeleteThisFn originalDeleteThis =
			reinterpret_cast<DeleteThisFn>(source[kDeleteThisSlot]);
		const SIZE_T originalRenderImmediateAddress =
			reinterpret_cast<SIZE_T>(originalRenderImmediate);
		const SIZE_T originalRenderImmediateAltAddress =
			reinterpret_cast<SIZE_T>(originalRenderImmediateAlt);
		const SIZE_T originalDeleteThisAddress =
			reinterpret_cast<SIZE_T>(originalDeleteThis);
		if (!originalRenderImmediate || !originalRenderImmediateAlt
			|| !originalDeleteThis
			|| originalRenderImmediateAddress
				== reinterpret_cast<SIZE_T>(&NativeFontRenderImmediate)
			|| originalRenderImmediateAltAddress
				== reinterpret_cast<SIZE_T>(&NativeFontRenderImmediateAlt)
			|| originalDeleteThisAddress
				== reinterpret_cast<SIZE_T>(&NativeFontDeleteThis)
			|| !hook_identity::IsExecutableTarget(
				originalRenderImmediateAddress)
			|| !hook_identity::IsExecutableTarget(
				originalRenderImmediateAltAddress)
			|| !hook_identity::IsExecutableTarget(
				originalDeleteThisAddress))
		{
			return false;
		}

		NativeFontShapeState& state = State();
		const void* expectedFalsePredicate =
			reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate);
		const bool predicateSlotsMatch =
			source[kGeometrySpecialPredicateSlot]
				== expectedFalsePredicate
			&& source[kGeometryAlternatePredicateSlot]
				== expectedFalsePredicate;
		// Verify the reverse-engineered identity and behavior once while the
		// object still owns the vanilla vtable. The lite hot path then needs only
		// the tNVSE vtable identity plus this immutable result.
		state.standardPassLitePredicatesValidated =
			predicateSlotsMatch
			&& !CallGeometryPredicate(
				shape, kGeometrySpecialPredicateSlot)
			&& !CallGeometryPredicate(
				shape, kGeometryAlternatePredicateSlot);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: RenderPassImmediately_Standard-lite predicate-envelope validated=%u special=%p alternate=%p expected=%p segmentState=independent-effective-v11 slot34=vendor-atoc-independent standardV2=classified-slot-delta slot31=constants-key+native-lite-transient+translation-lite-c0-c3+mismatch-first-field+world-mask slot35=need-only",
				state.standardPassLitePredicatesValidated ? 1u : 0u,
				source[kGeometrySpecialPredicateSlot],
				source[kGeometryAlternatePredicateSlot],
				expectedFalsePredicate);
		}

		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			triShapeVtable = {};
		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			vanillaLayoutTriShapeVtable = {};
		triShapeVtable[0] = source[-1];
		std::copy(source, source + kCopiedTriShapeVtableEntries,
			triShapeVtable.begin() + 1);
		vanillaLayoutTriShapeVtable = triShapeVtable;
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: direct-draw-lite immediate proof ready=%u original=%p expected=%p",
				originalRenderImmediateAlt
					== reinterpret_cast<RenderImmediateFn>(
						kNiTriShapeOnlyRenderImmediate) ? 1u : 0u,
				originalRenderImmediateAlt,
				reinterpret_cast<void*>(kNiTriShapeOnlyRenderImmediate));
		}
		triShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&NativeFontDeleteThis);
		triShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediate);
		triShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediateAlt);
		vanillaLayoutTriShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&NativeFontDeleteThis);
		vanillaLayoutTriShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediate);
		vanillaLayoutTriShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediateAlt);

		state.triShapeVtable = triShapeVtable;
		state.vanillaLayoutTriShapeVtable = vanillaLayoutTriShapeVtable;
		state.originalRenderImmediate = originalRenderImmediate;
		state.originalRenderImmediateAlt = originalRenderImmediateAlt;
		state.originalDeleteThis = originalDeleteThis;
		state.originalTriShapeVtable = source;
		return true;
	}
}
