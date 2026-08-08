#pragma once

namespace fonthook::compatibility
{
	// Detect the live Tianmiao font-patch injection before tNVSE installs any
	// game hooks.  On conflict this displays the recovery instructions and
	// terminates the game process; otherwise it returns false.
	bool BlockTianmiaoFontPatchIfPresent(const char* phase);
}
