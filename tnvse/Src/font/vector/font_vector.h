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

	void LoadFreeTypeFontConfig();
	void FreeTypeFontDebugLog(const char* apFormat, ...);
	void FlushFreeTypeFontDebugLog();
	void FlushFreeTypeFontDebugLogFully();
	void FinalizeFreeTypeUioDetection();
	void FinalizeFreeTypeA8Detection();
	void HandleFreeTypeA8MainLoop();
	void HandleFreeTypeShaderLoaderMessage(UInt32 auiMessageType);
	void InitializeFreeTypeDefaultPoolAtlas();
	void HandleFreeTypeDefaultPoolAtlasMainLoop();
	void ShutdownFreeTypeDefaultPoolAtlas();
	enum class FontPrewarmPumpStatus : UInt8
	{
		Idle,
		Active,
		Completed,
	};
	FontPrewarmPumpStatus PumpFreeTypeFontPrewarm();
	bool IsFreeTypeFontPrewarmActive();
	void ShutdownFreeTypeFontPrewarm();
	void PumpFreeTypeFontPerformance();
	void ReportFreeTypeFontPerformanceNow();
	void RecordFreeTypePreparedTextCacheResult(bool abHit);
	void RecordFreeTypeViewportNodeInstallResult(bool abInstalled);
	enum class FreeTypeViewportCullFailReason : UInt8
	{
		None,
		ListIndex,
		Clips,
		ClipWindow,
		RootBounds,
		Transform,
		NodeIdentity,
		SubtreeTopology,
		SubtreeBounds,
	};
	void RecordFreeTypeViewportCullResult(
		bool abCulled, bool abFailOpen, bool abFastVisible,
		bool abDeepCheck, UInt32 auiVisitedTiles,
		FreeTypeViewportCullFailReason aeFailReason,
		bool abDeepOverlap, bool abAppCulled);
	void FlushFreeTypePersistentFontCache();
	float GetCanonicalFreeTypeRasterScale();
	float ConsumeFreeTypeCreateTextScale();
	void FreeTypeCreateTextEntryHook();
	bool IsFreeTypeEffectSuppressionActive();
	bool IsFreeTypeVuiProxyMeasureOnlyActive();
	bool InitializeFreeTypeVectorRenderer();
	bool IsFreeTypeFontConfigured(UInt32 auiFontId);
	bool ActivateFreeTypeFont(Font* apFont, bool abForce = false);
	bool IsFreeTypeFontActive(const Font* apFont);
	bool HasEnabledFreeTypeFontEffects(const Font* apFont);
	bool GetFreeTypeLayoutIdentity(const Font* apFont, UInt64& arIdentity);
	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 auiEncodedCode);
	bool DecodeFreeTypeGlyph(Font* apFont, const char* apText, VectorEncodedGlyph& arGlyph);
	std::shared_ptr<const PreparedDirectTextSidecar>
		ConsumeFreeTypePreparedTextSidecar(const Font::TextData* apData,
			const Font* apFont, const char* apPreparedText);
	NiTriShape* CreateEmptyFreeTypeTextShape(Font* apFont, bool abPrepareObject);

	class VectorTextBuilder
	{
	public:
		VectorTextBuilder(Font* apFont, bool abPrepareObject, float afRasterScale = 1.0f,
			const NiColorA* apTileColor = nullptr);
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
