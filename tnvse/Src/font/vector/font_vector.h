#pragma once

#include "font_cpu_budget.h"
#include "ui_decode.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

namespace fonthook
{
	enum class VectorFontByteClass : UInt8
	{
		SingleByte = 0,
		DoubleByte = 1,
	};

	struct VectorEncodedGlyph
	{
		UInt32 encodedCode = 0;
		UInt32 codePoint = 0;
		UInt32 glyphIndex = 0;
		UInt16 faceIndex = 0;
		UInt16 directSlot = std::numeric_limits<UInt16>::max();
		bool hasGlyphIdentity = false;
		bool hasDirectMetrics = false;
		UInt8 byteLength = 0;
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		FontLetter* metrics = nullptr;
		float directWidth = 0.0f;
		float directHeight = 0.0f;
		float directLeadingEdge = 0.0f;
		float directSpacing = 0.0f;
		float directTopEdge = 0.0f;
		float directBaselineOffset = 0.0f;
	};

	inline bool HasVectorGlyphMetrics(const VectorEncodedGlyph& glyph)
	{
		return glyph.hasDirectMetrics || glyph.metrics;
	}

	inline float GetVectorGlyphRenderAdvance(const VectorEncodedGlyph& glyph)
	{
		if (glyph.hasDirectMetrics)
		{
			return glyph.directLeadingEdge + glyph.directWidth
				+ (glyph.directWidth > 0.0f ? glyph.directSpacing : 0.0f);
		}
		if (!glyph.metrics)
			return 0.0f;
		return glyph.metrics->fLeadingEdge + glyph.metrics->fWidth
			+ (glyph.metrics->fWidth > 0.0f
				? glyph.metrics->fSpacing : 0.0f);
	}

	inline float GetVectorGlyphTopEdge(const VectorEncodedGlyph& glyph)
	{
		return glyph.hasDirectMetrics ? glyph.directTopEdge
			: glyph.metrics ? glyph.metrics->fTopEdge : 0.0f;
	}

	namespace vectorfont
	{
		enum class DirectProfileAcquireStatus : UInt8
		{
			NotAttempted = 0,
			Acquired,
			MissingRuntimeSlot,
			EpochMismatch,
			LayoutIdentityMismatch,
			CodePageMismatch,
			ProfileInvalid,
			RasterScaleMismatch,
		};

		inline constexpr UInt8 kDirectProfileFailureEpoch = 1u << 0;
		inline constexpr UInt8 kDirectProfileFailureLayoutIdentity = 1u << 1;
		inline constexpr UInt8 kDirectProfileFailureCodePage = 1u << 2;
		inline constexpr UInt8 kDirectProfileFailureProfileInvalid = 1u << 3;
		inline constexpr UInt8 kDirectProfileFailureRasterScale = 1u << 4;

		enum class PreparedTextSidecarReason : UInt8
		{
			Pending = 0,
			Produced,
			InvalidArguments,
			NoActiveRuntime,
			NoSealedDirectProfile,
			DirectHyphenUnavailable,
			DirectHyphenInvalid,
			DirectGlyphUnavailable,
			DirectGlyphInvalid,
			DirectSpaceUnavailable,
			DirectSpaceInvalid,
			LayoutIdentityUnavailable,
			UnitZeroLength,
			UnitNonContiguous,
			UnitOutOfRange,
			GlyphSlotInvalid,
			GlyphByteClassInvalid,
			GlyphEncodingMismatch,
			ControlEncodingMismatch,
			IncompleteCoverage,
			NoSidecarProduced,
			RejectedBatch,
			CapturePreparedPointerChanged,
			CaptureLineSeparatorChanged,
			CaptureLengthChanged,
			CaptureLayoutIdentityUnavailable,
			CaptureLayoutIdentityChanged,
			DirectProfileMissingRuntimeSlot,
			DirectProfileEpochMismatch,
			DirectProfileLayoutIdentityMismatch,
			DirectProfileCodePageMismatch,
			DirectProfileInvalid,
		};

