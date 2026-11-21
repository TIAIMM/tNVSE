#pragma once

#include "encoding.h"

#include <cstring>

namespace fonthook::save_level_tile_fix
{
	// The menu may append a UTF-8 title from a save header to a CP949 level
	// label. Repair only the level row's known shape at the tile boundary.
	inline bool TryNormalize(const char* raw, std::string& corrected)
	{
		constexpr size_t maxLength = 512;
		if (!raw)
			return false;
		const size_t length = strnlen_s(raw, maxLength + 1);
		if (length > maxLength)
			return false;

		constexpr std::string_view levelCp949 = "\xB7\xB9\xBA\xA7 ";
		constexpr std::string_view levelUtf8 = "\xEB\xA0\x88\xEB\xB2\xA8 ";
		constexpr std::string_view levelAscii = "Level ";
		const std::string_view text(raw, length);
		const bool fullUtf8 = IsValidUTF8With3ByteMin(raw);
		size_t numberStart;
		if (!fullUtf8 && text.starts_with(levelCp949))
			numberStart = levelCp949.size();
		else if (fullUtf8 && text.starts_with(levelUtf8))
			numberStart = levelUtf8.size();
		else if (fullUtf8 && text.starts_with(levelAscii))
			numberStart = levelAscii.size();
		else
			return false;

		const size_t colon = text.find_first_not_of("0123456789", numberStart);
		if (colon == std::string_view::npos || colon == numberStart ||
			colon - numberStart > 4 || text.substr(colon, 2) != ": ")
			return false;

		const std::string_view title = text.substr(colon + 2);
		bool hasHangul = false;
		for (size_t i = 0; i + 2 < title.size(); ++i)
		{
			const auto lead = static_cast<UInt8>(title[i]);
			const auto middle = static_cast<UInt8>(title[i + 1]);
			const auto trail = static_cast<UInt8>(title[i + 2]);
			if ((lead == 0xEA && middle >= 0xB0) || (lead >= 0xEB && lead <= 0xEC) ||
				(lead == 0xED && (middle < 0x9E || (middle == 0x9E && trail <= 0xA3))))
			{
				hasHangul = true;
				break;
			}
		}
		if (!hasHangul)
			return false;

		std::string converted;
		if (!TryUTF8ToMultiByteStr(fullUtf8 ? text : title,
			static_cast<UInt32>(CodePage::UHC), converted))
			return false;
		if (!fullUtf8)
			converted.insert(0, text.substr(0, colon + 2));
		if (converted == text)
			return false;
		corrected.swap(converted);
		return true;
	}
}
