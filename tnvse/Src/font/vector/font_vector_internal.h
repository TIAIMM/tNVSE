#pragma once

#include "font_render_route.h"
#include "font_vector.h"
#include "font_vector_msdfgen.h"
#include "load_config.h"

#include <array>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BSShader;
class BSShaderProperty;
class TileShader;
struct IDirect3DDevice9;
class NiNode;
class NiTriShape;

namespace fonthook::vectorfont
{
	inline DistanceFieldMethod GetConfiguredDistanceFieldMethod()
	{
		return g_uiFreeTypeFontDistanceFieldMode == kFreeTypeFontMtsdfMode
			? DistanceFieldMethod::Mtsdf : DistanceFieldMethod::TrueSdf;
	}

	inline bool UsesBakedEffectRoute()
	{
		return g_uiFreeTypeFontDistanceFieldMode
			== kFreeTypeFontBakedEffectMode;
	}

	inline bool UsesMtsdfDistanceField()
	{
		return GetConfiguredDistanceFieldMethod() == DistanceFieldMethod::Mtsdf;
	}

	inline bool IsVanillaLayoutEnabled(DistanceFieldMethod method)
	{
		return !UsesBakedEffectRoute()
			&& g_bEnableFreeTypeFontVanillaLayout
			&& (method == DistanceFieldMethod::TrueSdf
				|| method == DistanceFieldMethod::Mtsdf);
	}

	inline const char* GetConfiguredDistanceFieldMethodName()
	{
		return UsesMtsdfDistanceField() ? "MTSDF" : "true SDF";
	}

	inline const char* GetConfiguredFontRenderModeName()
	{
		return UsesBakedEffectRoute()
			? "baked vanilla-like" : GetConfiguredDistanceFieldMethodName();
	}

	// Base Fill/distance-field generator revision. Route-specific pixel changes
	// may use a narrower revision below to preserve unrelated caches.
	constexpr UInt32 kGlyphMaskGeneratorVersion = 14;
	// CPU coverage revision 3 rasterizes Fill and every derived CPU effect from
	// the same unhinted scalable outline used by the distance-field route.
	// Earlier revisions started from grid-fitted coverage, which made small
	// aggressive/fallback text and all effects derived from it visibly heavier
	// than the SDF result. This revision is included in every CPU-effect
	// bitmap/cache identity.
	constexpr UInt32 kCpuEffectCoverageVersion = 3;
	// Version 12 removes the unused 16-band glyph-collision profile from every
	// manifest record. Version 11 added the final direct cached-letter/composite
	// profile contract.
	// Atlas snapshot identity also consumes this ABI because a restored atlas is
	// usable only with its matching complete manifest.
	constexpr UInt32 kPersistentGlyphManifestVersion = 12;
	// Version 3 separates the final BGRA-composite profile from the former
	// aggressive coverage representation while retaining cache-domain routing.
	constexpr UInt8 kPersistentGlyphManifestCacheIdentityVersion = 3;
	// Prewarm creates worker-local FreeType faces and distance/effect scratch
	// storage. Keep this shared cap in the scheduler and its memory model so a
	// larger host CPU cannot silently increase the x86 address-space peak.
	// Bound worker-local FreeType/MSDF scratch while allowing wider CPUs to use
	// more of their available cores. ResolvePrewarmWorkerCount still leaves one
	// logical processor free, and ResolveMemoryBoundedWorkerLimit may select fewer
	// workers without relaxing the 24 MiB batch ceiling.
	constexpr UInt32 kMaximumPrewarmRasterWorkers = 16;
	constexpr UInt32 kExpensivePrewarmParallelThreshold = 8;
	constexpr UInt32 kFillPrewarmParallelThreshold = 64;
	constexpr UInt32 kFillPrewarmWorkChunk = 8;

	struct NativeFontPayloadTemplate;
	enum class NativeFontVanillaLayoutKind : UInt8
	{
		None = 0,
		Uniform40,
		Parametric48,
	};

	inline bool UsesNativeFontVanillaLayout(
		NativeFontVanillaLayoutKind layoutKind)
	{
		return layoutKind != NativeFontVanillaLayoutKind::None;
	}

	// The thread-local text-artifact front is set-associative so menu churn
	// retains recent weak references without turning lookup into a linear scan.
	// Admission history is larger because one-shot strings must not evict a
	// warmed signature merely because both hashes share a direct-mapped slot.
	inline constexpr UInt32 kTextArtifactHotBucketCount = 64;
	inline constexpr UInt32 kTextArtifactHotWays = 4;
	inline constexpr UInt32 kTextArtifactAdmissionBucketCount = 256;
	inline constexpr UInt32 kTextArtifactAdmissionWays = 4;

