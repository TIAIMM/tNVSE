#pragma once

#include <string>

namespace fonthook
{
	void LoadDictionaryConfig();
	bool TranslateText(const char* source, std::string& translated);
	bool TranslateRichText(const char* source, std::string& translated);
}