		enum class VectorTextBuildRoute : UInt8
		{
			Unknown = 0,
			Unavailable,
			SealedDirect,
			Generic,
			SealedToGeneric,
			SealedToGenericFailed,
			MeasureOnly,
			Vanilla,
		};

		enum class PreparedTextSidecarOffsetDomain : UInt8
		{
			None = 0,
			LayoutSource,
			PreparedText,
		};

		enum class VectorTextBuildReason : UInt8
		{
			None = 0,
			BuilderUnavailable,
			NoActiveRuntime,
			WhiteTextureUnavailable,
			NoSealedDirectProfile,
			PreparedSidecarRejected,
			PreparedSidecarIgnoredNoProfile,
			DirectGlyphMetricsUnavailable,
			DirectLookupUnavailable,
			DirectLookupInvalid,
			DirectReplayDecodeFailed,
			SealedArtifactFailed,
			GenericArtifactFailed,
			DirectProfileMissingRuntimeSlot,
			DirectProfileEpochMismatch,
			DirectProfileLayoutIdentityMismatch,
			DirectProfileCodePageMismatch,
			DirectProfileInvalid,
			DirectProfileRasterScaleMismatch,
		};

		struct FreeTypeLongTextTrace
		{
			UInt32 traceId = 0;
			SInt32 fontId = -1;
			UInt32 sourceByteCount = 0;
			UInt32 preparedByteCount = 0;
			UInt32 preparedCharCount = 0;
			UInt32 preparedLineCount = 0;
			PreparedTextSidecarReason sidecarReason =
				PreparedTextSidecarReason::Pending;
			UInt32 sidecarFailureByteOffset =
				std::numeric_limits<UInt32>::max();
			UInt32 sidecarFailureEncodedCode = 0;
			UInt32 sidecarUnitCount = 0;
			UInt8 sidecarFailureByteLength = 0;
			UInt8 sidecarFailureByteClass = 0;
			PreparedTextSidecarOffsetDomain sidecarFailureOffsetDomain =
				PreparedTextSidecarOffsetDomain::None;
			bool sidecarRequested = false;
			bool sidecarDirectProfileAcquired = false;
			bool sidecarPublished = false;
			bool sidecarCaptured = false;
			bool sidecarRejected = false;
			bool sidecarConsumed = false;
			DirectProfileAcquireStatus sidecarAcquireStatus =
				DirectProfileAcquireStatus::NotAttempted;
			UInt8 sidecarAcquireFailureMask = 0;
			bool sidecarProfileSlotPresent = false;
			UInt32 sidecarProfileEpochExpected = 0;
			UInt32 sidecarProfileEpochActual = 0;
			UInt64 sidecarProfileLayoutExpected = 0;
			UInt64 sidecarProfileLayoutActual = 0;
			UInt32 sidecarProfileCodePageExpected = 0;
			UInt32 sidecarProfileCodePageActual = 0;

			VectorTextBuildRoute builderInitialRoute =
				VectorTextBuildRoute::Unknown;
			VectorTextBuildRoute builderFinalRoute =
				VectorTextBuildRoute::Unknown;
			VectorTextBuildReason builderReason =
				VectorTextBuildReason::None;
			UInt32 builderFailureEncodedCode = 0;
			UInt32 builderDirectGlyphCount = 0;
			UInt32 builderGenericGlyphCount = 0;
			UInt8 builderFailureByteLength = 0;
			UInt8 builderFailureByteClass = 0;
			UInt8 builderAtlasOutcome = 0;
			UInt16 builderShapeFailureStage = 0;
			bool builderShapeCreated = false;
		};

		class ScopedFreeTypeLongTextTrace final
		{
		public:
			explicit ScopedFreeTypeLongTextTrace(
				FreeTypeLongTextTrace* apTrace) noexcept;
			~ScopedFreeTypeLongTextTrace() noexcept;

			ScopedFreeTypeLongTextTrace(
				const ScopedFreeTypeLongTextTrace&) = delete;
			ScopedFreeTypeLongTextTrace& operator=(
				const ScopedFreeTypeLongTextTrace&) = delete;