	enum class FreeTypePerfCounter : UInt16
	{
		BitmapMemoryHit,
		BitmapCrossFontHit,
		BitmapDiskHit,
		BitmapDiskMiss,
		BitmapDiskWrite,
		BitmapDiskReadBytes,
		BitmapDiskWriteBytes,
		BitmapRasterized,
		BitmapBatchRequest,
		BitmapBatchDedupe,
		AtlasHit,
		AtlasCreated,
		AtlasGrown,
		AtlasUpload,
		AtlasUploadBytes,
		AtlasUploadRect,
		TextArtifactHit,
		TextArtifactMiss,
		TextArtifactHotHit,
		TextArtifactHotEntryExpired,
		TextArtifactHotEntryReplacement,
		TextArtifactAdmissionHistoryHit,
		TextArtifactAdmissionCandidateReplacement,
		TextArtifactAdmissionEstablishedReplacement,
		TextArtifactAdmissionBypass,
		TextArtifactAdmission,
		TextArtifactEviction,
		TextArtifactCompiledVertex,
		TextArtifactVertexInitializationBytesAvoided,
		TextArtifactCompositeProfileVertex,
		TextArtifactCompositeProfileVertexScanSaved,
		ShaderEffectBatch,
		CpuEffectMasksAvoided,
		GpuResidentGlyphHit,
		GpuResidentGlyphMiss,
		AtlasSnapshotProfileReuse,
		DynamicVertexUpload,
		DynamicVertexUploadBytes,
		DynamicVertexReuse,
		DynamicVertexDiscard,
		StaticVertexUpload,
		StaticVertexUploadBytes,
		StaticVertexHit,
		StaticVertexPromotionFailed,
		SortedStaticBatch,
		SortedStaticPayload,
		SortedStaticBytes,
		MergedPacketRange,
		MetadataHotHit,
		MetadataLockedLookup,
		SortedFrameFacade,
		SortedFramePayload,
		SortedFrameLookupHit,
		PreflightFastHit,
		PreflightFullValidation,
		DirectStaticResidencyHit,
		DirectDynamicResidencyHit,
		SortedDynamicBatch,
		SortedDynamicPayload,
		SortedDynamicBytes,
		LocklessPacketPrepare,
		VisibilityCheck,
		VisibilityCulled,
		VisibilityAppCulled,
		VisibilityZeroAlpha,
		VisibilityClip,
		VisibilityScissor,
		VisibilityPreflightSkipped,
		VisibilityPacketsSaved,
		VisibilityVerticesSaved,
		VisibilityPreflightClipCheck,
		VisibilityPreflightClipCulled,
		VisibilityPreflightClipViewport,
		VisibilityPreflightClipScissor,
		VisibilityPreflightClipFailOpen,
		VisibilityPreflightClipHonored,
		VisibilityPreflightClipRevoked,
		VisibilityPreflightClipRevokeInvalid,
		VisibilityPreflightClipRevokeFrame,
		VisibilityPreflightClipRevokeCamera,
		VisibilityPreflightClipRevokeGeometry,
		VisibilityPreflightClipRevokeTransform,
		VisibilityPreflightClipRevokeBound,
		VisibilityPreflightClipRevokeScissor,
		VisibilityPreflightClipRevokeProof,
		VisibilityPreflightClipTransformHit,
		VisibilityPreflightClipTransformMiss,
		VisibilityPreflightClipTransformIdentityMiss,
		VisibilityPreflightClipTransformKeyMiss,
		VisibilityPreflightClipTransformUnavailable,
		VisibilityPreflightClipVanillaUiOrthographicTranslation,
		VisibilityPreflightClipGenericTransform,
		SinglePacketDirectCandidate,
		SinglePacketDirectDraw,
		SinglePacketDirectVertex,
		SinglePacketDirectSyntheticBuffer,
		SinglePacketDirectFallback,
		SinglePacketDirectFallbackCommand,
		SinglePacketDirectFallbackSubmission,
		SinglePacketDirectFallbackBindingInput,
		SinglePacketDirectFallbackBindingTopology,
		SinglePacketDirectFallbackBindingAtlas,
		SinglePacketDirectFallbackBindingFacade,
		SinglePacketDirectFallbackFacadeModelData,
		SinglePacketDirectFallbackFacadeAlphaProperty,
		SinglePacketDirectFallbackFacadeBufferData,
		SinglePacketDirectFallbackFacadeTileProperty,
		SinglePacketDirectFallbackFacadeStreamCount,
		SinglePacketDirectFallbackFacadeVertexStride,
		SinglePacketDirectFallbackFacadeVertexChipArray,
		SinglePacketDirectFallbackFacadeVertexChip,
		SinglePacketDirectFallbackBindingProperty,
		SinglePacketDirectFallbackBindingTexture,
		SinglePacketDirectFallbackBindingShader,
		SinglePacketDirectFallbackRuntime,
		SingletonFacadeCandidate,
		SingletonFacadeCreated,
		SingletonFacadePayloadPacket,
		SingletonFacadeSinglePacketArtifact,
		SingletonFacadeMultiPacketArtifact,
		SingletonFacadeDirectFrame,
		SingletonFacadeSpanFrame,
		SingletonFacadePacketLoopFrame,
		SingletonFacadeTopologySwitch,
		SingletonFacadeFallback,
		SingletonFacadePartialFault,
		SingletonFacadeStaticHit,
		SingletonFacadeDynamicHit,
		SingletonFacadeRebind,
		SingletonFacadeRevoke,
		SingletonFacadeSortedPreflightSaved,
		SingletonFacadeProxyPacketSaved,
		ConstantOwnershipSegment,
		ConstantOwnershipReuse,
		ConstantOwnershipRelease,
		VanillaConstantUpdate,
		SegmentDevicePostSet,
		CompositeConstantFullUpload,
		NativePacketConstantReuse,
		CompositeConstantPartialUpload,
		VanillaPixelConstantCompatibilityRepublish,
		NativePacketConstantRegisterUpload,
		NativePacketConstantFullTailElided,
		NativePrivateStateForeignRenderPassInvalidation,
		NativePrivateStateVanillaLayoutPreserve,
		SamplerStateSet,
		SamplerStateReuse,
		StandardPassV2Replay,
		StandardPassV2CompatibilityReplay,
		SegmentDeviceConstantsSet,
		SegmentDeviceConstantsReuse,
		ConstantSnapshotGetElided,
		ConstantRestoreSetElided,
		VertexAaConstantSet,
		VertexAaConstantReuse,
		VertexAaConstantVanillaPreserved,
		CommandProgramSetup,
		CommandProgramBindElided,
		CommandTextureBindSet,
		CommandTextureBindReuse,
		CommandPacketConstantFullUpload,
		CommandPacketConstantPartialUpload,
		CommandPacketConstantReuse,
		CommandPacketConstantRegisterUpload,
		CommandPacketConstantFullTailElided,
		CompositeFusedEligible,
		CompositeOrderedEligible,
		CompositeOverlapFallback,
		CompositeMultiPageFallback,
		CompositeShaderFallback,
		CompositeDraw,
		TilePass,
		CompositeCacheHit,
		CompositeCacheMiss,
		CompositeCacheStateChange,
		CompositeCacheGenerated,
		CompositeCacheEvicted,
		CompositeCacheBytes,
		CompositeCacheBudgetReject,
		CompositeCacheRttFailure,
		CompositeCacheRestoreFailure,
		CompositeVisualValidated,
		CompositeVisualRejected,
		CompositeVisualInconclusive,
		CommandRecorded,
		CommandSpanRecorded,
		CommandPacketRecorded,
		CommandSinglePacketRecorded,
		CommandSinglePacketBuildFallback,
		CommandSinglePacketHit,
		CommandSinglePacketMiss,
		CommandSinglePacketReplay,
		CommandSinglePacketFallback,
		CommandDirectFacadeSinglePacketRecorded,
		CommandDirectFacadeSinglePacketHit,
		CommandDirectFacadeSinglePacketMiss,
		CommandDirectFacadeSinglePacketReplay,
		CommandDirectFacadeSinglePacketFallback,
		CommandBuildViewHit,
		CommandBuildViewMiss,
		CommandBuildBindingReuse,
		CommandBuildVectorGrowth,
		CommandDeferredRenderTargetCapture,
		CommandTileRetainedBuild,
		CommandTileRetainedRefresh,
		CommandTileRetainedHit,
		CommandTileRetainedMiss,
		CommandTileRetainedPacketReuse,
		CommandSpanHit,
		CommandSpanMiss,
		CommandRetainedBridgeDraw,
		CommandNativeReplay,
		StandardPassLiteCandidate,
		StandardPassLiteRetainedBuild,
		StandardPassLiteRetainedReuse,
		StandardPassLiteRetainedHit,
		StandardPassLiteRetainedMiss,
		StandardPassLiteStage1Eligible,
		StandardPassLiteStage2Resident,
		StandardPassLiteStage3Replay,
		StandardPassLiteVanillaFallback,
		StandardPassLiteFallbackEnvelope,
		StandardPassLiteFallbackProgram,
		StandardPassLiteFallbackRenderer,
		StandardPassLiteFallbackGeometry,
		StandardPassLiteFallbackBinding,
		StandardPassLiteFallbackPrelude,
		SegmentDeviceStateStart,
		SegmentDeviceStateReuse,
		SegmentDevicePassSet,
		SegmentDevicePassReuse,
		SegmentDeviceBlendSet,
		SegmentDeviceBlendReuse,
		SegmentDeviceAlphaTestSet,
		SegmentDeviceAlphaTestReuse,
		SegmentDeviceRenderStatesSet,
		SegmentDeviceRenderStatesReuse,
		CommandVanillaBootstrapSaved,
		CommandDirectRangeReplay,
		SegmentDevicePostElision,
		CommandPacketLightValidation,
		CommandPacketEpochGuard,
		CommandPacketStateValidationElided,
		CommandRenderTargetValidation,
		CommandExecutionSegment,
		CommandSegmentFullValidation,
		CommandSegmentValidationReuse,
		CommandSegmentInvalidation,
		CommandRetainedProgramHit,
		CommandRetainedProgramMiss,
		CommandFallbackToken,
		CommandFallbackGeneration,
		CommandFallbackAtlas,
		CommandFallbackResource,
		CommandFallbackTopology,
		CommandFallbackHook,
		CommandFallbackNested,
		CommandFallbackRenderTarget,
		CommandFallbackState,
		StaticPromotionDeferredLifecycle,
		StaticPromotionDeferredUploadHistory,
		StaticPromotionDeferredBudget,
		StaticPromotionDeferredRetry,
		StaticResidentColdEviction,
		StaticResidentColdEvictionBytes,
		NativeTileConstantsLiteReplay,
		NativeTileConstantsLiteFallback,
		NativeTileConstantsLiteScaledScissorFallback,
		SegmentDeviceConstantsFirstMismatchProgram,
		SegmentDeviceConstantsFirstMismatchRotates,
		SegmentDeviceConstantsFirstMismatchWorld,
		SegmentDeviceConstantsFirstMismatchView,
		SegmentDeviceConstantsFirstMismatchProjection,
		SegmentDeviceConstantsFirstMismatchViewProjection,
		SegmentDeviceConstantsFirstMismatchCameraRight,
		SegmentDeviceConstantsFirstMismatchCameraUp,
		SegmentDeviceConstantsFirstMismatchOverlayColor,
		SegmentDeviceConstantsFirstMismatchTextureTransform,
		SegmentDeviceConstantsFirstMismatchTileAlpha,
		SegmentDeviceConstantsFirstMismatchMaterialAlpha,
		SegmentDeviceConstantsFirstMismatchNearDepth,
		SegmentDeviceConstantsFirstMismatchDepthRange,
		SegmentDeviceConstantsWorldMismatchRotationOnly,
		SegmentDeviceConstantsWorldMismatchTranslationOnly,
		SegmentDeviceConstantsWorldMismatchScaleOnly,
		SegmentDeviceConstantsWorldMismatchRotationTranslation,
		SegmentDeviceConstantsWorldMismatchRotationScale,
		SegmentDeviceConstantsWorldMismatchTranslationScale,
		SegmentDeviceConstantsWorldMismatchRotationTranslationScale,
		NativeTileConstantsTranslationLiteReplay,
		NativeTileConstantsTranslationLiteTransientReplay,
		NativeTileConstantsTranslationLiteFallback,
		NativeTileConstantsTranslationLiteNotApplicableFallback,
		NativeTileConstantsTranslationLiteScaledScissorFallback,
		NativeTileConstantsTranslationLiteNonFiniteFallback,
		NativeTileConstantsTranslationLiteDeviceFailure,
		NativeDirectDrawLiteCandidate,
		NativeDirectDrawLiteReplay,
		NativeDirectDrawLiteFallback,
		NativeDirectDrawLiteFallbackProgram,
		NativeDirectDrawLiteFallbackRenderer,
		NativeDirectDrawLiteFallbackGeometry,
		NativeDirectDrawLiteFallbackBinding,
		NativeDirectDrawLiteFallbackDeclaration,
		NativeDirectDrawLiteBindingSet,
		NativeDirectDrawLiteBindingReuse,
		NativeDirectDrawLiteBindingDeviceFailure,
		NativeDirectDrawLiteDrawDeviceFailure,
		NativeRegistrationArtifactSealed,
		NativeRegistrationArtifactFallback,
		NativeRegistrationHookFast,
		NativeRegistrationHookSlow,
		NativeRegistrationProxyFast,
		NativeRegistrationProxySlow,
		ThinRegistrationCall,
		ThinRegistrationFastForward,
		ThinRegistrationHookMismatch,
		ThinRegistrationSlowAudit,
		ThinRegistrationSuppressed,
		ThinRegistrationTimingSample,
		ThinRegistrationMetadataBatch,
		ThinRegistrationMetadataShape,
		ThinRegistrationMetadataMissing,
		ThinRegistrationSortedScanFallback,
		ThinRegistrationFacadeTopology,
		ThinRegistrationFacadeFallback,
		ThinRegistrationOccurrenceFallback,
		StructuralReadinessRawHit,
		StructuralReadinessFullAudit,
		StructuralReadinessHookMismatch,
		StructuralReadinessRendererMismatch,
		StructuralReadinessVirtualQueryAvoided,
		StructuralReadinessTileCallbackMismatch,
		StructuralReadinessRenderAlphaMismatch,
		StructuralReadinessImmediateMismatch,
		StructuralReadinessAtlasMismatch,
		SingletonFacadeInlinePayload,
		SingletonFacadeHeapPayload,
		SingletonFacadeChildAllocationAvoided,
		MetadataMapReserve,
		MetadataMapRehash,
		SortedAllStaticFastExit,
		SortedAllStaticPayloadValidationElided,
		AccumulatorEmptyFastPath,
		AccumulatorMetadataCullSkipped,
		AccumulatorNoPreparedPayload,
		VanillaLayoutEligible,
		VanillaLayoutCreated,
		VanillaLayoutFallback,
		VanillaLayoutDraw,
		VanillaLayoutCull,
		VanillaLayoutRuntimeFallback,
		VanillaLayoutVertex,
		VanillaLayoutShiftedEligible,
		VanillaLayoutShiftedCreated,
		VanillaLayoutShiftedDraw,
		VanillaLayoutShiftedRuntimeFallback,
		VanillaLayoutPrecacheAccepted,
		VanillaLayoutPrecacheUnavailable,
		VanillaLayoutPayloadUploadAttempt,
		VanillaLayoutPayloadUploadSuccess,
		VanillaLayoutPayloadUploadFailure,
		VanillaLayoutPayloadUploadBytes,
		VanillaLayoutNativePackPending,
		VanillaLayoutPriorGenerationDeclarationUse,
		VanillaLayoutPrivateStateCarry,
		VanillaLayoutPrivateStateCarryRejected,
		PreparedSidecarCaptureFallback,
		PreparedSidecarRejectedFallback,
		VanillaLayoutDrawTokenHit,
		VanillaLayoutDrawTokenSlowPath,
		VanillaLayoutDrawTokenUncertified,
		VanillaLayoutDrawTokenShapeShaderMismatch,
		VanillaLayoutDrawTokenGenerationMismatch,
		VanillaLayoutDrawTokenGeometryMismatch,
		VanillaLayoutDrawTokenNativePackMismatch,
		VanillaLayoutDrawTokenLayoutMismatch,
		VanillaLayoutDrawTokenFirstCertification,
		VanillaLayoutDrawTokenRecertification,
		VanillaLayoutDrawTokenRejected,
		VanillaLayoutStandardLiteCandidate,
		VanillaLayoutStandardLiteReplay,
		VanillaLayoutStandardLiteFallback,
		VanillaLayoutStandardLiteFallbackEnvelope,
		VanillaLayoutStandardLiteFallbackProgram,
		VanillaLayoutStandardLiteFallbackRenderer,
		VanillaLayoutStandardLiteFallbackGeometry,
		VanillaLayoutStandardLiteFallbackBinding,
		VanillaLayoutStandardLiteBindingTokenState,
		VanillaLayoutStandardLiteBindingPacketVertexCount,
		VanillaLayoutStandardLiteBindingPacketIdentity,
		VanillaLayoutStandardLiteBindingDataVertexCount,
		VanillaLayoutStandardLiteBindingTokenStream,
		VanillaLayoutStandardLiteBindingDeclarationIdentity,
		VanillaLayoutStandardLiteBindingBufferFlags,
		VanillaLayoutStandardLiteBindingGeometryGroup,
		VanillaLayoutStandardLiteBindingFvf,
		VanillaLayoutStandardLiteBindingSoftwareVertexProcessing,
		VanillaLayoutStandardLiteBindingBufferVertexSnapshot,
		VanillaLayoutStandardLiteBindingBufferVertexPacket,
		VanillaLayoutStandardLiteBindingBufferMaxVertices,
		VanillaLayoutStandardLiteBindingBufferStreamCount,
		VanillaLayoutStandardLiteBindingStrideArray,
		VanillaLayoutStandardLiteBindingStrideIdentity,
		VanillaLayoutStandardLiteBindingStrideValue,
		VanillaLayoutStandardLiteBindingVertexChip,
		VanillaLayoutStandardLiteBindingVertexChipIdentity,
		VanillaLayoutStandardLiteBindingVertexChipIndex,
		VanillaLayoutStandardLiteBindingVertexBuffer,
		VanillaLayoutStandardLiteBindingVertexBufferIdentity,
		VanillaLayoutStandardLiteBindingVertexChipOffset,
		VanillaLayoutStandardLiteBindingVertexChipSize,
		VanillaLayoutStandardLiteBindingVertexChipLock,
		VanillaLayoutStandardLiteBindingVertexRange,
		VanillaLayoutStandardLiteBindingIndexBuffer,
		VanillaLayoutStandardLiteBindingIndexCount,
		VanillaLayoutStandardLiteBindingIndexSize,
		VanillaLayoutStandardLiteBindingBaseVertex,
		VanillaLayoutStandardLiteBindingPrimitiveTopology,
		VanillaLayoutStandardLiteBindingArrayTopology,
		VanillaLayoutStandardLiteBindingSubmissionWitness,
		VanillaLayoutStandardLiteBindingUnclassified,
		VanillaLayoutStandardLiteFallbackDeclaration,
		VanillaLayoutStandardLiteFallbackPrelude,
		VanillaLayoutStandardLiteCurrentDeclarationReplay,
		VanillaLayoutStandardLiteCompatibleDeclarationReplay,
		VanillaLayoutStandardLiteBindingAdjacentPair,
		VanillaLayoutStandardLiteBindingAdjacentExact,
		VanillaLayoutStandardLiteBindingAdjacentSameDeclaration,
		VanillaLayoutStandardLiteBindingAdjacentSameVertexBuffer,
		VanillaLayoutStandardLiteBindingAdjacentSameIndexBuffer,
		VanillaLayoutStandardLiteBindingAdjacentSameStreamOffset,
		VanillaLayoutStandardLiteBindingAdjacentSameStride,
		VanillaLayoutStandardLiteBindingRun,
		VanillaLayoutStandardLiteBindingRunDraw,
		VanillaLayoutStandardLiteBindingRunLength1,
		VanillaLayoutStandardLiteBindingRunLength2,
		VanillaLayoutStandardLiteBindingRunLength3To4,
		VanillaLayoutStandardLiteBindingRunLength5To8,
		VanillaLayoutStandardLiteBindingRunLength9To16,
		VanillaLayoutStandardLiteBindingRunLength17To32,
		VanillaLayoutStandardLiteBindingRunLength33Plus,
		PerfCounterBatchScope,
		PerfCounterBatchRecord,
		PerfCounterBatchAtomicFlush,
		PerfCounterBatchAtomicSaved,
		Count,
	};
	static_assert(
		static_cast<UInt32>(FreeTypePerfCounter::Count) <= 0xFFFFu,
		"FreeTypePerfCounter no longer fits its UInt16 storage");

