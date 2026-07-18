#pragma once

#include "ui_decode.h"

#include <cstddef>
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
		bool hasGlyphIdentity = false;
		UInt8 byteLength = 0;
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		FontLetter* metrics = nullptr;
	};

	struct FreeTypeLayoutGlyph
	{
		VectorEncodedGlyph glyph;
		UInt32 cluster = 0;
		float xAdvance = 0.0f;
		float xOffset = 0.0f;
		float yOffset = 0.0f;
	};

	struct FreeTypeLayoutRun
	{
		using GlyphStorage = std::vector<FreeTypeLayoutGlyph>;
		std::shared_ptr<const GlyphStorage> glyphs;
		float advance = 0.0f;
		bool shaped = false;
	};

	void LoadFreeTypeFontConfig();
	void FreeTypeFontDebugLog(const char* apFormat, ...);
	void FlushFreeTypeFontDebugLog();
	void FinalizeFreeTypeUioDetection();
	void FinalizeFreeTypeA8Detection();
	void HandleFreeTypeA8MainLoop();
	void HandleFreeTypeShaderLoaderMessage(UInt32 auiMessageType);
	void InitializeFreeTypeDefaultPoolAtlas();
	void HandleFreeTypeDefaultPoolAtlasMainLoop();
	void ShutdownFreeTypeDefaultPoolAtlas();
	void PumpFreeTypeFontPrewarm();
	void PumpFreeTypeFontPerformance();
	void RecordFreeTypePreparedTextCacheResult(bool abHit);
	void FlushFreeTypePersistentFontCache();
	float GetCanonicalFreeTypeRasterScale();
	float ConsumeFreeTypeCreateTextScale();
	void FreeTypeCreateTextEntryHook();
	bool IsFreeTypeEffectSuppressionActive();
	bool InitializeFreeTypeVectorRenderer();
	bool IsFreeTypeFontConfigured(UInt32 auiFontId);
	bool ActivateFreeTypeFont(Font* apFont, bool abForce = false);
	bool IsFreeTypeFontActive(const Font* apFont);
	bool HasEnabledFreeTypeFontEffects(const Font* apFont);
	bool GetFreeTypeLayoutIdentity(const Font* apFont, UInt64& arIdentity);
	bool BuildFreeTypeUnicodeLineBreakMap(const Font* apFont, const char* apText,
		size_t auiLength, std::vector<UInt8>& arBreakAfter);
	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 auiEncodedCode);
	bool DecodeFreeTypeGlyph(Font* apFont, const char* apText, VectorEncodedGlyph& arGlyph);
	bool LayoutFreeTypeRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping = true);
	bool LayoutFreeTypeFinalRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping = true);
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
		void ReserveGlyphs(size_t auiCount);
		bool AddGlyph(const VectorEncodedGlyph& arGlyph, const NiPoint3& arPen, const NiColorA* apColor);
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
