#pragma once

#include <cstdint>
#include <vector>

#include <ft2build.h>
#include FT_OUTLINE_H

namespace fonthook::vectorfont
{
	struct MsdfgenSdfBitmap
	{
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		std::vector<std::uint8_t> pixels;
	};

	bool GenerateMsdfgenTrueSdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenSdfBitmap& output);
}