	void RecordFreeTypePerf(FreeTypePerfCounter aeCounter, UInt64 auiAmount = 1);
	void ReportFreeTypePerf();

	// A render traversal may report the same small set of counters thousands of
	// times. On Win32, a 64-bit atomic add is substantially more expensive than a
	// plain TLS increment. This scope retains exact totals while coalescing every
	// touched counter into one atomic publication at the traversal boundary.
	class FreeTypePerfCounterBatchScope
	{
	public:
		explicit FreeTypePerfCounterBatchScope(bool abEnabled = true);
		~FreeTypePerfCounterBatchScope();

		FreeTypePerfCounterBatchScope(
			const FreeTypePerfCounterBatchScope&) = delete;
		FreeTypePerfCounterBatchScope& operator=(
			const FreeTypePerfCounterBatchScope&) = delete;

	private:
		bool m_active = false;
	};

	enum class FreeTypePerfPhase : UInt8
	{
		Layout = 0,
		Sidecar,
		DirectCompile,
		TextArtifactCompile,
		NativeRegistration,
		NativeRegistrationReadiness,
		NativeRegistrationShape,
		NativeRegistrationBudget,
		NativeRegistrationAllocation,
		NativeRegistrationPayload,
		NativeRegistrationAccounting,
		NativeRegistrationPublish,
		Preflight,
		Submit,
		CommandBuild,
		CommandBuildStamp,
		CommandBuildDirectFacade,
		CommandBuildOrdinary,
		CommandBuildFinalize,
		CommandSubmit,
		ExtendedFntGeometry,
		FrameRouteTotal,
		FrameRoutePrep,
		FramePrepReset,
		FramePrepTopology,
		FramePrepVisibility,
		FramePrepMetadata,
		FramePrepFacades,
		FramePrepReadiness,
		FramePrepLookup,
		FramePrepFacadeLoop,
		FramePrepRing,
		FramePrepRingInputScan,
		FramePrepRingResource,
		FramePrepRingStaticScan,
		FramePrepRingStaticLock,
		FramePrepRingStaticCopy,
		FramePrepRingStaticUnlock,
		FramePrepRingStaticCommit,
		FramePrepRingDynamicResolve,
		FramePrepRingDynamicLock,
		FramePrepRingDynamicCopy,
		FramePrepRingDynamicUnlock,
		FramePrepRingDynamicCommit,
		FramePrepRingLeasePublish,
		FramePrepSingletons,
		FramePrepPublish,
		FrameRouteVanillaRender,
		RegisterRoute,
		DispatchRoute,
		PreflightClipHonorGate,
		VanillaLayoutStandardLiteState,
		VanillaLayoutStandardLiteBinding,
		VanillaLayoutStandardLiteDraw,
		VanillaLayoutStandardLitePost,
		Count,
	};
	inline constexpr UInt32 kVanillaLayoutStandardLiteCpuSampleRate = 256u;
	inline constexpr UInt32 kNativeFontDispatchRouteCpuSampleRate = 256u;
	inline constexpr UInt32 kNativeFontVisibilityHonorCpuSampleRate = 256u;
	struct FreeTypeAccumulatorPrepTailSample
	{
		SInt64 totalTicks = 0;
		SInt64 resetTicks = 0;
		SInt64 topologyTicks = 0;
		SInt64 visibilityTicks = 0;
		SInt64 metadataTicks = 0;
		SInt64 readinessTicks = 0;
		SInt64 lookupTicks = 0;
		SInt64 facadeLoopTicks = 0;
		SInt64 ringTicks = 0;
		SInt64 singletonTicks = 0;
		SInt64 commandTicks = 0;
		SInt64 publishTicks = 0;
		UInt32 itemCount = 0;
		UInt32 facadeCount = 0;
		UInt32 survivorCount = 0;
		UInt32 payloadCount = 0;
		UInt32 singletonCount = 0;
		bool commandFrameActive = false;
	};
	SInt64 BeginFreeTypePerfSample();
	SInt64 EndFreeTypePerfSample(FreeTypePerfPhase aePhase, SInt64 aiStart);
	void RecordFreeTypeAccumulatorPrepTailSample(
		const FreeTypeAccumulatorPrepTailSample& arSample);
	void ResetFreeTypeGpuTiming();

