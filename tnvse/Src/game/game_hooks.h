#pragma once

#include "ui_decode.h"

namespace fonthook
{
	struct FontHookInstallState
	{
		bool multibyte = false;
		bool freeType = false;
	};

	// ---- Initialization functions ----
	void InitBigGunsDescHooks();
	void InitDoorPromptHooksCHS();
	void InitDoorPromptHooksKOR();
	void InitPluralHooks();
	FontHookInstallState InitFontHooks();
	bool AreMultibyteFontHooksInstalled();
	bool AreFreeTypeFontHooksInstalled();
	bool IsPrewarmOverlayMakeNodeRouteInstalled();

	Font* CallOriginalFontConstructor(
		Font* font, int fontNum, char* filename, bool load);
	void CallOriginalFontLoad(Font* font);
	void CallOriginalFontCreateText(
		Font* font, BSStringT<char>* text, int* width, int* height,
		int lineStart, int lineEnd, int flags, char lineBreak,
		const NiColorA* color, NiTriShape** textShape, NiTriShape** iconShape);
	NiAVObject* CallOriginalFontMakeString(
		Font* font, float startX, float startY, float z,
		BSStringT<char>* text, int* width, UInt32 flags,
		const NiColorA* color, bool upperLeftCorner, bool prepareObjectFinal);
	NiPoint3* CallOriginalCalculateStringDimensions(
		FontManager* manager, NiPoint3* dimensions, const char* text,
		UInt32 fontId, float maxWrapWidth, UInt32 startCharIndex);

} // namespace fonthook
