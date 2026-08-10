#pragma once

#include "font_native_shape_hooks_detail.h"

namespace fonthook::vectorfont::implementation::font_native_shape_hooks
{
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
			UInt32 streamOffset = 0;
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

	NativeSegmentDeviceStateCache& SegmentDeviceStateCache();

		enum class NativeSegmentConstantsStateRelation : UInt8
		{
			Different = 0,
			Exact,
			TranslationOnly
		};

		struct VisibilityCameraRunTracker
		{
			UInt64 frameToken = 0;
			UInt64 nestedTraversalSerial = 0;
			SInt32 previousSortedItem = -1;
			bool ready = false;
		};

	VisibilityCameraRunTracker& VisibilityCameraRun();
	UInt32& NativeFontDispatchRouteSampleCursor();

		enum class NativeDirectDrawLiteFallback : UInt8
		{
			None = 0,
			Program,
			Renderer,
			Geometry,
			Binding,
			Declaration,
		};

		enum class VanillaLayoutBindingFailure : UInt64
		{
			None = 0,
			TokenState = 1ull << 0,
			PacketVertexCount = 1ull << 1,
			PacketIdentity = 1ull << 2,
			DataVertexCount = 1ull << 3,
			TokenStream = 1ull << 4,
			DeclarationIdentity = 1ull << 5,
			BufferFlags = 1ull << 6,
			GeometryGroup = 1ull << 7,
			Fvf = 1ull << 8,
			SoftwareVertexProcessing = 1ull << 9,
			BufferVertexSnapshot = 1ull << 10,
			BufferVertexPacket = 1ull << 11,
			BufferMaxVertices = 1ull << 12,
			BufferStreamCount = 1ull << 13,
			StrideArray = 1ull << 14,
			StrideIdentity = 1ull << 15,
			StrideValue = 1ull << 16,
			VertexChip = 1ull << 17,
			VertexChipIdentity = 1ull << 18,
			VertexChipIndex = 1ull << 19,
			VertexBuffer = 1ull << 20,
			VertexBufferIdentity = 1ull << 21,
			VertexChipOffset = 1ull << 22,
			VertexChipSize = 1ull << 23,
			VertexChipLock = 1ull << 24,
			VertexRange = 1ull << 25,
			IndexBuffer = 1ull << 26,
			IndexCount = 1ull << 27,
			IndexSize = 1ull << 28,
			BaseVertex = 1ull << 29,
			PrimitiveTopology = 1ull << 30,
			ArrayTopology = 1ull << 31,
			SubmissionWitness = 1ull << 32,
			Unclassified = 1ull << 33,
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
			// Optional synchronous witness used by the metadata-only Vanilla route.
			// Existing frame-command submissions leave it null and retain their
			// established device-failure handling.
			bool* successfulDrawWitness = nullptr;
		};

