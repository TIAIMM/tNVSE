#include "font_a8_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiD3DRenderState.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9RenderState.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace fonthook::vectorfont
{
	namespace implementation::font_a8_hooks {}
	using namespace implementation::font_a8_hooks;

	namespace implementation::font_a8_hooks
	{
		inline constexpr UInt32 kRenderPassImmediatelyStandard = 0xB98E80;
		inline constexpr UInt32 kSelectShaderForPass = 0xB99390;
		inline constexpr UInt32 kSetVendorAlphaToCoverageState = 0xB98540;
		inline constexpr UInt32 kGeometryUsesSpecialPass = 0xE72C20;
		inline constexpr UInt32 kPassSuppressesBlendAlpha = 0xB630F0;
		inline constexpr UInt32 kTileSetSourceTexture = 0xBB7A10;
		inline constexpr UInt32 kCurrentRenderPass = 0x11F91E0;
		inline constexpr UInt32 kCurrentRenderPassType = 0x11F91E4;
		inline constexpr UInt32 kSelectedRenderPassType = 0x11FFE30;
		inline constexpr UInt32 kSelectedShader = 0x11FFE2C;
		inline constexpr UInt32 kVendorAlphaToCoverageEnabled = 0x11F9421;
		inline constexpr UInt32 kFirstPassState = 0x11AD8EC;
		inline constexpr UInt32 kRendererState = 0x11F9508;
		inline constexpr UInt32 kForcedShaderSelectionPass = 758;
		inline constexpr UInt32 kGeometrySpecialPredicateSlot = 12;
		inline constexpr UInt32 kGeometryAlternatePredicateSlot = 13;
		// Official NiTriShape vtable slots 12/13 both point at ACBB70,
		// the shared predicate thunk that returns false. E68810 is instead
		// the positive cast thunk used by slots 6/7/9 and returns this.
		inline constexpr UInt32 kNiGeometryFalsePredicate = 0xACBB70;

		struct A8MetadataHotEntry
		{
			const NiTriShape* shape = nullptr;
			UInt64 generation = 0;
			std::weak_ptr<const A8ShapeMetadata> metadata;
		};

		inline constexpr size_t kMetadataHotWayCount = 8;
		inline constexpr size_t kMetadataHotSetCount = 2048;
		static_assert((kMetadataHotSetCount & (kMetadataHotSetCount - 1)) == 0,
			"metadata hot-cache set count must remain a power of two");

		struct A8MetadataHotSet
		{
			std::array<A8MetadataHotEntry, kMetadataHotWayCount> ways;
			UInt8 nextVictim = 0;
		};

		// Eight ways keep allocator-neighbouring facades from evicting one another,
		// while weak ownership preserves menu/shape destruction semantics. 16384
		// entries cover the observed multi-page Pip-Boy working set without adding
		// any process-wide locks to the render path.
		thread_local std::unique_ptr<
			std::array<A8MetadataHotSet, kMetadataHotSetCount>>
			s_metadataHotSets;

		A8MetadataHotSet& GetMetadataHotSet(const NiTriShape* shape)
		{
			if (!s_metadataHotSets)
			{
				s_metadataHotSets = std::make_unique<
					std::array<A8MetadataHotSet, kMetadataHotSetCount>>();
			}
			const size_t index = HashMetadataShapeAddress(shape)
				& (kMetadataHotSetCount - 1);
			return (*s_metadataHotSets)[index];
		}

		A8MetadataHotEntry& SelectMetadataHotVictim(A8MetadataHotSet& set)
		{
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (!entry.shape)
					return entry;
			}
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (entry.metadata.expired())
				{
					entry = {};
					return entry;
				}
			}
			A8MetadataHotEntry& victim =
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
				m_generation = GetNativeA8ShaderGeneration();
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
			MarkNativeA8GenerationFault(s_constantOwnershipBatch.Generation(),
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
				EndNativeA8RingSubmission(submission);
			}

			NativeA8RingSubmission submission;
		};

		class NativeDirectShapeSubmissionScope
		{
		public:
			~NativeDirectShapeSubmissionScope()
			{
				EndNativeA8DirectShapeSubmission(submission);
			}

			NativeA8DirectShapeSubmission submission;
		};

		using NativeImmediateContinuationFn =
			bool(*)(void*, NiRenderer*, bool);

		enum class NativeImmediateCommandKind : UInt8
		{
			None = 0,
			SpanPacket,
			SinglePacket,
			VirtualSinglePacket
		};

		struct NativeDirectImmediateContext
		{
			NiTriShape* shape = nullptr;
			void* continuation = nullptr;
			NativeImmediateContinuationFn continueImmediate = nullptr;
			UInt32 commandSpanIndex = kInvalidNativeA8CommandIndex;
			UInt32 commandOffset = kInvalidNativeA8CommandIndex;
			NativeImmediateCommandKind commandKind =
				NativeImmediateCommandKind::None;
			bool strictValidation = false;
			bool packetStatePrevalidated = false;
			bool invoked = false;
			bool validationPassed = true;
			bool drew = false;
			bool continuationSucceeded = true;
		};

		thread_local NativeDirectImmediateContext*
			s_nativeDirectImmediateContext = nullptr;

		class NativeDirectImmediateScope
		{
		public:
			explicit NativeDirectImmediateScope(NiTriShape* shape,
				UInt32 commandSpanIndex = kInvalidNativeA8CommandIndex,
				UInt32 commandOffset = kInvalidNativeA8CommandIndex,
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
						!= kInvalidNativeA8CommandIndex
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
							!= kInvalidNativeA8CommandIndex
						&& GuardNativeA8Command(
							context.commandSpanIndex,
							context.commandOffset, shape, renderer);
				case NativeImmediateCommandKind::SinglePacket:
					return GuardNativeA8SinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				case NativeImmediateCommandKind::VirtualSinglePacket:
					return GuardNativeA8VirtualSinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				default:
					return true;
				}
			}
			switch (context.commandKind)
			{
			case NativeImmediateCommandKind::SpanPacket:
				return context.commandOffset
						!= kInvalidNativeA8CommandIndex
					&& ValidateNativeA8Command(
						context.commandSpanIndex,
						context.commandOffset, shape, renderer);
			case NativeImmediateCommandKind::SinglePacket:
				return ValidateNativeA8SinglePacketCommand(
					context.commandSpanIndex, shape, renderer);
			case NativeImmediateCommandKind::VirtualSinglePacket:
				return ValidateNativeA8VirtualSinglePacketCommand(
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

		class NativeFacadeShaderBatchScope
		{
		public:
			NativeFacadeShaderBatchScope()
			{
				BeginNativeA8FacadeShaderBatch();
			}

			~NativeFacadeShaderBatchScope()
			{
				EndNativeA8FacadeShaderBatch();
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
			const NativeA8CompiledPacketCommand* program = nullptr;
			const NiTexture* sourceTexture = nullptr;
			const NiTexture* alphaTexture = nullptr;
			const void* atlasTexture = nullptr;
			NiTexturingProperty::ClampMode effectiveClampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			UInt8 textureModeFlags = 0;
		};

		struct NativeSegmentBlendStateKey
		{
			UInt8 sourceFunction =
				static_cast<UInt8>(NiAlphaProperty::ALPHA_SRCALPHA);
			UInt8 destinationFunction =
				static_cast<UInt8>(NiAlphaProperty::ALPHA_INVSRCALPHA);
			bool enabled = false;
		};

		struct NativeSegmentAlphaTestStateKey
		{
			UInt8 testFunction = 0;
			UInt8 alphaTestRef = 0;
		};

		struct NativeSegmentDrawmodeStateKey
		{
			bool drawBoth = false;
			bool alphaTestEnabled = false;
		};

		struct NativeSegmentDeviceStateCache
		{
			NativeA8SegmentDeviceStateStamp stamp;
			NativeSegmentPassStateKey pass;
			NativeSegmentBlendStateKey blend;
			NativeSegmentAlphaTestStateKey alphaTest;
			NativeSegmentDrawmodeStateKey drawmode;
			bool stampReady = false;
			bool passReady = false;
			bool blendReady = false;
			bool alphaTestReady = false;
			bool drawmodeReady = false;

			void Reset()
			{
				stamp = {};
				pass = {};
				blend = {};
				alphaTest = {};
				drawmode = {};
				stampReady = false;
				passReady = false;
				blendReady = false;
				alphaTestReady = false;
				drawmodeReady = false;
			}

			void InvalidateStates()
			{
				passReady = false;
				blendReady = false;
				alphaTestReady = false;
				drawmodeReady = false;
			}
		};

		thread_local NativeSegmentDeviceStateCache
			s_segmentDeviceStateCache;

		bool SameSegmentDeviceStateStamp(
			const NativeA8SegmentDeviceStateStamp& left,
			const NativeA8SegmentDeviceStateStamp& right)
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
				const NativeA8SegmentDeviceStateStamp* stamp)
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
			const NativeA8DrawCommand& command,
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

		bool BuildSegmentBlendStateKey(NiTriShape* geometry,
			NativeSegmentBlendStateKey& key)
		{
			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!tile)
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
			const bool opacityBlend =
				!(tile->fAlpha >= 1.0f && tile->fFadeAlpha >= 1.0f);
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

		bool BuildSegmentDrawmodeStateKey(NiTriShape* geometry,
			UInt32 currentPass, bool firstPass,
			NativeSegmentDrawmodeStateKey& key)
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

		bool SameSegmentDrawmodeState(
			const NativeSegmentDrawmodeStateKey& left,
			const NativeSegmentDrawmodeStateKey& right)
		{
			return left.drawBoth == right.drawBoth
				&& left.alphaTestEnabled
					== right.alphaTestEnabled;
		}

		bool BuildSegmentDeviceStateStamp(
			const NativeA8FrameStamp* frame,
			UInt32 executionSegmentEpoch,
			UInt32 externalMutationEpoch,
			NativeA8SegmentDeviceStateStamp& stamp)
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
				NativeA8ShapePayload& payload,
				const NativeA8DirectShapeSubmission& submission)
				: m_shape(shape)
			{
				Initialize(payload, submission.vertexBuffer,
					submission.indexBuffer, submission.declaration,
					submission.baseVertex, submission.vertexCount,
					submission.indexBytes);
			}

			NativeDirectShapeBinding(NiTriShape* shape,
				NativeA8ShapePayload& payload,
				const NativeA8FramePacketBinding& binding)
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
					kGeometryBufferDataConstructor, m_buffer);
				m_syntheticBufferConstructed = true;
				std::memset(&m_syntheticChip, 0,
					sizeof(m_syntheticChip));
				m_syntheticChips[0] = &m_syntheticChip;
				m_syntheticStride = sizeof(NativeA8GpuVertex);
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
					kGeometryBufferDataDestructor, m_buffer);
				m_buffer = m_originalBuffer;
				m_chip = nullptr;
				m_syntheticBufferConstructed = false;
			}

			void Initialize(NativeA8ShapePayload& payload,
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
				const NativeA8PayloadTemplate& artifact =
					*payload.payloadTemplate;
				const std::vector<NativeA8PacketTemplate>& packets =
					GetNativeA8Packets(artifact,
						payload.useCompositePackets);
				if (packets.size() != 1
					|| packets[0].vertexCount != vertexCount)
				{
					m_failure =
						NativeDirectShapeBindingFailure::Topology;
					return;
				}
				const NativeA8PacketTemplate& packet = packets[0];
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
					ThisStdCall(kTileSetSourceTexture,
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
					sizeof(NativeA8GpuVertex);
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
					vertexCount * sizeof(NativeA8GpuVertex);

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
					ThisStdCall(kTileSetSourceTexture,
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
			const NativeA8FramePacketBinding& command,
			const NativeA8VirtualStockPacketBinding& live)
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

		class VirtualStockTileStateScope
		{
		public:
			VirtualStockTileStateScope(NiTriShape* shape,
				const NativeA8ShapePayload& payload,
				const NativeA8PacketTemplate& packet)
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

			~VirtualStockTileStateScope()
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
			bool stockLikeBitmapRoute = false;
			bool constantStateFault = false;
			UInt32 drawnPacketCount = 0;
			NativeA8FallbackReason failure =
				NativeA8FallbackReason::RuntimeFault;
			const char* operation = "generation-changed-after-packet";
			HRESULT result = D3DERR_DEVICELOST;
			SInt32 mismatchRegister = -1;
		};

		const NativeA8DrawCommand* ResolveNativeCommand(
			const NativeA8CommandSpanView& view, UInt32 commandOffset)
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

		NativeA8CommandBindState MakeNativeCommandBindState(
			const BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool testAlpha,
			bool blendAlpha, bool setupDrawmode)
		{
			NativeA8CommandBindState state;
			state.firstPass = pass && pass->bIsFirst;
			state.applyBlend = state.firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kPassSuppressesBlendAlpha, currentPass);
			state.applyAlphaTest = state.firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			state.applyDrawmode = setupDrawmode;
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
				*reinterpret_cast<void**>(kRendererState);
			if (!rendererState || !geometry)
				return true;
			// Retail B994F0 loads dword_11F9508 into ECX before calling
			// E72C20(rendererState, geometry, 0). Calling it as __cdecl leaves
			// ECX undefined and can classify every Tile as a multi-pass shape.
			using PredicateFn =
				bool(__thiscall*)(void*, NiGeometry*, UInt32);
			return reinterpret_cast<PredicateFn>(
				kGeometryUsesSpecialPass)(
					rendererState, geometry, 0);
		}

		bool CanUseNativeReplayBase(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeA8DrawCommand& command,
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
			NiTriShape* geometry, const NativeA8DrawCommand& command,
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
			NiTriShape* geometry, const NativeA8DrawCommand& command,
			bool packetStatePrevalidated,
			const NativeA8StandardPassLiteDispatch*& retainedDispatch)
		{
			retainedDispatch = nullptr;
			const NativeA8CompiledPacketCommand* program =
				command.program;
			const NativeA8StandardPassLiteDispatch* dispatch =
				command.standardPassLite;
			const bool dispatchCurrent =
				dispatch && program
				&& (packetStatePrevalidated
					? dispatch->ready
						&& dispatch->geometry == geometry
						&& dispatch->program == program
					: IsNativeA8StandardPassLiteDispatchCurrent(
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
				&& !pass->ucNumLights && !pass->ppSceneLights
				&& (packetStatePrevalidated
					|| (program->active
						&& geometry->GetShader() == dispatch->shader
						&& dispatch->shader->IsTileShader()));
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
				kCurrentRenderPass) = pass;
			*reinterpret_cast<UInt32*>(kCurrentRenderPassType) =
				currentPass;

			const bool selectShader =
				*reinterpret_cast<UInt32*>(
					kSelectedRenderPassType) != currentPass
				|| *reinterpret_cast<BSShader**>(
					kSelectedShader) != shader;
			if (selectShader)
			{
				// B99390 first tears down the previously selected shader and
				// invokes the new shader's pass callbacks. Those callbacks are
				// outside the four Standard-lite slots and may republish any of
				// their device-state categories. Do not carry a cache head
				// across that transition, even if the command stamp itself is
				// otherwise unchanged.
				InvalidateSegmentDeviceStateCache();
				CdeclCall<void>(kSelectShaderForPass,
					currentPass, shader);
				if (*reinterpret_cast<UInt32*>(
						kSelectedRenderPassType) != currentPass
					|| *reinterpret_cast<BSShader**>(
						kSelectedShader) != shader)
				{
					return false;
				}
			}
			// B994F0 performs post-pass restoration only for shader types 1-5
			// and 17. Guarded replay accepts only TileShader (type 20), so the
			// retail default branch has no corresponding restoration call.

			if (*reinterpret_cast<UInt8*>(
					kVendorAlphaToCoverageEnabled))
			{
				// B98540 is the PC vendor alpha-to-coverage publisher, not an
				// alpha-test-state owner. The formal build writes only render
				// state 154 with A2M0/A2M1 or state 181 with 0/ATOC. Slot 33
				// owns states 24/25, while slot 34 owns cull plus state 15, so
				// this mandatory per-pass call cannot invalidate either cached
				// output. Keep executing it to preserve the vendor extension's
				// nesting semantics, but retain all four Standard-lite proofs.
				NiAlphaProperty* alpha =
					geometry->GetAlphaProperty();
				CdeclCall<void>(kSetVendorAlphaToCoverageState,
					alpha && alpha->HasAlphaTest(), false);
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
				FreeTypePerfCounter::StandardPassLiteStockFallback);
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
			const NativeA8DrawCommand& command,
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
			bool testAlpha, bool blendAlpha, bool setupDrawmode,
			NiTriShape* geometry,
			const NativeA8StandardPassLiteDispatch& dispatch,
			const NativeA8DrawCommand& command,
			NiGeometryBufferData* preparedBuffer,
			const NativeA8SegmentDeviceStateStamp*
				deviceStateStamp)
		{
			NiDX9Renderer* renderer = dispatch.renderer;
			TileShader* shader = dispatch.shader;
			const NiPropertyState* properties = dispatch.properties;
			const NativeA8CompiledPacketCommand& program =
				*dispatch.program;
			NativeSegmentDeviceStateCache* deviceState =
				EnterSegmentDeviceStateCache(deviceStateStamp);

			// Retail RenderPassImmediately_Standard first publishes the current
			// property/effect state. Its geometry-group helper is deliberately
			// absent here: formal E88DC0 and the symbolized test build both prove
			// that the exact (skin=null) call has no side effects when
			// modelData->m_pkBuffData is already resident, which stage 2 proved.
			renderer->m_pkCurrProp =
				const_cast<NiPropertyState*>(properties);
			renderer->m_pkCurrEffects = nullptr;

			using SetupStateFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			using SetupDrawmodeFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*, bool);
			using PrepareGeometryFn = void(__thiscall*)(
				TileShader*, NiGeometry*, UInt32,
				NiGeometryBufferData*, const NiPropertyState*);

			// The formal build and the symbolized test build agree on these
			// disjoint effects:
			//   slot 30: programs, declaration, texture stages and clamp;
			//   slot 32: blend enable/function;
			//   slot 33: alpha-test function/reference;
			//   slot 34: cull mode and alpha-test enable.
			// Slot 31 always runs between them and changes constants, scissor and
			// stencil state only; slot 35 restores scissor/stencil only. Cache the
			// four effective outputs independently. The former all-or-nothing
			// aggregate made a page texture or Tile alpha change force every slot
			// to run and therefore produced zero reuse in real traversals.
			const bool firstPass = pass->bIsFirst;
			const bool blendApplicable = firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kPassSuppressesBlendAlpha, currentPass);
			const bool alphaTestApplicable = firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			const bool drawmodeApplicable = setupDrawmode;

			NativeSegmentPassStateKey passState;
			NativeSegmentBlendStateKey blendState;
			NativeSegmentAlphaTestStateKey alphaState;
			NativeSegmentDrawmodeStateKey drawmodeState;
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
					program.setupPass)(shader, properties);
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

			reinterpret_cast<SetupStateFn>(
				program.updateConstants)(shader, properties);
			if (firstPass)
			{
				if (blendApplicable)
				{
					const bool blendKeyReady =
						BuildSegmentBlendStateKey(
							geometry, blendState);
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
							program.setupBlend)(
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
							program.setupAlphaTest)(
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
			else if (*reinterpret_cast<UInt8*>(kFirstPassState)
				&& !CdeclCall<bool>(
					kPassSuppressesBlendAlpha, currentPass))
			{
				reinterpret_cast<SetupStateFn>(
					program.setupNonFirstPass)(shader, properties);
				*reinterpret_cast<UInt8*>(kFirstPassState) = 0;
				if (deviceState)
				{
					// Slot 68 changes blend, Z-write and Z-function state
					// after slot 30. The next pass cannot treat the resulting
					// aggregate as a reusable first-pass setup.
					deviceState->InvalidateStates();
				}
			}
			if (drawmodeApplicable)
			{
				const bool drawmodeKeyReady =
					BuildSegmentDrawmodeStateKey(
						geometry, currentPass, firstPass,
						drawmodeState);
				const bool drawmodeStateReady =
					deviceState && drawmodeKeyReady
					&& deviceState->drawmodeReady
					&& SameSegmentDrawmodeState(
						deviceState->drawmode, drawmodeState);
				if (drawmodeStateReady)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDeviceDrawmodeReuse);
				}
				else
				{
					reinterpret_cast<SetupDrawmodeFn>(
						program.setupDrawmode)(
							shader, properties, firstPass);
					if (deviceState)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceDrawmodeSet);
					}
				}
				if (deviceState)
				{
					deviceState->drawmodeReady =
						drawmodeKeyReady;
					if (drawmodeKeyReady)
						deviceState->drawmode = drawmodeState;
				}
			}

			reinterpret_cast<PrepareGeometryFn>(
				program.prepareGeometry)(
					shader, geometry, 0,
					preparedBuffer, properties);
			A8RenderImmediateAlt(geometry, nullptr, renderer);
			reinterpret_cast<SetupStateFn>(
				program.postGeometry)(shader, properties);
		}

		bool InvokeGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupDrawmode,
			NiTriShape* geometry, const NativeA8DrawCommand* command,
			bool preferStandardPassLite,
			bool packetStatePrevalidated,
			NiGeometryBufferData* preparedBuffer,
			const NativeA8SegmentDeviceStateStamp*
				deviceStateStamp)
		{
			if (preferStandardPassLite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteCandidate);
			}

			bool guardedEligible = false;
			bool liteEnvelope = false;
			const NativeA8StandardPassLiteDispatch* liteDispatch = nullptr;
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
					RecordNativeA8CommandFallback(
					NativeA8CommandFallback::State);
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
					RecordNativeA8CommandFallback(
						NativeA8CommandFallback::State);
				return false;
			}

			if (useStandardPassLite)
			{
				ExecuteStandardPassLite(pass, currentPass,
					testAlpha, blendAlpha, setupDrawmode,
					geometry, *liteDispatch, *command,
					preparedBuffer, deviceStateStamp);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteStage3Replay);
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandNativeReplay);
				return true;
			}

			// The complete stock standard path may touch every device-state
			// category. A later dedicated one-packet Tile must establish a new
			// cache head even when it remains in the same validation segment.
			InvalidateSegmentDeviceStateCache();
			using DefaultPassFn = int(__cdecl*)(
				BSShaderProperty::RenderPass*, bool, bool, bool);
			reinterpret_cast<DefaultPassFn>(kRenderPassImmediatelyStandard)(
				pass, testAlpha, blendAlpha, setupDrawmode);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandNativeReplay);
			return true;
		}

		bool InvokeNativeCommandBootstrap(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupDrawmode,
			NiTriShape* geometry, const NativeA8DrawCommand* command,
			bool preferStandardPassLite = false,
			bool packetStatePrevalidated = false,
			NiGeometryBufferData* preparedBuffer = nullptr,
			const NativeA8SegmentDeviceStateStamp*
				deviceStateStamp = nullptr)
		{
			if (InvokeGuardedNativeReplay(pass, currentPass,
				testAlpha, blendAlpha, setupDrawmode,
				geometry, command, preferStandardPassLite,
				packetStatePrevalidated, preparedBuffer,
				deviceStateStamp))
			{
				return true;
			}
			InvalidateSegmentDeviceStateCache();
			State().originalTileRenderPass(pass, currentPass,
				testAlpha, blendAlpha, setupDrawmode);
			return false;
		}

		void RecordRetainedPacketDraw(
			const NativeA8DrawCommand& command, bool virtualStock,
			bool retainedExtra)
		{
			if (retainedExtra)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandRetainedBridgeDraw);
			}
			if (virtualStock)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockDraw);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
			if (command.packet
				&& command.packet->shaderClass
					== NativeA8ShaderClass::Composite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeDraw);
			}
		}

		struct NativeBridgeExecutionContext
		{
			NativeA8CommandSpanView view;
			NiTriShape* facade = nullptr;
			NativeA8ShapePayload* payload = nullptr;
			NativeA8RingSubmission* ringSubmission = nullptr;
			UInt32 bootstrapCommandOffset = 0;
			UInt32 nextCommandOffset = 0;
			UInt32 endCommandOffset = 0;
			void* boundProfile = nullptr;
			NativeA8CommandBindState bindState;
			UInt32 drewPackets = 0;
			NativeA8FallbackReason failure =
				NativeA8FallbackReason::RuntimeFault;
			const char* operation = "retained-bridge";
			HRESULT result = E_FAIL;
			bool virtualStock = false;
			bool packetStatePrevalidated = false;
			bool failed = false;
			bool constantStateFault = false;
		};

		void FailRetainedBridge(NativeBridgeExecutionContext& context,
			NativeA8FallbackReason failure, const char* operation,
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
			const NativeA8DrawCommand& command, UInt32 commandOffset,
			NiRenderer* renderer, bool alternate)
		{
			NiTriShape* geometry = nullptr;
			if (context.virtualStock)
			{
				geometry = command.expectedGeometry;
			}
			else
			{
				if (!context.facade || !context.payload
					|| !context.ringSubmission
					|| command.packetIndex != commandOffset)
				{
					FailRetainedBridge(context,
						NativeA8FallbackReason::PacketBuild,
						"retained-command-order", E_FAIL);
					return false;
				}
				const NativeA8FallbackReason prepare =
					PrepareNativeA8RingPacket(context.facade,
						*context.payload, *context.ringSubmission,
						command.packetIndex, geometry);
				if (prepare != NativeA8FallbackReason::None
					|| !geometry)
				{
					FailRetainedBridge(context,
						prepare != NativeA8FallbackReason::None
							? prepare
							: NativeA8FallbackReason::RuntimeFault,
						"retained-ring-packet", E_FAIL);
					return false;
				}
			}

			auto drawGeometry = [&]() -> bool
			{
				const bool commandValid =
					context.packetStatePrevalidated
						? GuardNativeA8Command(
							context.view.spanIndex, commandOffset,
							geometry, renderer)
						: ValidateNativeA8Command(
							context.view.spanIndex, commandOffset,
							geometry, renderer);
				if (!commandValid)
				{
					FailRetainedBridge(context,
						NativeA8FallbackReason::PacketPrepare,
						"retained-command-binding", E_FAIL);
					return false;
				}
				const bool publishPrograms =
					context.boundProfile != command.program->profile;
				const char* operation = "none";
				HRESULT result = D3D_OK;
				if (!BindNativeA8CommandPacket(*command.program,
					command.atlasTexture, publishPrograms,
					&geometry->m_kProperties,
					context.bindState,
					operation, result))
				{
					FailRetainedBridge(context,
						NativeA8FallbackReason::RuntimeFault,
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
						NativeA8FallbackReason::RuntimeFault,
						"retained-immediate-missing", E_FAIL);
					return false;
				}
				immediate(geometry, renderer);
				++context.drewPackets;
				RecordRetainedPacketDraw(
					command, context.virtualStock, true);
				if (!IsNativeA8ShaderGenerationCurrent(
					command.program->generation))
				{
					FailRetainedBridge(context,
						NativeA8FallbackReason::DeviceReset,
						"retained-generation-after-draw",
						D3DERR_DEVICELOST);
					return false;
				}
				return true;
			};

			if (!context.virtualStock)
				return drawGeometry();
			if (!command.packet || !command.payload)
			{
				FailRetainedBridge(context,
					NativeA8FallbackReason::PacketBuild,
					"retained-virtual-packet", E_FAIL);
				return false;
			}
			VirtualStockTileStateScope tileState(
				geometry, *command.payload, *command.packet);
			if (!tileState.Active())
			{
				FailRetainedBridge(context,
					NativeA8FallbackReason::PropertySync,
					"retained-virtual-tile-state", E_FAIL);
				return false;
			}
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
				const NativeA8DrawCommand* command =
					ResolveNativeCommand(context->view, commandOffset);
				if (!command || !DrawRetainedBridgeCommand(
					*context, *command, commandOffset,
					renderer, alternate))
				{
					return false;
				}
			}
			const NativeA8DrawCommand* bootstrap =
				ResolveNativeCommand(
					context->view, context->bootstrapCommandOffset);
			if (!bootstrap || !bootstrap->program)
			{
				FailRetainedBridge(*context,
					NativeA8FallbackReason::PacketBuild,
					"retained-bootstrap-command", E_FAIL);
				return false;
			}
			// B994F0's global selected-shader identity still names the bootstrap
			// profile. Restore the corresponding D3D program/texture before its
			// one stock slot-35 cleanup returns, otherwise the next Tile can skip
			// B99390 while the driver still has the final retained profile bound.
			const char* operation = "none";
			HRESULT result = D3D_OK;
			NiTriShape* bootstrapGeometry =
				context->virtualStock
					? bootstrap->expectedGeometry
					: context->ringSubmission
						? context->ringSubmission->proxyShape
						: nullptr;
			if (!BindNativeA8CommandPacket(*bootstrap->program,
				bootstrap->atlasTexture,
				context->boundProfile
					!= bootstrap->program->profile,
				bootstrapGeometry
					? &bootstrapGeometry->m_kProperties : nullptr,
				context->bindState,
				operation, result))
			{
				FailRetainedBridge(*context,
					NativeA8FallbackReason::RuntimeFault,
					operation, result, true);
				return false;
			}
			context->boundProfile = bootstrap->program->profile;
			return true;
		}

		bool ResolveNextBridgeGroup(
			const NativeA8CommandSpanView& view, UInt32& runCursor,
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
			const NativeA8FrameCommandRun& firstRun =
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
				const NativeA8FrameCommandRun& next =
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
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload,
			UInt32 commandSpanIndex,
			NativePacketDrawResult& draw)
		{
			if (!g_bEnableFreeTypeFontCommandBuffer
				|| !pass || !facade
				|| commandSpanIndex == kInvalidNativeA8CommandIndex)
			{
				return false;
			}
			if (currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass))
			{
				RecordNativeA8CommandFallback(
					NativeA8CommandFallback::State);
				return false;
			}

			NativeA8CommandSpanView view;
			if (!BeginNativeA8CommandSpanExecution(
				commandSpanIndex, facade, false, view))
			{
				return false;
			}
			if (!view.span || view.span->virtualStock
				|| view.span->payload != &payload
				|| view.span->commandCount < 2
				|| !view.span->bridgeEligible)
			{
				EndNativeA8CommandSpanExecution(
					commandSpanIndex, false, false);
				RecordNativeA8CommandFallback(
					NativeA8CommandFallback::Topology);
				return false;
			}

			FreeTypePerfScope submitPerf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit);
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NativeRingSubmissionScope ringScope;
			const NativeA8FallbackReason ringFailure =
				BeginNativeA8RingSubmission(
					facade, payload, ringScope.submission);
			if (ringFailure != NativeA8FallbackReason::None)
			{
				EndNativeA8CommandSpanExecution(
					commandSpanIndex, false, false);
				return false;
			}

			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
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
						NativeA8FallbackReason::PacketBuild;
					draw.operation = "retained-run-topology";
					draw.result = E_FAIL;
					break;
				}
				const NativeA8DrawCommand* first =
					ResolveNativeCommand(view, firstOffset);
				NiTriShape* proxy = nullptr;
				if (!first || first->packetIndex != firstOffset)
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::PacketBuild;
					draw.operation = "retained-first-command";
					draw.result = E_FAIL;
					break;
				}
				const NativeA8FallbackReason prepare =
					PrepareNativeA8RingPacket(facade, payload,
						ringScope.submission,
						first->packetIndex, proxy);
				if (prepare != NativeA8FallbackReason::None
					|| !proxy)
				{
					draw.runtimeFault = true;
					draw.failure =
						prepare != NativeA8FallbackReason::None
							? prepare
							: NativeA8FallbackReason::RuntimeFault;
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
						NativeA8FallbackReason::RuntimeFault;
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
					setupDrawmode);
				// PrepareNativeA8RingPacket just established the exact retained
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
					false, true, setupDrawmode, proxy, first,
					false, true);
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordRetainedPacketDraw(
						*first, false, false);
				}
				if (bridge.drewPackets)
				{
					draw.drewPacket = true;
					draw.drawnPacketCount += bridge.drewPackets;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandStockBootstrapSaved,
						bridge.drewPackets);
				}
				if (!immediateScope.Invoked()
					|| !immediateScope.ValidationPassed()
					|| !immediateScope.Drew())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::RuntimeFault;
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
				&& draw.drawnPacketCount
					== view.span->commandCount;
			EndNativeA8CommandSpanExecution(
				commandSpanIndex, success, draw.drewPacket);
			if (success)
				return true;
			// No command reached the driver: the unmodified current path can
			// safely acquire a fresh proxy and render the complete payload.
			return draw.drewPacket || draw.constantStateFault;
		}

		bool TryDrawVirtualStockRetainedSpan(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* leader,
			const A8ShapeMetadata& metadata, UInt64 validationToken,
			UInt32 commandSpanIndex,
			NativePacketDrawResult& draw)
		{
			VirtualStockShapeGroup* group =
				metadata.virtualStockGroup;
			if (!g_bEnableFreeTypeFontCommandBuffer
				|| !pass || !leader || !group || !validationToken
				|| commandSpanIndex == kInvalidNativeA8CommandIndex)
			{
				return false;
			}
			if (currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass))
			{
				RecordNativeA8CommandFallback(
					NativeA8CommandFallback::State);
				return false;
			}

			NativeA8CommandSpanView view;
			if (!BeginNativeA8CommandSpanExecution(
				commandSpanIndex, leader, true, view))
			{
				return false;
			}
			if (!view.span || !view.span->virtualStock
				|| view.span->virtualStockGroup != group
				|| !view.span->payload
				|| view.span->commandCount < 2
				|| !view.span->bridgeEligible)
			{
				EndNativeA8CommandSpanExecution(
					commandSpanIndex, false, false);
				RecordNativeA8CommandFallback(
					NativeA8CommandFallback::Topology);
				return false;
			}

			NativeA8ShapePayload* payload = view.span->payload;
			{
				std::lock_guard<std::mutex> lock(group->mutex);
				if (!group->primaryMetadataOwner
					|| payload
						!= &group->primaryMetadataOwner->nativePayload
					|| group->preparedValidationToken
						!= validationToken
					|| group->frameMode.load(
						std::memory_order_acquire)
						!= VirtualStockFrameMode::Direct
					|| group->slots.size()
						!= view.span->commandCount
					|| group->duplicateRegistration
					|| !group->registrationContiguous
					|| group->registeredSlotCount
						!= group->slots.size()
					|| metadata.virtualStockSlot
						!= view.span->leaderSlot)
				{
					EndNativeA8CommandSpanExecution(
						commandSpanIndex, false, false);
					RecordNativeA8CommandFallback(
						NativeA8CommandFallback::Topology);
					return false;
				}
				for (UInt32 index = 0;
					index < view.span->commandCount; ++index)
				{
					const NativeA8DrawCommand* command =
						ResolveNativeCommand(view, index);
					if (!command || command->packetIndex != index
						|| group->slots[index].packetIndex != index
						|| group->slots[index].shape
							!= command->expectedGeometry)
					{
						EndNativeA8CommandSpanExecution(
							commandSpanIndex, false, false);
						RecordNativeA8CommandFallback(
							NativeA8CommandFallback::Topology);
						return false;
					}
				}
			}

			FreeTypePerfScope submitPerf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit);
			draw.directShapeRoute = true;
			draw.stockLikeBitmapRoute =
				payload->stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			NiRenderer* commandRenderer = renderer
				? renderer : NiDX9Renderer::GetSingleton();
			if (!ValidateNativeA8VirtualCommandRange(
					commandSpanIndex, 0,
					view.span->commandCount, commandRenderer))
			{
				EndNativeA8CommandSpanExecution(
					commandSpanIndex, false, false);
				return false;
			}
			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
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
						NativeA8FallbackReason::PacketBuild;
					draw.operation =
						"retained-virtual-run-topology";
					draw.result = E_FAIL;
					break;
				}
				const NativeA8DrawCommand* first =
					ResolveNativeCommand(view, firstOffset);
				NiTriShape* geometry =
					first ? first->expectedGeometry : nullptr;
				if (!first || !geometry || !first->packet)
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::PacketBuild;
					draw.operation =
						"retained-virtual-first-command";
					draw.result = E_FAIL;
					break;
				}
				VirtualStockTileStateScope tileState(
					geometry, *payload, *first->packet);
				if (!tileState.Active())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::PropertySync;
					draw.operation =
						"retained-virtual-first-state";
					draw.result = E_FAIL;
					break;
				}
				packetScope.Select(geometry);
				NativeImmediateHookVtableScope hookVtable(geometry);
				if (!hookVtable.Active())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::RuntimeFault;
					draw.operation =
						"retained-virtual-immediate-vtable";
					draw.result = E_FAIL;
					break;
				}

				NativeBridgeExecutionContext bridge;
				bridge.view = view;
				bridge.payload = payload;
				bridge.bootstrapCommandOffset = firstOffset;
				bridge.nextCommandOffset = firstOffset + 1u;
				bridge.endCommandOffset = endOffset;
				bridge.boundProfile = first->program
					? first->program->profile : nullptr;
				bridge.bindState = MakeNativeCommandBindState(
					pass, currentPass, false, true,
					setupDrawmode);
				bridge.virtualStock = true;
				bridge.packetStatePrevalidated = true;
				const bool hasContinuation =
					bridge.nextCommandOffset
						< bridge.endCommandOffset;
				NativeDirectImmediateScope immediateScope(
					geometry, commandSpanIndex, firstOffset, true,
					hasContinuation ? &bridge : nullptr,
					hasContinuation
						? &ContinueRetainedBridge : nullptr,
					NativeImmediateCommandKind::SpanPacket, true);
				InvokeNativeCommandBootstrap(pass, currentPass,
					false, true, setupDrawmode, geometry, first,
					false, true);
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordRetainedPacketDraw(
						*first, true, false);
				}
				if (bridge.drewPackets)
				{
					draw.drewPacket = true;
					draw.drawnPacketCount += bridge.drewPackets;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandStockBootstrapSaved,
						bridge.drewPackets);
				}
				if (!immediateScope.Invoked()
					|| !immediateScope.ValidationPassed()
					|| !immediateScope.Drew())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeA8FallbackReason::RuntimeFault;
					draw.operation =
						"retained-virtual-bootstrap-immediate";
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
					"retained-virtual-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}

			if (draw.drawnPacketCount)
			{
				group->directDrawCount.fetch_add(
					draw.drawnPacketCount,
					std::memory_order_acq_rel);
			}
			const bool success = !draw.runtimeFault
				&& draw.drawnPacketCount
					== view.span->commandCount;
			EndNativeA8CommandSpanExecution(
				commandSpanIndex, success, draw.drewPacket);
			if (success)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandVirtualSpanFused);
				return true;
			}
			return draw.drewPacket || draw.constantStateFault;
		}

		bool TryDrawVirtualStockPacket(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* shape,
			const A8ShapeMetadata& metadata, UInt64 validationToken,
			NativePacketDrawResult& draw,
			UInt32 commandSpanIndex =
				kInvalidNativeA8CommandIndex,
			UInt32 commandOffset =
				kInvalidNativeA8CommandIndex,
			UInt32 virtualSinglePacketCommandIndex =
				kInvalidNativeA8CommandIndex)
		{
			VirtualStockShapeGroup* group =
				metadata.virtualStockGroup;
			if (!pass || !shape || !group || !validationToken)
			{
				return false;
			}
			FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeA8CommandIndex
					|| virtualSinglePacketCommandIndex
						!= kInvalidNativeA8CommandIndex);

			const NativeA8ShapePayload* payload = nullptr;
			A8ShapeMetadataPtr primaryMetadataOwner;
			const NativeA8PacketTemplate* packet = nullptr;
			NativeA8VirtualStockPacketBinding binding;
			NiGeometryBufferData* expectedBuffer = nullptr;
			NiVBChip* expectedChip = nullptr;
			TileShader* expectedShader = nullptr;
			UInt32 packetIndex = 0;
			{
				std::lock_guard<std::mutex> lock(group->mutex);
				if (!group->primaryMetadataOwner
					|| metadata.virtualStockSlot >= group->slots.size()
					|| group->preparedValidationToken != validationToken
					|| group->frameMode.load(std::memory_order_acquire)
						!= VirtualStockFrameMode::Direct)
				{
					return false;
				}
				if (metadata.virtualStockPrimary)
				{
					if (group->primaryMetadataOwner.get() != &metadata)
						return false;
					payload = &metadata.nativePayload;
				}
				else
				{
					primaryMetadataOwner = group->primaryMetadataOwner;
					payload = &primaryMetadataOwner->nativePayload;
				}
				if (!payload->buildComplete || !payload->payloadTemplate)
					return false;
				const std::vector<NativeA8PacketTemplate>& packets =
					GetNativeA8Packets(*payload->payloadTemplate,
						payload->useCompositePackets);
				const VirtualStockSlotBinding& slot =
					group->slots[metadata.virtualStockSlot];
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
			}

			NativeA8CommandSpanView commandView;
			NativeA8VirtualSinglePacketCommandView
				virtualSingleCommandView;
			const NativeA8DrawCommand* command = nullptr;
			bool commandExecution = false;
			bool commandBegun = false;
			bool virtualSingleCommandExecution = false;
			if (g_bEnableFreeTypeFontCommandBuffer
				&& virtualSinglePacketCommandIndex
					!= kInvalidNativeA8CommandIndex)
			{
				commandBegun =
					BeginNativeA8VirtualSinglePacketCommandExecution(
						virtualSinglePacketCommandIndex,
						group, shape, virtualSingleCommandView);
				if (commandBegun
					&& virtualSingleCommandView.command)
				{
					command =
						virtualSingleCommandView.command->draw;
					commandExecution = command
						&& command->payload == payload
						&& command->expectedGeometry == shape
						&& command->packetIndex == packetIndex;
					virtualSingleCommandExecution =
						commandExecution;
				}
				if (commandBegun && !commandExecution)
				{
					EndNativeA8VirtualSinglePacketCommandExecution(
						virtualSinglePacketCommandIndex,
						false, false);
				}
			}
			if (!commandExecution
				&& g_bEnableFreeTypeFontCommandBuffer
				&& commandSpanIndex
					!= kInvalidNativeA8CommandIndex)
			{
				NativeA8CommandSpanView candidate;
				if (FindNativeA8CommandSpan(commandSpanIndex,
						validationToken, candidate)
					&& candidate.span
					&& candidate.span->virtualStock
					&& candidate.span->virtualStockGroup == group
					&& candidate.span->payload == payload
					&& candidate.span->commandCount == 1
					&& commandOffset == 0)
				{
					commandBegun =
						BeginNativeA8CommandSpanExecution(
							commandSpanIndex, shape, true,
							commandView);
					if (commandBegun)
					{
						command = ResolveNativeCommand(
							commandView, 0);
						commandExecution = command
							&& command->expectedGeometry == shape
							&& command->packetIndex == packetIndex;
					}
				}
				if (commandBegun && !commandExecution)
				{
					EndNativeA8CommandSpanExecution(
						commandSpanIndex, false, false);
				}
			}

			draw.directShapeRoute = true;
			draw.stockLikeBitmapRoute = payload->stockLikeBitmapPackets;
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
					== sizeof(NativeA8GpuVertex)
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
						* sizeof(NativeA8GpuVertex)
				&& binding.vertexCount == packet->vertexCount
				&& IsNativeA8VirtualStockPacketAtlasCurrent(
					shape, *payload, packetIndex);
			const bool bindingCurrent = bindingDescriptorCurrent
				&& (commandExecution
					? command
						&& SameNativePacketBinding(
							command->binding, binding)
					: validationToken
							== GetNativeA8SortedFrameValidationToken()
						&& IsNativeA8VirtualStockPacketBindingCurrent(
							binding));
			if (!bindingCurrent)
			{
				draw.runtimeFault = true;
				draw.failure = NativeA8FallbackReason::PacketPrepare;
				draw.operation = "virtual-stock-binding";
				draw.result = E_FAIL;
			}

			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.runtimeFault && !draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
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
				VirtualStockTileStateScope tileState(
					shape, *payload, *packet);
				if (!tileState.Active())
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::PropertySync;
					draw.operation = "virtual-stock-tile-state";
					draw.result = E_FAIL;
				}
				else
				{
					const UInt32 validationCommandIndex =
						commandExecution
							? (virtualSingleCommandExecution
								? virtualSinglePacketCommandIndex
								: commandSpanIndex)
							: kInvalidNativeA8CommandIndex;
					NativeDirectImmediateScope immediateScope(
						shape, validationCommandIndex,
						validationCommandIndex
								!= kInvalidNativeA8CommandIndex
							&& !virtualSingleCommandExecution
							? commandOffset
							: kInvalidNativeA8CommandIndex,
						commandExecution, nullptr, nullptr,
						virtualSingleCommandExecution
							? NativeImmediateCommandKind::
								VirtualSinglePacket
							: NativeImmediateCommandKind::SpanPacket,
						commandExecution && bindingCurrent);
					bool usedNativeReplay = false;
					if (commandExecution)
					{
						NativeA8SegmentDeviceStateStamp
							segmentDeviceStateStamp;
						const NativeA8SegmentDeviceStateStamp*
							segmentDeviceState = nullptr;
						if (virtualSingleCommandExecution
							&& virtualSingleCommandView.stamp
							&& virtualSingleCommandView.command
							&& BuildSegmentDeviceStateStamp(
								virtualSingleCommandView.stamp,
								virtualSingleCommandView.command->
									executionSegmentEpoch,
								virtualSingleCommandView.command->
									executionExternalMutationEpoch,
								segmentDeviceStateStamp))
						{
							segmentDeviceState =
								&segmentDeviceStateStamp;
						}
						usedNativeReplay =
							InvokeNativeCommandBootstrap(pass,
							currentPass, false, true,
							setupDrawmode, shape, command,
							virtualSingleCommandExecution,
							bindingCurrent,
							expectedBuffer,
							segmentDeviceState);
					}
					else
					{
						InvalidateSegmentDeviceStateCache();
						State().originalTileRenderPass(pass,
							currentPass, false, true,
							setupDrawmode);
					}
					if (immediateScope.Drew())
					{
						draw.drewPacket = true;
						draw.drawnPacketCount = 1;
						group->directDrawCount.fetch_add(
							1, std::memory_order_acq_rel);
						RecordFreeTypePerf(
							FreeTypePerfCounter::VirtualStockDraw);
						RecordFreeTypePerf(
							FreeTypePerfCounter::TilePass);
						if (usedNativeReplay
							&& virtualSingleCommandExecution)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									CommandVirtualSinglePacketReplay);
						}
						if (packet->shaderClass
							== NativeA8ShaderClass::Composite)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::CompositeDraw);
						}
					}
					else
					{
						draw.runtimeFault = true;
						draw.failure =
							NativeA8FallbackReason::RuntimeFault;
						draw.operation = immediateScope.Invoked()
							? "virtual-stock-command-validation"
							: "virtual-stock-immediate-not-invoked";
						draw.result = E_FAIL;
					}
				}
				if (!IsNativeA8ShaderGenerationCurrent(
					payload->preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::DeviceReset;
					draw.operation =
						"generation-changed-after-virtual-stock";
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
					"virtual-stock-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_constantOwnershipBatch.Operation();
				draw.result = s_constantOwnershipBatch.Result();
				draw.mismatchRegister =
					s_constantOwnershipBatch.MismatchRegister();
			}
			if (commandExecution)
			{
				if (virtualSingleCommandExecution)
				{
					EndNativeA8VirtualSinglePacketCommandExecution(
						virtualSinglePacketCommandIndex,
						!draw.runtimeFault && draw.drewPacket,
						draw.drewPacket);
				}
				else
				{
					EndNativeA8CommandSpanExecution(
						commandSpanIndex,
						!draw.runtimeFault && draw.drewPacket,
						draw.drewPacket);
				}
			}
			if (draw.runtimeFault)
			{
				const bool facadeFallbackSafe =
					!draw.drewPacket && !draw.constantStateFault
					&& group->directDrawCount.load(
						std::memory_order_acquire) == 0;
				if (facadeFallbackSafe)
				{
					std::shared_ptr<VirtualStockShapeGroup> groupOwner =
						AcquireVirtualStockShapeGroup(metadata);
					if (groupOwner)
					{
						RestoreVirtualStockGroupToFacade(
							groupOwner, draw.failure);
					}
					else
					{
						group->frameMode.store(
							VirtualStockFrameMode::Fault,
							std::memory_order_release);
					}
				}
				else
				{
					std::lock_guard<std::mutex> lock(group->mutex);
					if (group->frameMode.load(
						std::memory_order_acquire)
						!= VirtualStockFrameMode::Retired)
					{
						group->frameMode.store(
							VirtualStockFrameMode::Fault,
							std::memory_order_release);
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFallbackResource);
				}
			}
			return true;
		}

		NativePacketDrawResult DrawNativePacketSet(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload,
			UInt32 commandSpanIndex =
				kInvalidNativeA8CommandIndex)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeA8CommandIndex);
			InvalidateSegmentDeviceStateCache();
			NativePacketDrawResult draw;
			NativeRingSubmissionScope ringScope;
			NativeA8FallbackReason failure = BeginNativeA8RingSubmission(
				facade, payload, ringScope.submission);
			if (failure != NativeA8FallbackReason::None)
			{
				draw.runtimeFault = true;
				draw.failure = failure;
				draw.operation = "ring-submission";
			}
			NativeTilePacketScope packetScope(pass);
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}
			// Retail NiDX9Renderer's indexed-array loop cannot express
			// triangle-list page boundaries: m_pusArrayLengths is interpreted as
			// strip lengths (primitiveCount = length - 2). Keep the page split at
			// the one stock sorted Tile callsite instead. Each packet selects a
			// Font::MakeTriShape proxy and then executes the untouched
			// TileRenderPass -> NiTriShape::RenderImmediate renderer path.
			//
			// Final ARGB and baked-coverage bitmaps use only stock c0. Skipping the
			// distance-field private-high-range ownership and facade bookkeeping
			// removes the only per-facade isolation work from this stock-like
			// multipage route.
			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
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
					const NativeA8FallbackReason packetFailure =
						PrepareNativeA8RingPacket(facade, payload,
							ringScope.submission, packetIndex, proxyShape);
					if (packetFailure != NativeA8FallbackReason::None
						|| !proxyShape)
					{
						draw.runtimeFault = true;
						draw.failure =
							packetFailure != NativeA8FallbackReason::None
								? packetFailure
								: NativeA8FallbackReason::RuntimeFault;
						draw.operation = "ring-packet";
						break;
					}
					packetScope.Select(proxyShape);
					State().originalTileRenderPass(pass,
						currentPass, false, true,
						setupDrawmode);
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					const std::vector<NativeA8PacketTemplate>& activePackets =
						GetNativeA8Packets(*payload.payloadTemplate,
							payload.useCompositePackets);
					if (packetIndex < activePackets.size()
						&& activePackets[packetIndex].shaderClass
							== NativeA8ShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
					if (!IsNativeA8ShaderGenerationCurrent(
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
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload,
			NativePacketDrawResult& draw,
			UInt32 commandSpanIndex =
				kInvalidNativeA8CommandIndex,
			UInt32 singlePacketCommandIndex =
				kInvalidNativeA8CommandIndex)
		{
			if (!pass || !facade || !payload.buildComplete
				|| !payload.payloadTemplate
				|| payload.packetShaders.size() != 1)
			{
				return false;
			}
			const NativeA8PayloadTemplate& artifact =
				*payload.payloadTemplate;
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(artifact, payload.useCompositePackets);
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
				commandSpanIndex != kInvalidNativeA8CommandIndex
					|| singlePacketCommandIndex
						!= kInvalidNativeA8CommandIndex);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectCandidate);

			const bool commandRequested =
				g_bEnableFreeTypeFontCommandBuffer
				&& (singlePacketCommandIndex
						!= kInvalidNativeA8CommandIndex
					|| commandSpanIndex
						!= kInvalidNativeA8CommandIndex);
			NativeA8CommandSpanView commandView;
			NativeA8SinglePacketCommandView singleCommandView;
			const NativeA8DrawCommand* command = nullptr;
			bool commandExecution = false;
			bool singleCommandExecution = false;
			if (commandRequested
				&& singlePacketCommandIndex
					!= kInvalidNativeA8CommandIndex
				&& BeginNativeA8SinglePacketCommandExecution(
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
					RecordNativeA8CommandFallback(
						NativeA8CommandFallback::Topology);
					EndNativeA8SinglePacketCommandExecution(
						singlePacketCommandIndex, false, false);
				}
			}
			else if (commandRequested
				&& commandSpanIndex
					!= kInvalidNativeA8CommandIndex
				&& BeginNativeA8CommandSpanExecution(
					commandSpanIndex, facade, false, commandView))
			{
				if (commandView.span
					&& !commandView.span->virtualStock
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
					EndNativeA8CommandSpanExecution(
						commandSpanIndex, false, false);
				}
			}

			auto endCommandExecution =
				[&](bool success, bool drewPacket)
			{
				if (singleCommandExecution)
				{
					EndNativeA8SinglePacketCommandExecution(
						singlePacketCommandIndex,
						success, drewPacket);
				}
				else
				{
					EndNativeA8CommandSpanExecution(
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
				const NativeA8FallbackReason submissionFailure =
					BeginNativeA8DirectShapeSubmission(
						facade, payload, submissionScope.submission);
				if (submissionFailure != NativeA8FallbackReason::None)
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
						RecordNativeA8CommandFallback(
							NativeA8CommandFallback::Resource);
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
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
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
						: kInvalidNativeA8CommandIndex;
				NativeDirectImmediateScope immediateScope(
					facade, validationCommandIndex,
					commandExecution && !singleCommandExecution
						? 0u : kInvalidNativeA8CommandIndex,
					commandExecution, nullptr, nullptr,
					singleCommandExecution
						? NativeImmediateCommandKind::SinglePacket
						: NativeImmediateCommandKind::SpanPacket,
					commandExecution);
				bool usedNativeReplay = false;
				if (commandExecution)
				{
					NativeA8SegmentDeviceStateStamp
						segmentDeviceStateStamp;
					const NativeA8SegmentDeviceStateStamp*
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
						false, true, setupDrawmode,
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
					State().originalTileRenderPass(pass,
						currentPass, false, true,
						setupDrawmode);
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
						== NativeA8ShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
				}
				else
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::RuntimeFault;
					draw.operation = immediateScope.Invoked()
						? "direct-shape-command-validation"
						: "direct-shape-immediate-not-invoked";
					draw.result = E_FAIL;
				}
				if (!IsNativeA8ShaderGenerationCurrent(
					payload.preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::RuntimeFault;
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
			// Once the stock Tile pass has been entered, this route owns the item
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
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(shape);
			if (!metadata)
			{
				LogMissingMetadata(shape, phase);
				return;
			}
			RecordNativeA8Suppression(shape, *metadata,
				metadata->nativePayload.buildComplete
					? NativeA8FallbackReason::DirectImmediate
					: NativeA8FallbackReason::PacketBuild,
				phase);
		}

		struct A8MetadataIdentitySnapshot
		{
			UInt64 allocationId = 0;
			const A8ShapeMetadata* selfIdentity = nullptr;
			const NiTriShape* shapeIdentity = nullptr;
			UInt32 fontId = 0;
			UInt32 backend = 0;
			UInt32 virtualStockSlot = 0;
			bool virtualStockPrimary = false;
			bool buildComplete = false;
		};

		bool TryReadA8MetadataIdentity(
			const A8ShapeMetadata* metadata,
			A8MetadataIdentitySnapshot& snapshot)
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
				snapshot.virtualStockSlot =
					static_cast<UInt32>(metadata->virtualStockSlot);
				snapshot.virtualStockPrimary =
					metadata->virtualStockPrimary;
				snapshot.buildComplete =
					metadata->nativePayload.buildComplete;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool LogA8MetadataDeleteAudit(NiTriShape* shape,
			bool registryFound, const A8ShapeMetadataEntry& entry)
		{
			const A8ShapeMetadata* mappedMetadata =
				entry.metadata.get();
			A8MetadataIdentitySnapshot snapshot;
			const bool readable = TryReadA8MetadataIdentity(
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
			if (!g_bEnableFreeTypeFontRenderingLog && !logFailure)
				return integrity;

			gLog.FormattedMessage(
				"tnvse_freetype_native: metadata-delete-pre shape=%p registry=%u mapped=%p expectedSelf=%p expectedShape=%p allocationId=%llu readable=%u objectAllocationId=%llu objectSelf=%p objectShape=%p font=%u backend=%u slot=%u primary=%u build=%u registryIdentity=%u pointer=%u allocation=%u self=%u shapeIdentity=%u integrity=%u",
				shape, registryFound ? 1u : 0u,
				mappedMetadata, entry.selfIdentity,
				entry.shapeIdentity,
				static_cast<unsigned long long>(entry.allocationId),
				readable ? 1u : 0u,
				static_cast<unsigned long long>(
					snapshot.allocationId),
				snapshot.selfIdentity, snapshot.shapeIdentity,
				snapshot.fontId, snapshot.backend,
				snapshot.virtualStockSlot,
				snapshot.virtualStockPrimary ? 1u : 0u,
				snapshot.buildComplete ? 1u : 0u,
				registryIdentityValid ? 1u : 0u,
				pointerMatch ? 1u : 0u,
				allocationMatch ? 1u : 0u,
				selfMatch ? 1u : 0u,
				shapeMatch ? 1u : 0u,
				integrity ? 1u : 0u);
			return integrity;
		}

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			A8State& state = State();
			InvalidateNativeA8CommandGeometry(shape);
			A8ShapeMetadataEntry retiredEntry;
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
				LogA8MetadataDeleteAudit(
				shape, registryFound, retiredEntry);
			A8ShapeMetadataPtr retiredMetadata =
				std::move(retiredEntry.metadata);
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->nativePayload.buildComplete)
			{
				InvalidateNativeA8TileRetainedText(
					retiredMetadata->nativePayload);
			}
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->backend
					== FreeTypeShapeBackend::VirtualStockNative)
			{
				ReleaseVirtualStockShapeBinding(
					shape, *retiredMetadata);
			}
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
		}
	}

	void BeginA8SortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		s_constantOwnershipBatch.BeginFrame();
	}

	void EndA8SortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		ReleaseNativeConstantOwnershipBatch("sorted-frame-end");
		s_constantOwnershipBatch.EndFrame();
	}

	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape)
	{
		if (!shape)
			return {};
		A8State& state = State();
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		A8MetadataHotSet& hotSet = GetMetadataHotSet(shape);
		A8MetadataHotEntry* replacement = nullptr;
		for (A8MetadataHotEntry& hot : hotSet.ways)
		{
			if (hot.shape != shape)
				continue;
			if (hot.generation == generation)
			{
				A8ShapeMetadataPtr metadata = hot.metadata.lock();
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
		A8MetadataHotEntry& hot = replacement
			? *replacement : SelectMetadataHotVictim(hotSet);
		hot.shape = shape;
		hot.generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_relaxed);
		hot.metadata = found->second.metadata;
		return found->second.metadata;
	}

	std::shared_ptr<VirtualStockShapeGroup>
		AcquireVirtualStockShapeGroup(const A8ShapeMetadata& metadata)
	{
		VirtualStockShapeGroup* group = metadata.virtualStockGroup;
		if (!group)
			return {};
		std::shared_ptr<VirtualStockShapeGroup> owner =
			metadata.virtualStockOwner.lock();
		return owner && owner.get() == group
			&& owner->metadataPublished.load(std::memory_order_acquire)
			? owner : std::shared_ptr<VirtualStockShapeGroup>{};
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
			if (s_constantOwnershipBatch.FrameActive())
				ReleaseNativeConstantOwnershipBatch("before-stock-tile");
			InvalidateSegmentDeviceStateCache();
			InvalidateNativeA8SortedShaderState();
			state.originalTileRenderPass(pass, currentPass, testAlpha,
				blendAlpha, setupDrawmode);
			return;
		}

		NativeA8SortedFrameEntryView frameEntry;
		const bool sortedFrameHit =
			FindNativeA8SortedFrameEntry(shape, frameEntry);
		A8ShapeMetadataPtr metadataOwner;
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		if (sortedFrameHit)
		{
			metadata = frameEntry.metadata;
			payload = frameEntry.payload;
		}
		else
		{
			metadataOwner = FindA8ShapeMetadata(shape);
			metadata = metadataOwner.get();
			if (metadata && metadata->nativePayload.buildComplete)
				payload = &metadata->nativePayload;
		}
		if (!metadata)
		{
			LogMissingMetadata(shape, "tile-render-pass");
			return;
		}
		if (metadata->backend
			== FreeTypeShapeBackend::VirtualStockNative)
		{
			VirtualStockShapeGroup* group =
				metadata->virtualStockGroup;
			if (!group)
			{
				RecordNativeA8Suppression(shape, *metadata,
					NativeA8FallbackReason::PacketBuild,
					"virtual-stock-tile");
				return;
			}
			const VirtualStockFrameMode mode =
				group->frameMode.load(std::memory_order_acquire);
			if (mode == VirtualStockFrameMode::Culled
				|| mode == VirtualStockFrameMode::Fault
				|| mode == VirtualStockFrameMode::Retired)
			{
				if (!metadata->virtualStockPrimary)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
				}
				return;
			}

			const UInt64 validationToken =
				GetNativeA8SortedFrameValidationToken();
			if (mode == VirtualStockFrameMode::Direct)
			{
				NativePacketDrawResult draw;
				bool handled = false;
				const UInt64 commandToken =
					group->commandValidationToken.load(
						std::memory_order_acquire);
				const UInt32 commandSpanIndex =
					group->commandSpanIndex.load(
						std::memory_order_acquire);
				const UInt32 virtualSinglePacketCommandIndex =
					group->commandVirtualSinglePacketIndex.load(
						std::memory_order_acquire);
				const UInt32 commandLeaderSlot =
					group->commandLeaderSlot.load(
						std::memory_order_acquire);
				const bool commandCurrent = commandToken
					&& commandToken == validationToken
					&& (commandSpanIndex
							!= kInvalidNativeA8CommandIndex
						|| virtualSinglePacketCommandIndex
							!= kInvalidNativeA8CommandIndex);
				if (commandCurrent
					&& g_bEnableFreeTypeFontCommandBuffer)
				{
					if (commandSpanIndex
							!= kInvalidNativeA8CommandIndex
						&& metadata->virtualStockSlot
						!= commandLeaderSlot)
					{
						if (ShouldConsumeNativeA8CommandFollower(
							commandSpanIndex, validationToken,
							shape,
							metadata->virtualStockSlot))
						{
							return;
						}
					}
					else if (commandSpanIndex
						!= kInvalidNativeA8CommandIndex)
					{
						NativeA8CommandSpanView candidate;
						if (FindNativeA8CommandSpan(
								commandSpanIndex,
								validationToken, candidate)
							&& candidate.span
							&& candidate.span->commandCount > 1)
						{
							handled =
								TryDrawVirtualStockRetainedSpan(
									pass, currentPass,
									setupDrawmode, shape,
									*metadata, validationToken,
									commandSpanIndex, draw);
						}
					}
				}
				if (!handled)
				{
					draw = {};
					const UInt32 packetCommandSpanIndex =
						commandCurrent
							&& commandSpanIndex
								!= kInvalidNativeA8CommandIndex
							? commandSpanIndex
							: kInvalidNativeA8CommandIndex;
					const UInt32 packetVirtualSingleCommandIndex =
						commandCurrent
							&& virtualSinglePacketCommandIndex
								!= kInvalidNativeA8CommandIndex
							? virtualSinglePacketCommandIndex
							: kInvalidNativeA8CommandIndex;
					handled = TryDrawVirtualStockPacket(
						pass, currentPass, setupDrawmode, shape,
						*metadata, validationToken, draw,
						packetCommandSpanIndex,
						packetCommandSpanIndex
								!= kInvalidNativeA8CommandIndex
							? metadata->virtualStockSlot
							: kInvalidNativeA8CommandIndex,
						packetVirtualSingleCommandIndex);
				}
				if (!handled)
				{
					if (validationToken)
					{
						if (group->directDrawCount.load(
							std::memory_order_acquire) == 0)
						{
							std::shared_ptr<VirtualStockShapeGroup>
								groupOwner =
									AcquireVirtualStockShapeGroup(
										*metadata);
							if (groupOwner)
							{
								RestoreVirtualStockGroupToFacade(
									groupOwner,
									NativeA8FallbackReason::
										PacketPrepare);
							}
							else
							{
								group->frameMode.store(
									VirtualStockFrameMode::Fault,
									std::memory_order_release);
							}
						}
						else
						{
							std::lock_guard<std::mutex> lock(
								group->mutex);
							if (group->frameMode.load(
								std::memory_order_acquire)
								!= VirtualStockFrameMode::Retired)
							{
								group->frameMode.store(
									VirtualStockFrameMode::Fault,
									std::memory_order_release);
							}
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									VirtualStockFallbackResource);
						}
						if (group->frameMode.load(
							std::memory_order_acquire)
							!= VirtualStockFrameMode::Facade)
						{
							return;
						}
					}
				}
				else if (!draw.runtimeFault)
					return;
				else
				{
					InvalidateNativeA8SortedShaderState();
					A8ShapeMetadataPtr primaryMetadata;
					{
						std::lock_guard<std::mutex> lock(group->mutex);
						primaryMetadata = group->primaryMetadataOwner;
					}
					NativeA8ShapePayload* primaryPayload =
						primaryMetadata
							? &primaryMetadata->nativePayload : nullptr;
					if (draw.constantStateFault && primaryPayload)
					{
						MarkNativeA8GenerationFault(
							primaryPayload->preparedGeneration,
							draw.operation, draw.result);
						gLog.FormattedMessage(
							"tnvse_freetype_native: virtual-stock pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-group",
							draw.operation,
							static_cast<UInt32>(draw.result),
							draw.mismatchRegister, shape,
							metadata->fontId,
							primaryPayload->preparedGeneration,
							draw.drewPacket ? 1 : 0);
					}
					if (draw.drewPacket && primaryPayload)
					{
						MarkNativeA8RuntimeFault(
							*metadata, *primaryPayload,
							draw.failure);
					}
					if (draw.drewPacket || draw.constantStateFault
						|| group->frameMode.load(
							std::memory_order_acquire)
							!= VirtualStockFrameMode::Facade)
					{
						return;
					}
				}
			}

			if (!metadata->virtualStockPrimary)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						VirtualStockFollowerSkipped);
				return;
			}
			// The primary owns the complete payload and therefore remains the
			// sole compatibility facade whenever the frozen real-shape topology
			// is not ready for this traversal.
			payload = &metadata->nativePayload;
		}
		if (payload)
		{
			const bool needsVisibilityCheck = !sortedFrameHit
				|| frameEntry.visibilityCull
					!= NativeA8VisibilityCull::None;
			if (needsVisibilityCheck)
			{
				const NativeA8VisibilityCull visibilityCull =
					EvaluateNativeA8SubmissionVisibility(shape, *payload);
				if (visibilityCull != NativeA8VisibilityCull::None)
				{
					RecordNativeA8VisibilityCull(
						visibilityCull, *payload);
					return;
				}
			}
		}
		NativeA8FallbackReason failure = NativeA8FallbackReason::None;
		if (!payload)
			failure = NativeA8FallbackReason::PacketBuild;
		else if (payload->suppressNextSubmit.exchange(false,
			std::memory_order_acq_rel))
		{
			failure = payload->stickyReason.exchange(
				NativeA8FallbackReason::None, std::memory_order_acq_rel);
			if (failure == NativeA8FallbackReason::None)
				failure = NativeA8FallbackReason::RuntimeFault;
		}
		else if (sortedFrameHit
			&& frameEntry.preflightResult == NativeA8FallbackReason::None
			&& frameEntry.validationToken
			&& frameEntry.generation == payload->preparedGeneration
			&& payload->preflightAtlasTextureEpoch
				== GetNativeA8AtlasTextureEpoch()
			)
		{
			// NativeA8RenderSorted retained the metadata owner and validated this
			// exact payload immediately before the stock sorted Tile traversal.
			failure = NativeA8FallbackReason::None;
		}
		else
			failure = PrepareNativeA8Group(shape, *metadata, *payload);

		if (failure == NativeA8FallbackReason::None)
		{
			NativeA8ShapePayload* const sourcePayload = payload;
			NativePacketDrawResult draw;
			const UInt32 commandSpanIndex =
				sortedFrameHit
					? frameEntry.commandSpanIndex
					: kInvalidNativeA8CommandIndex;
			bool commandHandled = false;
			if (g_bEnableFreeTypeFontCommandBuffer
				&& commandSpanIndex
					!= kInvalidNativeA8CommandIndex)
			{
				NativeA8CommandSpanView commandView;
				if (FindNativeA8CommandSpan(commandSpanIndex,
						frameEntry.validationToken, commandView)
					&& commandView.span
					&& commandView.span->commandCount > 1)
				{
					commandHandled = TryDrawNativeRetainedSpan(
						pass, currentPass, setupDrawmode,
						shape, *sourcePayload,
						commandSpanIndex, draw);
				}
			}
			if (!commandHandled)
			{
				draw = {};
				const bool directShapeHandled = sortedFrameHit
					&& TryDrawNativeSinglePacketDirect(
						pass, currentPass, setupDrawmode,
						shape, *sourcePayload, draw,
						commandSpanIndex,
						frameEntry.singlePacketCommandIndex);
				if (!directShapeHandled)
				{
					draw = DrawNativePacketSet(pass, currentPass,
						setupDrawmode, shape, *sourcePayload,
						kInvalidNativeA8CommandIndex);
				}
			}
			if (!draw.runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedTileRenderPassHit)
				{
					state.loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile group route hit shape=%p font=%u pass=%u packets=%u ranges=%u route=%s",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(
							sourcePayload->packetShaders.size()),
						sourcePayload->payloadTemplate
							? sourcePayload->payloadTemplate->sourceRangeCount
							: 0u,
						draw.directShapeRoute
							? "direct-single-packet-shape"
							: (draw.stockLikeBitmapRoute
								? "stock-like-bitmap-pages"
								: "effect-packets"));
				}
				return;
			}
			InvalidateNativeA8SortedShaderState();
			if (draw.constantStateFault)
			{
				MarkNativeA8GenerationFault(
					sourcePayload->preparedGeneration,
					draw.operation, draw.result);
				gLog.FormattedMessage(
					"tnvse_freetype_native: pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-group",
					draw.operation, static_cast<UInt32>(draw.result),
					draw.mismatchRegister, shape, metadata->fontId,
					sourcePayload->preparedGeneration,
					draw.drewPacket ? 1 : 0);
			}
			if (draw.drewPacket)
			{
				MarkNativeA8RuntimeFault(*metadata, *sourcePayload,
					draw.failure);
				return;
			}
			failure = draw.failure;
		}

		RecordNativeA8Suppression(shape, *metadata, failure, "tile-render-pass");
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
			if (State().tileRenderPassHookInstalled)
			{
				State().tileRenderPassHookInstalled = false;
				InvalidateAllVirtualStockBindings();
			}
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site is not CALL rel32; native route unavailable");
			}
			return false;
		}
		if (State().tileRenderPassHookInstalled)
		{
			State().tileRenderPassHookInstalled = false;
			InvalidateAllVirtualStockBindings();
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator native route was replaced; marked groups will be suppressed");
			}
			return false;
		}
		if (reinterpret_cast<UInt32>(current) != kStockTileRenderPassImmediately)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site already has a non-stock target=%p; leaving it untouched",
					current);
			}
			return false;
		}

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
				"tnvse_freetype_native: installed Tile accumulator native route original=%p stock=1",
				current);
		}
		return true;
	}

	bool IsA8AtlasShape(const NiTriShape* shape)
	{
		return shape && *reinterpret_cast<void* const* const*>(shape)
			== &State().triShapeVtable[1];
	}

	void __fastcall A8RenderImmediate(NiTriShape* shape, void*,
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
					!= kInvalidNativeA8CommandIndex)
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
					!= kInvalidNativeA8CommandIndex)
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

	void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*,
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
					!= kInvalidNativeA8CommandIndex)
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
			State().originalRenderImmediateAlt(shape, renderer);
			context.drew = true;
			if (!context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeA8CommandIndex)
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

	bool InitializeA8TriShapeVtable(NiTriShape* shape)
	{
		void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
		if (!source)
			return false;
		if (source == &State().triShapeVtable[1])
			return true;
		if (State().originalTriShapeVtable)
			return source == State().originalTriShapeVtable;

		A8State& state = State();
		state.originalTriShapeVtable = source;
		const void* expectedFalsePredicate =
			reinterpret_cast<void*>(kNiGeometryFalsePredicate);
		const bool predicateSlotsMatch =
			source[kGeometrySpecialPredicateSlot]
				== expectedFalsePredicate
			&& source[kGeometryAlternatePredicateSlot]
				== expectedFalsePredicate;
		// Verify the reverse-engineered identity and behavior once while the
		// object still owns the stock vtable. The lite hot path then needs only
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
				"tnvse_freetype_native: RenderPassImmediately_Standard-lite predicate-envelope validated=%u special=%p alternate=%p expected=%p segmentState=independent-effective-v3 slot34=vendor-atoc-independent",
				state.standardPassLitePredicatesValidated ? 1u : 0u,
				source[kGeometrySpecialPredicateSlot],
				source[kGeometryAlternatePredicateSlot],
				expectedFalsePredicate);
		}

		state.triShapeVtable[0] = source[-1];
		std::copy(source, source + kCopiedTriShapeVtableEntries,
			state.triShapeVtable.begin() + 1);
		state.originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
			state.triShapeVtable[kRenderImmediateSlot + 1]);
		state.originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
			state.triShapeVtable[kRenderImmediateAltSlot + 1]);
		state.originalDeleteThis = reinterpret_cast<DeleteThisFn>(
			state.triShapeVtable[kDeleteThisSlot + 1]);
		state.triShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&A8DeleteThis);
		state.triShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediate);
		state.triShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediateAlt);
		return state.originalRenderImmediate && state.originalRenderImmediateAlt
			&& state.originalDeleteThis;
	}
}
