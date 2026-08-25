#include "dictionary_translation_guard.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
	int s_failures = 0;

	void Expect(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL " << message << '\n';
			++s_failures;
		}
	}
}

int main()
{
	using namespace fonthook::dictionary_translation_guard;

	static_assert(kRetailConsoleOpenStateAddress == 0x11DEA2E);

	volatile std::uint8_t consoleOpenState = 0;
	Expect(!IsConsoleOpen(nullptr), "null console state reported open");
	Expect(!IsConsoleOpen(&consoleOpenState), "closed console reported open");
	consoleOpenState = 1;
	Expect(IsConsoleOpen(&consoleOpenState), "open console reported closed");
	consoleOpenState = 0xFF;
	Expect(IsConsoleOpen(&consoleOpenState), "nonzero console state reported closed");

	Expect(!ShouldBypassDictionaryTranslation(true, false, true, false),
		"ordinary enabled translation was bypassed");
	Expect(ShouldBypassDictionaryTranslation(false, false, true, false),
		"disabled dictionary was not bypassed");
	Expect(ShouldBypassDictionaryTranslation(true, true, true, false),
		"tile-scoped suppression was ignored");
	Expect(ShouldBypassDictionaryTranslation(true, false, true, true),
		"open-console suppression was ignored");
	Expect(!ShouldBypassDictionaryTranslation(true, false, false, true),
		"disabled console suppression still bypassed translation");
	Expect(ShouldBypassDictionaryTranslation(false, true, false, true),
		"combined suppression state was ignored");

	if (s_failures != 0)
	{
		std::cerr << s_failures
			<< " dictionary translation guard test(s) failed\n";
		return 1;
	}

	std::cout << "dictionary translation guard tests passed\n";
	return 0;
}