		class NativeDirectDrawLiteArmScope
		{
		public:
			NativeDirectDrawLiteArmScope(NiTriShape* geometry,
				const NativeDirectDrawLiteSubmission& submission)
			{
				m_context = DirectImmediateContext();
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

		enum class VanillaLayoutStandardLiteFallback : UInt8
		{
			None = 0,
			Envelope,
			Program,
			Renderer,
			Geometry,
			Binding,
			Declaration,
			Prelude,
		};

	bool SameSegmentDeviceStateStamp(
		const NativeFontSegmentDeviceStateStamp& left,
		const NativeFontSegmentDeviceStateStamp& right);
	NativeSegmentDeviceStateCache* EnterSegmentDeviceStateCache(
		const NativeFontSegmentDeviceStateStamp* stamp);
	void InvalidateSegmentDeviceStateCache();
	bool SameSegmentPassState(const NativeSegmentPassStateKey& left,
		const NativeSegmentPassStateKey& right);
	bool BuildSegmentPassStateKey(NiTriShape* geometry,
		const NativeFontCompiledPacketCommand* program,
		const void* atlasTexture, NativeSegmentPassStateKey& key);
	bool BuildSegmentConstantsStateKey(NiTriShape* geometry,
		NiDX9Renderer* renderer,
		const NativeFontCompiledPacketCommand* program,
		NativeSegmentConstantsStateKey& key, bool& cleanupRequired);
	NativeSegmentConstantsStateRelation CompareSegmentConstantsState(
		const NativeSegmentConstantsStateKey& left,
		const NativeSegmentConstantsStateKey& right);
	bool BuildSegmentBlendStateKey(NiTriShape* geometry,
		NativeFontStandardBlendSemantics semantics,
		NativeSegmentBlendStateKey& key);
	bool SameSegmentBlendState(const NativeSegmentBlendStateKey& left,
		const NativeSegmentBlendStateKey& right);
	bool BuildSegmentAlphaTestStateKey(NiTriShape* geometry,
		NativeSegmentAlphaTestStateKey& key);
	bool SameSegmentAlphaTestState(
		const NativeSegmentAlphaTestStateKey& left,
		const NativeSegmentAlphaTestStateKey& right);
	bool BuildSegmentRenderStatesKey(NiTriShape* geometry,
		UInt32 currentPass, bool firstPass,
		NativeSegmentRenderStatesKey& key);
	bool SameSegmentRenderStates(const NativeSegmentRenderStatesKey& left,
		const NativeSegmentRenderStatesKey& right);
	bool SameSegmentGeometryBinding(
		const NativeSegmentGeometryBindingKey& left,
		const NativeSegmentGeometryBindingKey& right);
	void BreakVisibilityCameraRun();
	bool CanReuseVisibilityCameraRun(
		const NativeFontSortedFrameEntryView& frameEntry,
		const NativeFontVisibilityPreflight& visibility);
	void ContinueVisibilityCameraRun(
		const NativeFontSortedFrameEntryView& frameEntry,
		const NativeFontVisibilityPreflight& visibility);
	bool ShouldSampleNativeFontDispatchRoute();
	bool BuildSegmentDeviceStateStamp(const NativeFontFrameStamp* frame,
		UInt32 executionSegmentEpoch, UInt32 externalMutationEpoch,
		NativeFontSegmentDeviceStateStamp& stamp);

	void AddVanillaLayoutBindingFailure(UInt64& failures,
		VanillaLayoutBindingFailure failure);
	void RecordNativeDirectDrawLiteFallback(
		NativeDirectDrawLiteFallback failure);
	NativeDirectDrawLiteFallback BuildNativeDirectDrawLiteSubmission(
		NiTriShape* geometry, NiDX9Renderer* renderer,
		const NiPropertyState* properties,
		const NativeFontCompiledPacketCommand& program,
		const NativeFontDrawCommand& command,
		NiGeometryBufferData* buffer,
		NativeSegmentDeviceStateCache* deviceState,
		NativeDirectDrawLiteSubmission& submission);
	NativeDirectDrawLiteFallback BuildVanillaLayoutDirectDrawLiteSubmission(
		NiTriShape* geometry, NiDX9Renderer* renderer,
		const NativeFontCompiledPacketCommand& program,
		const NativeFontPacketTemplate& packet,
		const NativeFontVanillaLayoutDrawToken& drawToken,
		NativeDirectDrawLiteSubmission& submission,
		UInt64& bindingFailures);
	void ExecuteNativeDirectDrawLite(
		const NativeDirectDrawLiteSubmission& submission);
	bool TryDrawVanillaLayoutStandardPassLite(
		BSShaderProperty::RenderPass* pass, UInt32 currentPass,
		bool testAlpha, bool blendAlpha, bool setupRenderStates,
		NiTriShape* geometry, TileShader* shader,
		const NativeFontShapePayload& payload,
		const NativeFontPayloadTemplate& artifact,
		const NativeFontPacketTemplate& packet,
		const NativeFontVanillaLayoutDrawToken& drawToken);
}
