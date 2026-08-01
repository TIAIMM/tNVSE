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

namespace fonthook::vectorfont
{
	inline DistanceFieldMethod GetConfiguredDistanceFieldMethod()
	{
		return g_uiFreeTypeFontDistanceFieldMode == 0
			? DistanceFieldMethod::TrueSdf : DistanceFieldMethod::Mtsdf;
	}

	inline bool UsesMtsdfDistanceField()
	{
		return GetConfiguredDistanceFieldMethod() == DistanceFieldMethod::Mtsdf;
	}

	inline const char* GetConfiguredDistanceFieldMethodName()
	{
		return UsesMtsdfDistanceField() ? "MTSDF" : "true SDF";
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
	constexpr UInt32 kMaximumPrewarmRasterWorkers = 12;
	constexpr UInt32 kExpensivePrewarmParallelThreshold = 8;
	constexpr UInt32 kFillPrewarmParallelThreshold = 64;
	constexpr UInt32 kFillPrewarmWorkChunk = 8;

	struct NativeA8PayloadTemplate;
	// The runtime now has more than 255 independently reported counters.
	// Keeping an UInt8 base aliases the tail entries and can index the wrong
	// atomic slot after integer promotion.
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
		PreparedTextHit,
		PreparedTextMiss,
		PreparedTextPromotionBypass,
		PreparedTextGlobalProbe,
		PreparedTextGlobalProbeMiss,
		PreparedTextAdmission,
		PreparedTextEviction,
		PreparedTextAdmissionRejected,
		PreparedTextRejectionBypass,
		AtlasHit,
		AtlasCreated,
		AtlasGrown,
		AtlasUpload,
		AtlasUploadBytes,
		AtlasUploadRect,
		TextArtifactHit,
		TextArtifactMiss,
		TextArtifactHotHit,
		TextArtifactAdmissionBypass,
		TextArtifactAdmission,
		TextArtifactEviction,
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
		SortedMixedEqualDepthRunRestored,
		SortedMixedEqualDepthItemRestored,
		SortedMixedEqualDepthRestoreRejected,
		SortedOriginalOrderAnchorSort,
		SortedOriginalOrderAnchorItem,
		SortedOriginalOrderAnchorMixedRun,
		SortedOriginalOrderAnchorFallback,
		SortedOriginalOrderAnchorPredecessorFallback,
		SortedOriginalOrderAnchorProofFallback,
		SortedOriginalOrderStockEquivalentSort,
		SortedOriginalOrderStockEquivalentItem,
		SortedOriginalOrderSidecarRecovered,
		SortedOriginalOrderSidecarMixedRun,
		SortedOriginalOrderSidecarLegacy,
		SortedOriginalOrderAnchorFailGate,
		SortedOriginalOrderAnchorFailCount,
		SortedOriginalOrderAnchorFailStorage,
		SortedOriginalOrderAnchorFailSource,
		SortedOriginalOrderAnchorFailDepth,
		SortedOriginalOrderAnchorFailMetadata,
		SortedOriginalOrderAnchorFailRegistration,
		SortedOriginalOrderAnchorFailGroup,
		SortedOriginalOrderAnchorFailSingleton,
		SortedOriginalOrderAnchorFailCoverage,
		SortedOriginalOrderAnchorFailApply,
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
		VisibilityScissorPreConstants,
		VisibilityScissorPostConstants,
		VisibilityPreflightSkipped,
		VisibilityPacketsSaved,
		VisibilityVerticesSaved,
		ViewportNodeInstalled,
		ViewportNodeInstallFailed,
		ViewportCullCheck,
		ViewportCullFastVisible,
		ViewportCullDeepCheck,
		ViewportCullDeepTile,
		ViewportCulled,
		ViewportFailOpen,
		ViewportFailListIndex,
		ViewportFailClips,
		ViewportFailClipWindow,
		ViewportFailRootBounds,
		ViewportFailTransform,
		ViewportFailNodeIdentity,
		ViewportFailSubtreeTopology,
		ViewportFailSubtreeBounds,
		ViewportDeepOverlap,
		ViewportAppCulled,
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
		VirtualStockCandidate,
		VirtualStockSingleton,
		VirtualStockGroup,
		VirtualStockShape,
		VirtualStockDraw,
		VirtualStockStaticHit,
		VirtualStockDynamicHit,
		VirtualStockRebind,
		VirtualStockRevoke,
		VirtualStockFacadeFallback,
		VirtualStockFollowerSkipped,
		VirtualStockSortedPreflightSaved,
		VirtualStockProxyPacketSaved,
		VirtualStockFallbackNoParent,
		VirtualStockFallbackPacketLimit,
		VirtualStockFallbackCpuBudget,
		VirtualStockFallbackStaticNotReady,
		VirtualStockFallbackTopology,
		VirtualStockFallbackShader,
		VirtualStockFallbackGeneration,
		VirtualStockFallbackAtlas,
		VirtualStockFallbackResource,
		VirtualStockFallbackNoncontiguous,
		VirtualStockRegistrationResolved,
		VirtualStockRegistrationRejected,
		VirtualStockRegistrationMissing,
		VirtualStockRegistrationDuplicate,
		VirtualStockRegistrationOrderMismatch,
		ConstantOwnershipSegment,
		ConstantOwnershipReuse,
		ConstantOwnershipRelease,
		StockConstantUpdate,
		SegmentDevicePostSet,
		CompositeConstantFullUpload,
		NativePacketConstantReuse,
		CompositeConstantPartialUpload,
		StockPixelConstantCompatibilityRepublish,
		NativePacketConstantRegisterUpload,
		NativePacketConstantFullTailElided,
		NativePrivateStateStockTilePreserve,
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
		VertexAaConstantStockPreserved,
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
		CommandVirtualSinglePacketRecorded,
		CommandVirtualSinglePacketHit,
		CommandVirtualSinglePacketMiss,
		CommandVirtualSinglePacketReplay,
		CommandVirtualSinglePacketFallback,
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
		StandardPassLiteStockFallback,
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
		SegmentDeviceDrawmodeSet,
		SegmentDeviceDrawmodeReuse,
		CommandStockBootstrapSaved,
		CommandVirtualSpanFused,
		CommandVirtualFollowerConsumed,
		CommandDirectRangeReplay,
		SegmentDevicePostElision,
		CommandPacketLightValidation,
		CommandPacketEpochGuard,
		CommandPacketStateValidationElided,
		CommandPacketRangeValidation,
		CommandPacketRangeValidated,
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
		Count,
	};
	static_assert(
		static_cast<UInt32>(FreeTypePerfCounter::Count) <= 0xFFFFu,
		"FreeTypePerfCounter no longer fits its UInt16 storage");

