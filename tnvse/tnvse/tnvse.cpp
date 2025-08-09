#include "tnvse.h"
#include "SafeWrite.h"

namespace tNVSE {

	static double __stdcall VertSpacingAdjust(int32_t fontID) {
		return 0;
	}

	void InitVertSpacingHook() {
		ReplaceCall(0xA128E2, VertSpacingAdjust);
		ReplaceCall(0xA13028, VertSpacingAdjust);
		ReplaceCall(0xA1B0AF, VertSpacingAdjust);
	}
}