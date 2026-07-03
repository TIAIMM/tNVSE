#pragma once
#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	struct RichTextCharExtra
	{
		UInt32 dbcsCode;
	};

	void SetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32 auiDbcsCode);
	bool TryGetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32& arDbcsCode);
	void ClearRichTextCharExtra(const FontManager::CharData* apChar);
	void ClearRichTextCharExtras();

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