	struct FreeTypeGpuEnvelopeViewport
	{
		UInt32 x = 0;
		UInt32 y = 0;
		UInt32 width = 0;
		UInt32 height = 0;
	};

	enum class FreeTypeGpuEnvelopeCull : UInt8
	{
		App = 0,
		Alpha,
		Clip,
		Scissor,
	};

	// These recorders update only the currently sampled asynchronous query slot.
	// Unsampled frames and non-render threads are no-ops.
	void RecordFreeTypeGpuEnvelopeForeignPass();
	void RecordFreeTypeGpuEnvelopeNativeFacadePass();
	void RecordFreeTypeGpuEnvelopeVanillaPass();
	void RecordFreeTypeGpuEnvelopeVanillaCull(
		FreeTypeGpuEnvelopeCull aeCull);
	void RecordFreeTypeGpuEnvelopeVanillaRuntimeFallback();
	void RecordFreeTypeGpuEnvelopeVanillaDraw(
		bool abStandardLite, UInt32 auiVertexCount,
		UInt32 auiTriangleCount, bool abUseScissor,
		SInt32 aiScissorLeft, SInt32 aiScissorTop,
		SInt32 aiScissorRight, SInt32 aiScissorBottom);

	// Measures the GPU command-stream interval occupied by the complete Tile
	// alpha traversal when that traversal contains prepared direct payloads or
	// preflight-surviving Vanilla-layout work. Results are collected
	// asynchronously; this scope never waits for the GPU.
	class FreeTypeGpuAlphaEnvelopeScope
	{
	public:
		explicit FreeTypeGpuAlphaEnvelopeScope(
			IDirect3DDevice9* apDevice, bool abHasPreparedPayloads,
			bool abHasVanillaLayout,
			const FreeTypeGpuEnvelopeViewport& arViewport);
		~FreeTypeGpuAlphaEnvelopeScope();

