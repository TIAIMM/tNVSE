#pragma once

#include "ui_decode.h"

#include <memory>

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
		UInt8 byteLength = 0;
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		FontLetter* metrics = nullptr;
	};

	void LoadFreeTypeFontConfig();
	void FreeTypeFontDebugLog(const char* apFormat, ...);
	void FlushFreeTypeFontDebugLog();
	void FinalizeFreeTypeUioDetection();
	float ConsumeFreeTypeCreateTextScale();
	void FreeTypeCreateTextEntryHook();
	bool InitializeFreeTypeVectorRenderer();
	bool ActivateFreeTypeFont(Font* apFont, bool abForce = false);
	bool IsFreeTypeFontActive(const Font* apFont);
	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 auiEncodedCode);
	bool DecodeFreeTypeGlyph(Font* apFont, const char* apText, VectorEncodedGlyph& arGlyph);
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
