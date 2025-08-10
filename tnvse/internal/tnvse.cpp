#pragma once
#include "tnvse.h"
#include "SafeWrite.h"
#include "Game/netimmerse.h"

namespace tNVSE {

	static Float32 __stdcall VertSpacingAdjust(UInt32 fontID) {
		return 0;
	}

	class FontInfoEx : public FontInfo {
	public:
		void __thiscall LoadFontDataEx() {

		}
	};

	void InitVertSpacingHook() {
		ReplaceCall(0xA128E2, VertSpacingAdjust);
		ReplaceCall(0xA13028, VertSpacingAdjust);
		ReplaceCall(0xA1B0AF, VertSpacingAdjust);
	}
}