		FreeTypeGpuAlphaEnvelopeScope(
			const FreeTypeGpuAlphaEnvelopeScope&) = delete;
		FreeTypeGpuAlphaEnvelopeScope& operator=(
			const FreeTypeGpuAlphaEnvelopeScope&) = delete;

	private:
		bool m_active = false;
	};

	class FreeTypePerfScope
	{
	public:
		explicit FreeTypePerfScope(
			FreeTypePerfPhase aePhase, bool abEnabled = true,
			SInt64* apElapsedTicks = nullptr);
		~FreeTypePerfScope();
		void Stop();

		FreeTypePerfScope(const FreeTypePerfScope&) = delete;
		FreeTypePerfScope& operator=(const FreeTypePerfScope&) = delete;

	private:
		FreeTypePerfPhase m_phase;
		SInt64 m_start = 0;
		SInt64* m_elapsedTicks = nullptr;
		bool m_active = false;
	};
	struct FaceConfig
	{
		// Separator-normalized XML value used for portable configuration hashes.
		std::wstring configuredPath;
		// Resolved filesystem path used only to open the font.
		std::wstring path;
		long faceIndex = 0;
	};

	struct ByteStyle
	{
		float pixelSize = 0.0f;
		float tracking = 0.0f;
		float scaleX = 1.0f;
		float scaleY = 1.0f;
		float embolden = 0.0f;
		float slantDegrees = 0.0f;
		float baselineOffset = 0.0f;
		float fixedWidth = 0.0f;
		std::vector<FaceConfig> faces;
	};

	enum class EffectColorMode : UInt8
	{
		Fixed = 0,
		Fill = 1,
	};

	struct EffectStyle
	{
		bool enabled = false;
		bool includeGlow = false;
		bool includeOutline = false;
		float width = 0.0f;
		float blur = 0.0f;
		float inner = 0.0f;
		float outer = 0.0f;
		float power = 2.0f;
		float softness = 0.5f;
		float x = 0.0f;
		float y = 0.0f;
		NiColorA color = { 0.0f, 0.0f, 0.0f, 1.0f };
		EffectColorMode colorMode = EffectColorMode::Fixed;
	};

	struct FontColorStyle
	{
		bool configured = false;
		NiColorA color = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	enum class EffectQuality : UInt8
	{
		Fast = 0,
		Balanced = 1,
		High = 2,
	};

	// Coverage revision 3 means every Windows-decodable unit in the active code
	// page. The former codepage mode used value 2 and covered only DCFGCF ranges,
	// so it must not satisfy the new completion contract. Zero remains the
	// in-progress manifest state.
	inline constexpr UInt8 kCompleteCodePagePrewarmIdentity = 3;

	enum class VerticalMetricsMode : UInt8
	{
		FreeType = 0,
		Vanilla = 1,
	};

	enum class FontPrewarmRange : UInt8
	{
		CompleteCodePage = 0,
		GB2312 = 1,
	};

	enum class GlyphMaskType : UInt8
	{
		Fill = 0,
		Outline = 1,
		Glow = 2,
		DistanceField = 3,
		Shadow = 4,
		// Aggressive profiles bake Shadow -> Glow -> Outline -> Fill into one
		// D3D-native BGRA rectangle with a distinct persistent identity.
		Composite = 5,
	};

