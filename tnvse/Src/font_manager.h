#pragma once
#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	class FontManagerEx : public FontManager
	{
	public:
		// outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
		NiPoint3* __thiscall CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex);
		UInt32* PrepText(BSStringT<char>* a7, int a3);
	};

} // namespace fonthook