		private:
			FreeTypeLongTextTrace* m_previous = nullptr;
			bool m_active = false;
		};

		FreeTypeLongTextTrace* GetActiveFreeTypeLongTextTrace() noexcept;
		const char* DirectProfileAcquireStatusName(
			DirectProfileAcquireStatus aeStatus) noexcept;
		const char* PreparedTextSidecarReasonName(
			PreparedTextSidecarReason aeReason) noexcept;
		const char* VectorTextBuildRouteName(
			VectorTextBuildRoute aeRoute) noexcept;
		const char* VectorTextBuildReasonName(
			VectorTextBuildReason aeReason) noexcept;
		const char* VectorTextBuildOutcomeName(UInt8 aeOutcome) noexcept;
	}

	enum class DirectTextUnitKind : UInt8
	{
		Glyph = 0,
		LineBreak,
		Tab,
		Icon,
		Control,
	};

	struct DirectTextUnit
	{
		UInt32 byteOffset = 0;
		float advance = 0.0f;
		UInt16 directSlot = std::numeric_limits<UInt16>::max();
		UInt16 encodedCode = 0;
		UInt8 byteLength = 0;
		UInt8 byteClass = 0;
		DirectTextUnitKind kind = DirectTextUnitKind::Control;
		UInt8 reserved = 0;
	};

	struct PreparedDirectTextSidecar
	{
		UInt64 layoutIdentity = 0;
		size_t textLength = 0;
		bool rejectBatch = false;
		std::vector<DirectTextUnit> units;
	};

	// Font::TextData is a vanilla 0x28-byte stack object with no ownership token.
	// Keep the direct-layout sidecar scoped to the synchronous CreateText call
	// instead of retaining raw TextData/BSString pointers across calls.
	class PreparedTextSidecarCapture final
	{
	public:
		PreparedTextSidecarCapture(const Font::TextData* apData,
			const Font* apFont);
		~PreparedTextSidecarCapture();

		PreparedTextSidecarCapture(const PreparedTextSidecarCapture&) = delete;
		PreparedTextSidecarCapture& operator=(
			const PreparedTextSidecarCapture&) = delete;

		bool Matches(const Font::TextData* apData,
			const Font* apFont) const;
		void Publish(const Font::TextData* apData, const Font* apFont,
			std::shared_ptr<const PreparedDirectTextSidecar> apSidecar);
		std::shared_ptr<const PreparedDirectTextSidecar> Take();

	private:
		const Font::TextData* m_data = nullptr;
		const Font* m_font = nullptr;
		const char* m_preparedText = nullptr;
		char m_lineSeparator = 0;
		std::shared_ptr<const PreparedDirectTextSidecar> m_sidecar;
		vectorfont::FreeTypeLongTextTrace* m_trace = nullptr;
		PreparedTextSidecarCapture* m_previous = nullptr;
	};

	void LoadFreeTypeFontConfig();
	void FreeTypeFontDebugLog(const char* apFormat, ...);
	void FlushFreeTypeFontDebugLog();
	void FlushFreeTypeFontDebugLogFully();
	void FinalizeFreeTypeNativeRendererDetection();
	void HandleFreeTypeNativeRendererMainLoop();
	void HandleFreeTypeShaderLoaderMessage(UInt32 auiMessageType);
	void InitializeFreeTypeDefaultPoolAtlas();
	void HandleFreeTypeDefaultPoolAtlasMainLoop();
	void ShutdownFreeTypeDefaultPoolAtlas();
	enum class FontPrewarmPumpStatus : UInt8
	{
		Idle,
		Active,
		Completed,
		Failed,
	};
	FontPrewarmPumpStatus PumpFreeTypeFontPrewarm();
	// DeferredInit owns the pre-entry barrier. CPU and disk work stays on a
	// below-normal-priority coordinator while the blocked game thread services
	// engine/D3D publication and its window message queue.
	FontPrewarmPumpStatus RunFreeTypeFontPrewarmLoadingBarrier();
	bool IsFreeTypeFontPrewarmActive();
	void ShutdownFreeTypeFontPrewarm();
	void PumpFreeTypeFontPerformance();
	void ReportFreeTypeFontPerformanceNow();
	void FlushFreeTypePersistentFontCache();
	float GetCanonicalFreeTypeRasterScale();
	bool IsFreeTypeEffectSuppressionActive();
	bool IsFreeTypeVuiProxyMeasureOnlyActive();
	bool InitializeFreeTypeVectorRenderer();
	bool IsFreeTypeFontConfigured(UInt32 auiFontId);
	bool ActivateFreeTypeFont(Font* apFont, bool abForce = false);
	// LoadingMenu-owned progress text can be created while the vector-font cache
	// itself is being rebuilt and while LoadingMenu owns renderer/UI locks. Keep
	// FreeType active, but route native shapes around creation-time renderer
	// precache. The override is thread-local and nestable.
	class ScopedFreeTypeNoPrecacheRoute final
	{
	public:
		ScopedFreeTypeNoPrecacheRoute() noexcept;
		~ScopedFreeTypeNoPrecacheRoute() noexcept;