	void RecordFreeTypePerf(FreeTypePerfCounter aeCounter, UInt64 auiAmount = 1);
	void ReportFreeTypePerf();

	enum class FreeTypePerfPhase : UInt8
	{
		Layout = 0,
		Sidecar,
		DirectCompile,
		NativeRegistration,
		Preflight,
		Submit,
		CommandBuild,
		CommandBuildStamp,
		CommandBuildVirtual,
		CommandBuildOrdinary,
		CommandBuildFinalize,
		CommandSubmit,
		ExtendedFntGeometry,
		Count,
	};

	class FreeTypePerfScope
	{
	public:
		explicit FreeTypePerfScope(
			FreeTypePerfPhase aePhase, bool abEnabled = true);
		~FreeTypePerfScope();

		FreeTypePerfScope(const FreeTypePerfScope&) = delete;
		FreeTypePerfScope& operator=(const FreeTypePerfScope&) = delete;

	private:
		FreeTypePerfPhase m_phase;
		SInt64 m_start = 0;
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
		Original = 1,
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

	struct MtsdfSharedRasterProfile
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
		std::vector<UInt8> alpha;
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
	inline constexpr UInt32 GetA8LayerDrawRank(UInt32 layer)
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

	struct A8DrawRange
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

	struct A8EffectShapeConfig
	{
		bool enabled = false;
		bool shaderEffects = false;
		// CPU-rasterized Fill/effect coverage remains in an A8 atlas. Colors and
		// live-Tile RGB selection are carried by each vertex so adjacent layers
		// can share one native coverage packet without baking text-specific RGB
		// into the atlas.
		bool bakedCoverage = false;
		// Aggressive profiles store the final configured BGRA glyph. Single-page
		// batches use the stock Tile shader; multi-page batches use the native
		// ARGB packet shader without reconstructing effect layers.
		bool precomposedArgb = false;
		DistanceFieldMethod distanceFieldMethod =
			GetConfiguredDistanceFieldMethod();
		EffectQuality quality = EffectQuality::Balanced;
		// Source pixels per configured logical pixel. Packet c2.w mirrors this
		// value so the native vertex shader can derive the MTSDF AA footprint
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
		std::vector<A8DrawRange> ranges;
	};

