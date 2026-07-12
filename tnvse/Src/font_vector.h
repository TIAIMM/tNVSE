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
		std::vector<FreeTypeLayoutGlyph> glyphs;
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
	void UpdateFreeTypeDevicePixelScale();
	bool TryGetFreeTypeDevicePixelScale(float& arScale);
	float ResolveFreeTypeRasterScale(float afLocalScale = 1.0f);
	float ConsumeFreeTypeCreateTextScale();
	void FreeTypeCreateTextEntryHook();
	bool InitializeFreeTypeVectorRenderer();
	bool ActivateFreeTypeFont(Font* apFont, bool abForce = false);
	bool IsFreeTypeFontActive(const Font* apFont);
	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 auiEncodedCode);
	bool DecodeFreeTypeGlyph(Font* apFont, const char* apText, VectorEncodedGlyph& arGlyph);
	bool LayoutFreeTypeRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping = true);
	bool GetFreeTypePairKerning(Font* apFont,
		const char* apLeft, size_t auiLeftLength,
		const char* apRight, size_t auiRightLength, float& arKerning);
	bool IsHarfBuzzShapingEnabled(const Font* apFont);
	NiTriShape* CreateEmptyFreeTypeTextShape(Font* apFont, bool abPrepareObject);

	class VectorTextBuilder
	{
	public:
		VectorTextBuilder(Font* apFont, bool abPrepareObject, float afRasterScale = 1.0f);
		~VectorTextBuilder();

		VectorTextBuilder(const VectorTextBuilder&) = delete;
		VectorTextBuilder& operator=(const VectorTextBuilder&) = delete;

		bool IsAvailable() const;
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