	struct FontConfig
	{
		UInt32 fontId = 0;
		std::array<ByteStyle, 2> styles;
		// CP936 may prewarm either the complete GBK table or the smaller GB2312
		// byte zone. Other code pages always resolve to CompleteCodePage.
		FontPrewarmRange prewarmRange = FontPrewarmRange::CompleteCodePage;
		VerticalMetricsMode verticalMetrics = VerticalMetricsMode::FreeType;
		float baseline = 0.0f;
		FontColorStyle fontColor;
		EffectQuality effectQuality = EffectQuality::Balanced;
		EffectStyle glow;
		EffectStyle outline;
		EffectStyle shadow;
		UInt64 layoutHash = 0;
		std::array<UInt64, 2> layoutRoleHashes = {};
		UInt64 maskGenerationHash = 0;
		std::array<UInt64, 2> maskGenerationRoleHashes = {};
		UInt64 shaderEffectHash = 0;
		// MTSDF double-byte atlases may be shared only by raster-compatible
		// configurations whose complete pixel-size span is at most eight pixels.
		// True SDF always keeps one atlas profile per font. Layout metrics remain
		// local to every logical font.
		UInt32 mtsdfDoubleByteOwnerFontId = 0;
		UInt32 mtsdfDoubleByteGroupSize = 1;
	};

	struct DistanceFieldRasterProfile
	{
		const FontConfig* ownerConfig = nullptr;
		float sourceToLogicalScale = 1.0f;
		UInt32 sdfSpread = 0;
	};

	struct GlyphBitmap
	{
		CpuMemoryLease cpuMemory;
		UInt64 cacheId = 0;
		UInt32 atlasRgb = 0x00FFFFFF;
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		int effectiveWidth = 0;
		int effectiveHeight = 0;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		DistanceFieldMethod distanceFieldMethod = GetConfiguredDistanceFieldMethod();
		UInt8 sdfSpread = 0;
		SInt32 strokeWidth26Dot6 = 0;
		bool colorBaked = false;
		UInt32 bakedRgba = 0;
		UInt8 bakedLayer = 0;
		// Single-channel coverage/true SDF, D3D-native BGRA MTSDF, or a
		// precomposed BGRA glyph, according to maskType/distanceFieldMethod.
		std::vector<UInt8> pixels;
	};

	// Composite raster ABI. Revision 3 uses the unhinted CPU coverage contour
	// shared with the fallback masks and the distance-field source outline.
	// Revision 2 removed transparent outer rows/columns after the final
	// four-layer BGRA composition and stored the corrected bearing rectangle.
	inline constexpr UInt32 kCpuCompositeRasterRevision = 3;

	inline constexpr UInt32 GlyphBitmapBytesPerPixel(GlyphMaskType maskType,
		DistanceFieldMethod distanceFieldMethod)
	{
		return maskType == GlyphMaskType::Composite ? 4u
			: maskType == GlyphMaskType::DistanceField
				? DistanceFieldBytesPerPixel(distanceFieldMethod) : 1u;
	}

	inline UInt32 GlyphBitmapBytesPerPixel(GlyphMaskType maskType)
	{
		return GlyphBitmapBytesPerPixel(
			maskType, GetConfiguredDistanceFieldMethod());
	}

	inline size_t ExpectedGlyphBitmapBytes(const GlyphBitmap& bitmap)
	{
		if (bitmap.width <= 0 || bitmap.height <= 0)
			return 0;
		return static_cast<size_t>(bitmap.width) * bitmap.height
			* GlyphBitmapBytesPerPixel(
				bitmap.maskType, bitmap.distanceFieldMethod);
	}

	struct GlyphBitmapRequest
	{
		const VectorEncodedGlyph* glyph = nullptr;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		UInt32 sdfSpread = 0;
	};

	// Layer IDs are part of the shader/native-packet ABI. Keep those IDs stable
	// while defining composition order independently: Shadow, Glow, Outline, Fill.
	inline constexpr UInt32 GetNativeFontLayerDrawRank(UInt32 layer)
	{
		switch (layer)
		{
		case 0: return 0; // Shadow
		case 1: return 1; // Glow
		case 2: return 2; // Outline
		case 3: return 3; // Fill
		default: return 4;
		}
	}

	struct NativeFontDrawRange
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 startIndex = 0;
		UInt32 primitiveCount = 0;
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		bool usesSdf = false;
		bool usesLiveTileRgb = true;
		float sdfSpreadPixels = 0.0f;
		float sourceToLogicalScale = 1.0f;
		NiColorA layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct NativeFontEffectShapeConfig
	{
		bool enabled = false;
		bool shaderEffects = false;
		// CPU-rasterized Fill/effect coverage remains in an A8 atlas. Colors and
		// live-Tile RGB selection are carried by each vertex so adjacent layers
		// can share one native coverage packet without baking text-specific RGB
		// into the atlas.
		bool bakedCoverage = false;
		// Aggressive profiles store the final configured BGRA glyph. Single-page
		// batches use the vanilla Tile shader; multi-page batches use the native
		// ARGB packet shader without reconstructing effect layers.
		bool precomposedArgb = false;
		DistanceFieldMethod distanceFieldMethod =
			GetConfiguredDistanceFieldMethod();
		EffectQuality quality = EffectQuality::Balanced;
		// Source pixels per configured logical pixel. Packet c2.w mirrors this
		// value so the native vertex shader can derive the distance-field AA footprint
		// without per-pixel ddx/ddy instructions.
		float rasterScale = 1.0f;
		float inverseAtlasWidth = 0.0f;
		float inverseAtlasHeight = 0.0f;
		float sdfSpreadPixels = 0.0f;
		float shadowBlurPixels = 0.0f;
		float shadowPower = 2.0f;
		float shadowGlowAlpha = 0.0f;
		float shadowOutlineAlpha = 0.0f;
		// Composite packets keep the shadow inside the glyph's single submitted
		// quad. Geometry uses logical offsets; the shader converts the same offset
		// to source pixels with the raster scale and each glyph's distance scale.
		float shadowOffsetX = 0.0f;
		float shadowOffsetY = 0.0f;
		float shadowOffsetRasterScale = 1.0f;
		float glowInnerPixels = 0.0f;
		float glowOuterPixels = 0.0f;
		float glowPower = 2.0f;
		float outlineWidthPixels = 0.0f;
		float outlineSoftnessPixels = 0.5f;
		std::array<NiColorA, 4> layerColorModifiers = {{
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f }
		}};
		std::array<bool, 4> layerUsesLiveTileRgb = {{ true, true, true, true }};
		// Retain every page property for the lifetime of the shape. This is
		// required by the DEFAULT-pool reset/retirement path; page 0 is already a
		// shape property, but secondary page textures otherwise have no property
		// reference that the reset tracker can observe.
		std::vector<NiTexturingPropertyPtr> atlasProperties;
		std::vector<NiTexturePtr> atlasTextures;
		std::vector<NiPoint2> atlasInverseSizes;
		std::vector<NativeFontDrawRange> ranges;
	};

	struct NativeFontShapeColorContract
	{
		// COLOR0 carries only the per-glyph base modifier. Packet c1 carries the
		// layer modifier, while c2.z selects whether fixed effects ignore both the
		// base and live Tile RGB. Every path continues to inherit live Tile alpha.
		// ABI 13 adds the exact glyph UV rectangle in TEXCOORD2. Composite
		// one-glyph quads may extend to cover an offset shadow without sampling
		// adjacent glyphs in the physical atlas.
		static constexpr UInt32 kTileUniformColorAbi = 13;

