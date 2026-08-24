#include "text_safety.h"

#include <iostream>
#include <string>
#include <string_view>

namespace
{
	using fonthook::text_safety::CopyChoice;
	using fonthook::text_safety::CopyStatus;
	using fonthook::text_safety::GameVariableToken;

	int s_failures = 0;

	void Expect(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL " << message << '\n';
			++s_failures;
		}
	}

	void TestRetailReplacementArgumentBoundary()
	{
		using fonthook::text_safety::IsRetailReplacementArgumentSafe;
		using fonthook::text_safety::kRetailReplacementNameMaxBytes;

		Expect(!IsRetailReplacementArgumentSafe(nullptr),
			"null replacement argument accepted");
		Expect(!IsRetailReplacementArgumentSafe(""),
			"empty replacement argument accepted");
		Expect(!IsRetailReplacementArgumentSafe("&;"),
			"empty entity accepted");
		Expect(IsRetailReplacementArgumentSafe("PCName"),
			"ordinary replacement name rejected");
		Expect(IsRetailReplacementArgumentSafe("&-PCName;"),
			"negative full entity rejected");
		Expect(!IsRetailReplacementArgumentSafe("&PCName;tail"),
			"trailing entity bytes accepted");
		Expect(!IsRetailReplacementArgumentSafe("PC\nName"),
			"line-spanning replacement name accepted");

		const std::string maxName(kRetailReplacementNameMaxBytes, 'A');
		const std::string overName(kRetailReplacementNameMaxBytes + 1, 'A');
		Expect(IsRetailReplacementArgumentSafe(maxName.c_str()),
			"127-byte replacement name rejected");
		Expect(!IsRetailReplacementArgumentSafe(overName.c_str()),
			"128-byte replacement name accepted");
		Expect(IsRetailReplacementArgumentSafe(
			("&" + maxName + ";").c_str()),
			"127-byte full entity rejected");
		Expect(!IsRetailReplacementArgumentSafe(
			("&" + overName + ";").c_str()),
			"128-byte full entity accepted");

		std::string korean127;
		for (size_t index = 0; index < 63; ++index)
		{
			korean127.push_back(static_cast<char>(0xB0));
			korean127.push_back(static_cast<char>(0xA1));
		}
		korean127.push_back('A');
		std::string korean128 = korean127;
		korean128.push_back('B');
		Expect(IsRetailReplacementArgumentSafe(korean127.c_str()),
			"127-byte multibyte replacement name rejected");
		Expect(!IsRetailReplacementArgumentSafe(korean128.c_str()),
			"128-byte multibyte replacement name accepted");
	}

	void TestPreResolveTokenBoundary()
	{
		using fonthook::text_safety::TryParseGameVariableToken;
		using fonthook::text_safety::kRetailReplacementNameMaxBytes;

		GameVariableToken token;
		const std::string ordinary = "before &PCName; after";
		const size_t ampersand = ordinary.find('&');
		Expect(TryParseGameVariableToken(ordinary, ampersand, token),
			"ordinary terminated token rejected");
		Expect(ordinary.substr(token.nameBegin, token.nameLength) == "PCName"
			&& token.nextIndex == ordinary.find(';') + 1
			&& token.isPositive,
			"ordinary token boundaries changed");

		const std::string negative = "&-Caps\nnext";
		Expect(TryParseGameVariableToken(negative, 0, token)
			&& negative.substr(token.nameBegin, token.nameLength) == "Caps"
			&& token.nextIndex == negative.find('\n')
			&& !token.isPositive,
			"negative line-terminated token boundaries changed");

		const std::string maxToken = "&"
			+ std::string(kRetailReplacementNameMaxBytes, 'V') + ";";
		const std::string overToken = "&"
			+ std::string(kRetailReplacementNameMaxBytes + 1, 'V') + ";";
		Expect(TryParseGameVariableToken(maxToken, 0, token),
			"127-byte PreResolve token rejected");
		Expect(!TryParseGameVariableToken(overToken, 0, token),
			"128-byte PreResolve token accepted");
		Expect(!TryParseGameVariableToken("&PCName", 0, token),
			"unterminated PreResolve token accepted");
		Expect(!TryParseGameVariableToken("&;", 0, token),
			"empty PreResolve token accepted");

		std::string anchorage = "warning *&$Jk9";
		anchorage += std::string(384, 'X');
		anchorage += "\nnext line";
		Expect(!TryParseGameVariableToken(
			anchorage, anchorage.find('&'), token),
			"Anchorage 389-byte literal run reached retail replacement code");

		std::string embeddedNull("&Name\0;", 7);
		Expect(!TryParseGameVariableToken(embeddedNull, 0, token),
			"embedded-NUL token accepted");
	}

	void TestFixedBufferCopyAndFallback()
	{
		using namespace fonthook::text_safety;

		char exact[5] = {};
		Expect(CopyTextIfFits(exact, sizeof(exact), "four")
			== CopyStatus::Copied && std::string_view(exact) == "four",
			"exact fixed-buffer copy failed");

		char tooSmall[4] = { 'x', 'x', 'x', '\0' };
		Expect(CopyTextIfFits(tooSmall, sizeof(tooSmall), "four")
			== CopyStatus::InsufficientCapacity && tooSmall[0] == '\0',
			"oversized fixed-buffer copy did not fail closed");

		char uiBuffer[kRetailUiTextCapacity] = {};
		const std::string longTranslation(kRetailUiTextCapacity, 'T');
		Expect(CopyPreferredTextWithFallback(
			uiBuffer, sizeof(uiBuffer), longTranslation, "vanilla")
			== CopyChoice::Fallback
			&& std::string_view(uiBuffer) == "vanilla",
			"oversized translation did not fall back to vanilla text");

		const std::string exactTranslation(kRetailUiTextCapacity - 1, 'T');
		Expect(CopyPreferredTextWithFallback(
			uiBuffer, sizeof(uiBuffer), exactTranslation, "vanilla")
			== CopyChoice::Preferred
			&& std::string_view(uiBuffer).size() == exactTranslation.size(),
			"259-byte translated UI text rejected");

		const std::string longFallback(kRetailUiTextCapacity, 'F');
		Expect(CopyPreferredTextWithFallback(
			uiBuffer, sizeof(uiBuffer), longTranslation, longFallback)
			== CopyChoice::None && uiBuffer[0] == '\0',
			"double-oversized UI candidates did not fail closed");

		const std::string embedded("ab\0cd", 5);
		Expect(CopyTextIfFits(uiBuffer, sizeof(uiBuffer), embedded)
			== CopyStatus::InsufficientCapacity && uiBuffer[0] == '\0',
			"embedded-NUL UI text accepted");
	}
}

int main()
{
	TestRetailReplacementArgumentBoundary();
	TestPreResolveTokenBoundary();
	TestFixedBufferCopyAndFallback();

	if (s_failures != 0)
	{
		std::cerr << s_failures << " text safety test(s) failed\n";
		return 1;
	}
	std::cout << "text safety tests passed\n";
	return 0;
}
