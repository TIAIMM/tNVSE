#pragma once
#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	inline constexpr UInt32 kVanillaGameFontCount = FontManager::kVanillaFontCount;
	inline constexpr UInt32 kFirstJipExtendedFontId = 10;
	inline constexpr UInt32 kLastJipExtendedFontId = 89;
	inline constexpr UInt32 kJipExtendedFontCount = FontManager::kJipExtendedFontCount;
	inline constexpr UInt32 kAddressableGameFontCount =
		kVanillaGameFontCount + kJipExtendedFontCount;
	// TextDoc::Render and TextPage::pCharsPerFont are fixed retail ABI arrays.
	// This is not the size of the JIP font registry.
	inline constexpr UInt32 kVanillaRichTextFontCount =
		FontManager::kVanillaRichTextFontCount;
	static_assert(kJipExtendedFontCount
		== kLastJipExtendedFontId - kFirstJipExtendedFontId + 1);

	bool HasJipExtendedFontManager();
	bool IsGameFontIdAddressable(UInt32 auiFontId);
	bool TryResolveGameFontId(float afFontTrait, UInt32& arFontId);
	Font* ResolveGameFont(FontManager* apManager, UInt32 auiFontId);

	struct RichTextCharExtra
	{
		UInt32 dbcsCode;
		const FontManager::TextDoc* textDoc;
	};

	void SetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32 auiDbcsCode, const FontManager::TextDoc* apDoc = nullptr);
	bool TryGetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32& arDbcsCode);
	void ClearRichTextCharExtra(const FontManager::CharData* apChar);

	struct RichTextRenderAddCharInfo
	{
		const FontManager::TextDoc* textDoc;
		const FontManager::CharData* charData;
		UInt32 callIndex;
		UInt32 expectedAddCharCount;
	};

	void BeginRichTextRenderContext(FontManager::TextDoc* apDoc, FontManager::TextData* apData);
	void EndRichTextRenderContext(FontManager::TextDoc* apDoc);
	bool TryConsumeRichTextRenderAddChar(RichTextRenderAddCharInfo& arInfo);

	class FontManagerEx : public FontManager
	{
	public:
		// outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
		NiPoint3* __thiscall CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex);
		static BSStringT<char>* __fastcall CollectTo(FontManager* apManager, void*, BSStringT<char>* apOutString, const char* apSource, UInt32* apIndex, UInt32 aiStopMask, UInt32 aiRequiredMask, UInt32* apOutType, char* apOutChar, bool abUseReplacements);
		static BSStringT<char>* __fastcall CollectToAttributeValue(FontManager* apManager, void*, BSStringT<char>* apOutString, const char* apSource, UInt32* apIndex, UInt32 aiStopMask, UInt32 aiRequiredMask, UInt32* apOutType, char* apOutChar, bool abUseReplacements);
		TextDoc* __thiscall PrepHypertext(BSStringT<char>& arTextString, TextData& arData);
		TextDoc* __thiscall PrepText(BSStringT<char>& arTextString, TextData& arData);
		void __thiscall TextDocRender(NiNode* apNode, TextData* apData);
		void __thiscall TextDocDestroy();
		void __thiscall TextDocAddChar(CharData* apChar, int aiNewLines, bool abNewPage);
		TextPage* __thiscall TextPageAddChar(CharData* apChar, int aiNewLines);
		TextLine* __thiscall TextLineAddChar(CharData* apChar, bool abAddHead);
		static CharData* __fastcall CharDataCopy(CharData* apChar, void*);
	};

} // namespace fonthook