		UInt32 abiVersion = kTileUniformColorAbi;
		NiColorA minimumModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
		NiColorA maximumModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct AtlasGlyphInstance
	{
		VectorEncodedGlyph glyph;
		NiPoint3 pen;
		NiColorA color;
	};

	// Sealed profiles use the same encoded-slot contract as the extended .fnt
	// path. Keep only the data that survives into final geometry; the immutable
	// profile owns metrics, placements, page references, and baseline metadata.
	struct DirectGlyphCommand
	{
		NiPoint3 pen;
		UInt32 packedColor = 0xFFFFFFFFu;
		UInt16 directSlot = std::numeric_limits<UInt16>::max();
		UInt16 encodedCode = 0;
		UInt8 byteClass = 0;
		UInt8 byteLength = 0;
	};
	static_assert(sizeof(DirectGlyphCommand) <= 24);

	enum class GlyphAtlasBuildOutcome : UInt8
	{
		Unknown = 0,
		Created,
		EmptyInput,
		NoDrawableShaderQuads,
		NoDrawableCpuQuads,
		CpuMaskBuildFailure,
		QuadLimit,
		AtlasOrShapeFailure,
	};

	enum class GlyphAtlasMaskFailure : UInt8
	{
		None = 0,
		Fill,
		Shadow,
		Glow,
		Outline,
	};

	struct GlyphAtlasBuildDiagnostics
	{
		GlyphAtlasBuildOutcome outcome = GlyphAtlasBuildOutcome::Unknown;
		GlyphAtlasMaskFailure cpuMaskFailure = GlyphAtlasMaskFailure::None;
		UInt32 inputGlyphCount = 0;
		UInt32 missingMetricsCount = 0;
		UInt32 zeroByteLengthCount = 0;
		UInt32 controlGlyphCount = 0;
		UInt32 spaceGlyphCount = 0;
		UInt32 shaderQuadCount = 0;
		UInt32 cpuQuadCount = 0;
		UInt32 cpuAttempts = 0;
		UInt32 degradedLayerCount = 0;
		UInt32 shaderShapeAttempts = 0;
		UInt32 cpuShapeAttempts = 0;
		UInt32 firstEncodedCode = 0;
		UInt32 firstCodePoint = 0;
		UInt32 firstGlyphIndex = 0;
		UInt8 firstByteLength = 0;
		UInt8 firstByteClass = 0;
		UInt8 requestedQuality = 0;
		UInt8 resolvedQuality = 0;
		bool expectedEmpty = false;
		bool wantsShaderPath = false;
		bool hasEffects = false;
		bool requestsSdfFill = false;
		bool nativeRendererAvailable = false;
		bool shaderQuadsBuilt = false;
		bool shaderAtlasOrShapeFailed = false;
		bool cpuQuadsBuilt = false;
	};

	struct RuntimeFont;
	struct SealedDirectFontProfile;

	extern std::unordered_map<UInt32, FontConfig> g_configs;