		ScopedFreeTypeNoPrecacheRoute(
			const ScopedFreeTypeNoPrecacheRoute&) = delete;
		ScopedFreeTypeNoPrecacheRoute& operator=(
			const ScopedFreeTypeNoPrecacheRoute&) = delete;
	};
	bool IsFreeTypeNoPrecacheRouteActive() noexcept;
	bool IsFreeTypeFontActive(const Font* apFont);
	bool HasEnabledFreeTypeFontEffects(const Font* apFont);
	bool GetFreeTypeLayoutIdentity(const Font* apFont, UInt64& arIdentity);
	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 auiEncodedCode);
	bool DecodeFreeTypeGlyph(Font* apFont, const char* apText, VectorEncodedGlyph& arGlyph);
	// Construct the exact retail BSScissorTriShape/TileShaderProperty pair, but
	// bind a FreeType-owned texture instead of reading Font::pTextureData[0].
	// The property and texture must describe the same atlas page. This changes
	// only the FreeType draw shape; the original FNT/TEX objects remain loaded
	// and Font::pTextureData is never modified.
	NiTriShape* CreateFreeTypeTextShape(UInt32 auiQuadCount,
		const NiColorA& arTileColor, bool abPrepareObject,
		NiTexturingProperty* apTextureProperty, NiTexture* apTexture);
	// Bootstrap shells (empty text and the native proxy pool) use the permanent
	// 1x1 FreeType texture until their real atlas page is installed.
	NiTriShape* CreateFreeTypePlaceholderTextShape(UInt32 auiQuadCount,
		const NiColorA& arTileColor, bool abPrepareObject);
	NiTriShape* CreateEmptyFreeTypeTextShape(Font* apFont, bool abPrepareObject);

	class VectorTextBuilder
	{
	public:
		VectorTextBuilder(Font* apFont, bool abPrepareObject,
			float afRasterScale = 1.0f,
			const NiColorA* apTileColor = nullptr,
			bool abSuppressEffects = false);
		~VectorTextBuilder();

		VectorTextBuilder(const VectorTextBuilder&) = delete;
		VectorTextBuilder& operator=(const VectorTextBuilder&) = delete;

		bool IsAvailable() const;
		bool UsesSealedDirectProfile() const;
		void ReserveGlyphs(size_t auiCount);
		bool AddGlyph(const VectorEncodedGlyph& arGlyph, const NiPoint3& arPen, const NiColorA* apColor);
		bool AddEncodedGlyph(const char* apEncodedText,
			const NiPoint3& arPen, const NiColorA* apColor,
			VectorEncodedGlyph* apDecodedGlyph = nullptr);
		NiTriShape* Finish();

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	void BeginFreeTypeRichTextRender(NiNode* apParent);
	void EndFreeTypeRichTextRender();
	bool AddFreeTypeRichTextGlyph(
		Font* apFont,
		const FontManager::CharData* apChar,
		const NiPoint3& arPen,
		const NiColorA* apColor);
}