	struct A8ShapeColorContract
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
		bool a8RendererAvailable = false;
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
		std::vector<std::shared_ptr<const GlyphBitmap>>& arResults);
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
	bool ResolveMtsdfSharedRasterProfile(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass, float afRasterScale,
		bool abIncludeEffects, MtsdfSharedRasterProfile& arProfile);
	const FontConfig& GetMtsdfAtlasConfig(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass);
	RuntimeFont* GetMtsdfAtlasRuntime(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	bool IsMtsdfAtlasAlias(const FontConfig& arConfig,
		VectorFontByteClass aeByteClass);
	bool ResolvePrewarmGlyph(RuntimeFont& arRuntime, const char* apBytes,
		size_t auiLength, VectorEncodedGlyph& arGlyph);
	bool PrewarmGlyphAtlas(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arBitmaps,
		float afRasterScale);
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
	bool ConsolidatePhysicalFontAtlasGroups(float afRasterScale);
	bool BuildDirectGlyphAtlasTables(RuntimeFont& arRuntime, float afRasterScale);
	void QueueFontPrewarm(UInt32 auiFontId);
	FontPrewarmPumpStatus PumpFontPrewarm();
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
	bool IsA8RendererAvailable();
	bool ResolveA8EffectQuality(EffectQuality aeRequested, EffectQuality& arResolved);
	bool PrepareA8AtlasShape(Font& arFont, NiTriShape* apShape, UInt32 auiFontId,
		UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const A8EffectShapeConfig* apEffectConfig = nullptr,
		const A8ShapeColorContract* apColorContract = nullptr,
		std::shared_ptr<const NativeA8PayloadTemplate> apPayloadTemplate = {},
		const NiPoint3& arGeometryOrigin = NiPoint3());
	bool PrepareVirtualStockA8ShapeGroup(Font& arFont,
		const std::vector<NiTriShape*>& arShapes, UInt32 auiPrimarySlot,
		UInt32 auiFontId, UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const A8EffectShapeConfig* apEffectConfig,
		const A8ShapeColorContract* apColorContract,
		std::shared_ptr<const NativeA8PayloadTemplate> apPayloadTemplate,
		const NiPoint3& arGeometryOrigin, bool abUseCompositeTopology);
	bool PrepareVirtualStockA8Singleton(Font& arFont, NiTriShape* apShape,
		UInt32 auiFontId, UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const A8EffectShapeConfig* apEffectConfig,
		const A8ShapeColorContract* apColorContract,
		std::shared_ptr<const NativeA8PayloadTemplate> apPayloadTemplate,
		const NiPoint3& arGeometryOrigin, bool abUseCompositeTopology);
}
