#pragma once

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


namespace fonthook::vectorfont::implementation::font_native_shape_hooks
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
		inline const hook_identity::Rel32InstructionImage
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

	using NativeFontMetadataHotSets =
		std::array<NativeFontMetadataHotSet, kMetadataHotSetCount>;
	std::unique_ptr<NativeFontMetadataHotSets>& MetadataHotSets();
	NativeFontMetadataHotSet& GetMetadataHotSet(const NiTriShape* shape);
	NativeFontMetadataHotEntry& SelectMetadataHotVictim(
		NativeFontMetadataHotSet& set);

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

	NativePassConstantBatch& ConstantOwnershipBatch();
	bool ReleaseNativeConstantOwnershipBatch(const char* phase);

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

	NativeDirectImmediateContext*& DirectImmediateContext();

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
				: m_previous(DirectImmediateContext())
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
				DirectImmediateContext() = &m_context;
			}

			~NativeDirectImmediateScope()
			{
				DirectImmediateContext() = m_previous;
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
		NiTriShape* shape, NiRenderer* renderer);

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
				m_originalVtable = *reinterpret_cast<void***>(m_shape);
				void** nativeVtable = &State().triShapeVtable[1];
				if (!m_originalVtable || !nativeVtable)
					return;
				if (m_originalVtable != nativeVtable)
				{
					*reinterpret_cast<void***>(m_shape) = nativeVtable;
					m_changed = true;
				}
				m_active =
					*reinterpret_cast<void***>(m_shape) == nativeVtable;
			}

			~NativeImmediateHookVtableScope()
			{
				if (m_changed && m_shape)
					*reinterpret_cast<void***>(m_shape) = m_originalVtable;
			}

			bool Active() const
			{
				return m_active;
			}

		private:
			NiTriShape* m_shape = nullptr;
			void** m_originalVtable = nullptr;
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
		const NiTransform& source, const NiPoint3& origin);

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

	DirectTileShaderPropertyView* GetDirectTileProperty(NiTriShape* shape);
	void RecordGpuEnvelopeVanillaCull(NativeFontVisibilityCull cull);

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
		const NativeFontCommandSpanView& view, UInt32 commandOffset);
	bool IsDefaultNativeReplayPass(UInt32 pass);
	NativeFontCommandBindState MakeNativeCommandBindState(
		const BSShaderProperty::RenderPass* pass, UInt32 currentPass,
		bool testAlpha, bool blendAlpha,
		bool setupRenderStates);
	bool CallGeometryPredicate(NiGeometry* geometry, UInt32 slot);
	bool RendererUsesSpecialPass(NiGeometry* geometry);
	bool CanUseNativeReplayBase(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, NiTriShape* geometry,
		const NativeFontDrawCommand& command,
		bool packetStatePrevalidated);
	bool InvokeNativeCommandBootstrap(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha,
		bool setupRenderStates, NiTriShape* geometry,
		const NativeFontDrawCommand* command,
		bool preferStandardPassLite = false,
		bool packetStatePrevalidated = false,
		NiGeometryBufferData* preparedBuffer = nullptr,
		const NativeFontSegmentDeviceStateStamp* deviceStateStamp = nullptr);
	void RecordRetainedPacketDraw(
		const NativeFontDrawCommand& command, bool retainedExtra);
	bool TryDrawNativeRetainedSpan(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool setupRenderStates, NiTriShape* facade,
		NativeFontShapePayload& payload, UInt32 commandSpanIndex,
		NativePacketDrawResult& draw);
	bool TryDrawSingletonFacadePacket(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool setupRenderStates, NiTriShape* shape,
		const NativeFontShapeMetadata& metadata,
		UInt64 validationToken,
		NativePacketDrawResult& draw,
		UInt32 directFacadeSinglePacketCommandIndex =
			kInvalidNativeFontCommandIndex);
	NativePacketDrawResult DrawNativePacketSet(
		BSShaderProperty::RenderPass* pass, UInt32 currentPass,
		bool setupRenderStates,
		NiTriShape* facade, NativeFontShapePayload& payload,
		UInt32 commandSpanIndex = kInvalidNativeFontCommandIndex);
	bool TryDrawNativeSinglePacketDirect(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool setupRenderStates, NiTriShape* facade,
		NativeFontShapePayload& payload, NativePacketDrawResult& draw,
		UInt32 commandSpanIndex = kInvalidNativeFontCommandIndex,
		UInt32 singlePacketCommandIndex = kInvalidNativeFontCommandIndex);
	bool SameNativePacketBinding(
		const NativeFontFramePacketBinding& command,
		const NativeFontDirectFacadePacketBinding& live);
	void LogMissingMetadata(NiTriShape* shape, const char* phase);
	void SuppressImmediateRoute(NiTriShape* shape, const char* phase);
	void __fastcall NativeFontDeleteThis(NiTriShape* shape, void*);
}