	const FontConfig* FindConfig(UInt32 auiFontId);
	RuntimeFont* FindRuntimeFont(UInt32 auiFontId);
	RuntimeFont* FindActiveRuntime(const Font* apFont);
	RuntimeFont* EnsureRuntimeFont(UInt32 auiFontId);
	bool ApplyRuntimeMetrics(RuntimeFont& arRuntime, Font& arFont);
	bool LoadGlyphManifestIdentity(RuntimeFont& arRuntime, UInt32 auiEncodedCode,
		VectorFontByteClass aeByteClass, VectorEncodedGlyph& arGlyph);
	bool LoadGlyphManifest(RuntimeFont& arRuntime, UInt32 auiEncodedCode,
		VectorFontByteClass aeByteClass, VectorEncodedGlyph* apGlyph,
		FontLetter* apMetrics);
	FontLetter* EnsureDoubleByteMetrics(RuntimeFont& arRuntime, Font& arFont, UInt32 auiEncodedCode);
	bool DecodeEncodedGlyph(RuntimeFont& arRuntime, Font& arFont, const char* apText, VectorEncodedGlyph& arGlyph);
	enum class SealedDirectGlyphLookup : UInt8
	{
		Unavailable = 0,
		Resolved,
		Invalid,
	};
	SealedDirectGlyphLookup DecodeSealedDirectGlyph(RuntimeFont& arRuntime,
		const char* apText, VectorEncodedGlyph& arGlyph);
	SealedDirectGlyphLookup DecodeSealedDirectGlyph(
		const SealedDirectFontProfile& arProfile,
		const char* apText, VectorEncodedGlyph& arGlyph);
	void InvalidateSealedDirectFontProfile(RuntimeFont& arRuntime);
	void InvalidateSealedDirectFontProfileIfCurrent(RuntimeFont& arRuntime,
		const std::shared_ptr<const SealedDirectFontProfile>& apExpected);
	const FontConfig& GetRuntimeConfig(const RuntimeFont& arRuntime);
	UInt64 GetRuntimeDirectLayoutIdentity(const RuntimeFont& arRuntime);
	UInt64 GetRuntimeDirectRoleLayoutIdentity(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	void GetRuntimeDirectBaselineOffsets(const RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass, float& arRoleBaseline,
		std::vector<float>& arFaceBaselines);
	std::shared_ptr<const SealedDirectFontProfile>
		LoadRuntimeSealedDirectProfile(const RuntimeFont& arRuntime);
	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectFontProfile(RuntimeFont& arRuntime,
			float afRasterScale);
	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectLayoutProfile(RuntimeFont& arRuntime);
	void StoreRuntimeSealedDirectProfile(RuntimeFont& arRuntime,
		std::shared_ptr<const SealedDirectFontProfile> apProfile);
	void ReleaseSealedRuntimeFreeTypeState(RuntimeFont& arRuntime);
	UInt64 GetRuntimeMaskContentHash(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	size_t GetRuntimeDirectFaceCount(const RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	bool GetFreeTypeFontCacheDirectory(std::wstring& arDirectory);
	void MarkFreeTypeFontCacheFileUsed(const std::wstring& arPath);
	enum class PersistentCacheCleanupClass : UInt8
	{
		Neutral,
		CurrentDistanceField,
		InactiveDistanceField,
		Invalid,
	};
	PersistentFontCacheDomain GetPersistentFontCacheDomain();
	FontAtlasRoute GetPersistentFontCacheRoute();
	void SynchronizePersistentFontCacheRoute(FontAtlasRoute aeRoute);
	PersistentCacheCleanupClass ClassifyAtlasSnapshotCacheForCleanup(
		const std::wstring& arPath);
	PersistentCacheCleanupClass ClassifyDirectCachedLetterCacheForCleanup(
		const std::wstring& arPath);
	void DeleteUnusedFreeTypeFontCacheFiles(bool abDeleteAllUnused);
	bool MarkCurrentFallbackBitmapProfilesUsed(RuntimeFont& arRuntime,
		float afRasterScale);
	void MarkGlyphManifestComplete(RuntimeFont& arRuntime);
	const std::vector<UInt16>& GetCompleteCodePageEncodedUnits();
	const std::vector<UInt16>& GetFontPrewarmEncodedUnits(
		const FontConfig& arConfig);
	FontPrewarmRange ResolveFontPrewarmRange(const FontConfig& arConfig);
	const char* GetFontPrewarmRangeName(FontPrewarmRange aeRange,
		UInt32 aCodePage);
	float GetGlyphBaselineOffset(const RuntimeFont& arRuntime,
		const VectorEncodedGlyph& arGlyph);
	void GetGlyphBitmaps(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& arResults);
	void ResolveGlyphBitmapCacheIds(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<UInt64>& arCacheIds);
	void GetPrewarmGlyphBitmaps(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& arResults,
		UInt32 aMaximumWorkers);
	// Releases worker-local FreeType libraries/faces retained only to amortize
	// repeated batches for the current font. Call before atlas finalization or
	// memory recovery so this scratch never overlaps a large physical page.
	void ReleasePrewarmRasterWorkerContexts() noexcept;
	void BeginCompleteCodePageAtlasOnlyPrewarm();
	void EndCompleteCodePageAtlasOnlyPrewarm();
	void FlushGlyphBitmapDiskCache();
	UInt64 ReleaseGlyphBitmapDiskCacheMappings();
	bool ResetPersistentFontCachesForRegeneration(RuntimeFont& arRuntime);
	bool DeleteCompleteCodePageGlyphBitmapDiskCaches(
		const std::vector<UInt32>& arFontIds);
	void SetBitmapCacheReducedAfterPrewarm(bool abReduced);
	bool HardShadowIncludesGlow(const FontConfig& arConfig);
	bool HardShadowIncludesOutline(const FontConfig& arConfig);
	bool HasSdfEffects(const FontConfig& arConfig);
	SInt32 ResolveCpuEffectMaskIdentity(const FontConfig& arConfig,
		GlyphMaskType aeMaskType, float afRasterScale);
	bool ResolveSdfSpread(const FontConfig& arConfig, float afRasterScale, UInt32& arSpread,
		bool abIncludeEffects = true);
	bool ResolveDistanceFieldRasterProfile(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass, float afRasterScale,
		bool abIncludeEffects, DistanceFieldRasterProfile& arProfile);
	const FontConfig& GetDistanceFieldRasterOwnerConfig(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass);
	RuntimeFont* GetDistanceFieldRasterOwnerRuntime(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	bool IsMtsdfAtlasAlias(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass);
	bool ResolvePrewarmGlyph(RuntimeFont& arRuntime, const char* apBytes,
		size_t auiLength, VectorEncodedGlyph& arGlyph);
	bool PrewarmGlyphAtlas(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arBitmaps,
		float afRasterScale);
	enum class FontAtlasPrewarmProgressStage : UInt8
	{
		PublishPhysicalGroup,
		RestorePhysicalGroup,
		PlanPhysicalPools,
		PublishPhysicalPool,
	};
	struct FontAtlasPrewarmProgressReporter
	{
		using Callback = void (*)(FontAtlasPrewarmProgressStage aeStage,
			UInt32 auiItem, UInt32 auiTotal, void* apContext);

		Callback callback = nullptr;
		void* context = nullptr;

		void Report(FontAtlasPrewarmProgressStage stage,
			UInt32 item = 0, UInt32 total = 0) const
		{
			if (callback)
				callback(stage, item, total, context);
		}
	};
	bool TryLoadGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool StageGlyphAtlasSnapshotMetadata(RuntimeFont& arRuntime,
		float afRasterScale);
	bool HasGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& arRuntime,
		float afRasterScale);
	bool TryLoadGloballyRepackedGlyphAtlasSnapshotRole(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass, float afRasterScale);
	bool TryLoadGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& arRuntime,
		float afRasterScale);
	bool EnsureGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& arRuntime,
		float afRasterScale, bool* apRepacked = nullptr);
	bool DiscardGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool SaveGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool RebuildGlyphAtlasFromSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool ConsolidatePhysicalFontAtlasGroups(float afRasterScale,
		const FontAtlasPrewarmProgressReporter* apProgress = nullptr);
	bool ConsolidatePhysicalFontAtlasPools(float afRasterScale,
		const FontAtlasPrewarmProgressReporter* apProgress = nullptr);
	void PruneRetiredAtlasGenerationsSafely();
	bool BuildDirectGlyphAtlasTables(RuntimeFont& arRuntime, float afRasterScale);
	void QueueFontPrewarm(UInt32 auiFontId);
	void ResetAtlasAllocationMemoryPressure();
	void MarkAtlasAllocationMemoryPressure();
	bool ConsumeAtlasAllocationMemoryPressure();
	FontPrewarmPumpStatus PumpFontPrewarm();
	void ServiceFontPrewarmHostMessages();
	bool IsFontPrewarmActive();
	void ShutdownFontPrewarm();
	NiTriShape* TryCreateGlyphAtlasShape(Font& arFont, RuntimeFont& arRuntime,
		const std::vector<AtlasGlyphInstance>& arGlyphs, float afRasterScale,
		bool abPrepareObject, const NiColorA& arTileColor, bool abSuppressEffects,
		GlyphAtlasBuildDiagnostics* apDiagnostics = nullptr);
	NiTriShape* TryCreateSealedGlyphAtlasShape(Font& arFont,
		RuntimeFont& arRuntime,
		const std::shared_ptr<const SealedDirectFontProfile>& apProfile,
		const std::vector<DirectGlyphCommand>& arGlyphs, float afRasterScale,
		bool abPrepareObject, const NiColorA& arTileColor,
		bool abSuppressEffects,
		GlyphAtlasBuildDiagnostics* apDiagnostics = nullptr);
	bool IsNativeFontRendererAvailable();
	bool ResolveNativeFontEffectQuality(EffectQuality aeRequested, EffectQuality& arResolved);
	bool PrepareNativeFontAtlasShape(Font& arFont, NiTriShape* apShape, UInt32 auiFontId,
		UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const NativeFontEffectShapeConfig* apEffectConfig = nullptr,
		const NativeFontShapeColorContract* apColorContract = nullptr,
		std::shared_ptr<const NativeFontPayloadTemplate> apPayloadTemplate = {},
		const NiPoint3& arGeometryOrigin = NiPoint3());
	bool PrepareNativeFontSingletonFacadeShape(Font& arFont, NiTriShape* apShape,
		UInt32 auiFontId, UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const NativeFontEffectShapeConfig* apEffectConfig,
		const NativeFontShapeColorContract* apColorContract,
		std::shared_ptr<const NativeFontPayloadTemplate> apPayloadTemplate,
		const NiPoint3& arGeometryOrigin);
	bool PrepareNativeFontVanillaLayoutShape(Font& arFont, NiTriShape* apShape,
		UInt32 auiFontId, UInt32 auiGlyphCount, UInt32 auiQuadCount,
		NativeFontVanillaLayoutKind aeLayoutKind,
		const NativeFontEffectShapeConfig* apEffectConfig,
		const NativeFontShapeColorContract* apColorContract,
		std::shared_ptr<const NativeFontPayloadTemplate> apPayloadTemplate,
		const NiPoint3& arGeometryOrigin);
}
