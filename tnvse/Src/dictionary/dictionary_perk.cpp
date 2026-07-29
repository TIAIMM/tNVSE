#include "dictionary_internal.h"
#include "load_config.h"

#include <utility>

namespace fonthook
{
	namespace implementation::dictionary_perk {}
	using namespace implementation::dictionary_perk;

	namespace implementation::dictionary_perk
	{
		struct PerkDescriptionParts
		{
			std::string requirement;
			std::string ranks;
			std::string source;
			std::string body;
			bool hasRanks = false;
			bool hasSource = false;
			bool hasBody = false;
		};

		struct PerkRequirementConfig
		{
			std::string reqText;
			std::string ranksText;
			std::string sourceText;
			std::string levelSuffix;
			std::string andText;
			std::string orText;
			std::string notText;
		};

		PerkRequirementConfig s_perkRequirementConfig;

		bool IsAsciiWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
		}

		bool IsAsciiDigit(char ch)
		{
			return ch >= '0' && ch <= '9';
		}

		bool IsAsciiAlpha(char ch)
		{
			return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
		}

		bool IsAsciiAlnum(char ch)
		{
			return IsAsciiAlpha(ch) || IsAsciiDigit(ch);
		}

		bool ContainsAsciiAlpha(std::string_view text)
		{
			for (char ch : text)
			{
				if (IsAsciiAlpha(ch))
					return true;
			}
			return false;
		}

		char ToLowerAsciiChar(char ch)
		{
			if (ch >= 'A' && ch <= 'Z')
				return static_cast<char>(ch + ('a' - 'A'));
			return ch;
		}

		bool EqualsIgnoreCase(std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
				return false;
			for (size_t i = 0; i < left.size(); ++i)
			{
				if (ToLowerAsciiChar(left[i]) != ToLowerAsciiChar(right[i]))
					return false;
			}
			return true;
		}

		bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
		{
			return text.size() >= prefix.size() &&
				EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
		}

		bool IsStandaloneWordAt(const std::string& text, size_t pos, const char* word)
		{
			const size_t length = std::char_traits<char>::length(word);
			if (pos + length > text.size() || !EqualsIgnoreCase(std::string_view(text).substr(pos, length), word))
				return false;

			const bool leftOk = pos == 0 || !IsAsciiAlnum(text[pos - 1]);
			const bool rightOk = pos + length == text.size() || !IsAsciiAlnum(text[pos + length]);
			return leftOk && rightOk;
		}

		void TrimAsciiWhitespace(std::string& text)
		{
			size_t first = 0;
			while (first < text.size() && IsAsciiWhitespace(text[first]))
				++first;
			size_t last = text.size();
			while (last > first && IsAsciiWhitespace(text[last - 1]))
				--last;
			text = text.substr(first, last - first);
		}

		std::string TrimAsciiWhitespaceCopy(std::string text)
		{
			TrimAsciiWhitespace(text);
			return text;
		}

		void CollapseAsciiWhitespace(std::string& text)
		{
			std::string result;
			result.reserve(text.size());
			bool pendingSpace = false;
			for (char ch : text)
			{
				if (IsAsciiWhitespace(ch))
				{
					pendingSpace = true;
					continue;
				}

				if (pendingSpace && !result.empty())
					result.push_back(' ');
				result.push_back(ch);
				pendingSpace = false;
			}
			text.swap(result);
		}

		void SkipAsciiWhitespace(const std::string& text, size_t& pos)
		{
			while (pos < text.size() && IsAsciiWhitespace(text[pos]))
				++pos;
		}

		size_t FindLineBreak(const std::string& text, size_t pos)
		{
			while (pos < text.size())
			{
				if (text[pos] == '\r' || text[pos] == '\n')
					return pos;
				++pos;
			}
			return std::string::npos;
		}

		size_t SkipLineBreak(const std::string& text, size_t pos)
		{
			if (pos < text.size() && text[pos] == '\r')
			{
				++pos;
				if (pos < text.size() && text[pos] == '\n')
					++pos;
				return pos;
			}
			if (pos < text.size() && text[pos] == '\n')
				++pos;
			return pos;
		}

		size_t FindKeyword(const std::string& text, const char* keyword, size_t pos)
		{
			const size_t length = std::char_traits<char>::length(keyword);
			while (pos < text.size())
			{
				const size_t found = text.find(keyword, pos);
				if (found == std::string::npos)
					return std::string::npos;
				if (found == 0 || IsAsciiWhitespace(text[found - 1]))
					return found;
				pos = found + length;
			}
			return std::string::npos;
		}

		size_t MinPosition(size_t left, size_t right)
		{
			if (left == std::string::npos)
				return right;
			if (right == std::string::npos)
				return left;
			return left < right ? left : right;
		}

		size_t ParseRanksEnd(const std::string& text, size_t pos)
		{
			size_t cursor = pos;
			size_t lastDigitEnd = pos;
			bool sawDigit = false;
			while (cursor < text.size())
			{
				const size_t beforeWhitespace = cursor;
				SkipAsciiWhitespace(text, cursor);
				if (cursor >= text.size() || !IsAsciiDigit(text[cursor]))
					return sawDigit ? lastDigitEnd : beforeWhitespace;

				while (cursor < text.size() && IsAsciiDigit(text[cursor]))
					++cursor;
				lastDigitEnd = cursor;
				sawDigit = true;
			}
			return lastDigitEnd;
		}

		size_t FindSourceExtensionEnd(const std::string& text, size_t pos)
		{
			for (size_t i = pos; i + 4 <= text.size(); ++i)
			{
				if (text[i] != '.')
					continue;

				const std::string_view ext(text.c_str() + i, 4);
				if (EqualsIgnoreCase(ext, ".esm") || EqualsIgnoreCase(ext, ".esp"))
					return i + 4;
			}
			return std::string::npos;
		}

		bool ParsePerkDescription(const std::string& source, PerkDescriptionParts& parts)
		{
			parts = PerkDescriptionParts{};
			std::string text = source;
			TrimAsciiWhitespace(text);
			if (!StartsWithIgnoreCase(text, "Req:"))
				return false;

			size_t cursor = 4;
			SkipAsciiWhitespace(text, cursor);

			const size_t ranksPos = FindKeyword(text, "Ranks:", cursor);
			const size_t sourcePos = FindKeyword(text, "Source:", cursor);
			const size_t firstKeyword = MinPosition(ranksPos, sourcePos);
			if (firstKeyword == std::string::npos)
			{
				const size_t lineBreak = FindLineBreak(text, cursor);
				if (lineBreak == std::string::npos)
				{
					parts.requirement = TrimAsciiWhitespaceCopy(text.substr(cursor));
				}
				else
				{
					parts.requirement = TrimAsciiWhitespaceCopy(text.substr(cursor, lineBreak - cursor));
					size_t bodyBegin = SkipLineBreak(text, lineBreak);
					SkipAsciiWhitespace(text, bodyBegin);
					parts.body = TrimAsciiWhitespaceCopy(text.substr(bodyBegin));
					parts.hasBody = !parts.body.empty();
				}
				return !parts.requirement.empty();
			}

			parts.requirement = TrimAsciiWhitespaceCopy(text.substr(cursor, firstKeyword - cursor));
			if (parts.requirement.empty())
				return false;

			cursor = firstKeyword;
			if (cursor == ranksPos)
			{
				cursor += 6;
				SkipAsciiWhitespace(text, cursor);

				const size_t sourceAfterRanks = FindKeyword(text, "Source:", cursor);
				if (sourceAfterRanks != std::string::npos)
				{
					parts.ranks = TrimAsciiWhitespaceCopy(text.substr(cursor, sourceAfterRanks - cursor));
					cursor = sourceAfterRanks;
				}
				else
				{
					const size_t ranksEnd = ParseRanksEnd(text, cursor);
					parts.ranks = TrimAsciiWhitespaceCopy(text.substr(cursor, ranksEnd - cursor));
					size_t bodyBegin = ranksEnd;
					SkipAsciiWhitespace(text, bodyBegin);
					parts.body = TrimAsciiWhitespaceCopy(text.substr(bodyBegin));
					parts.hasBody = !parts.body.empty();
					parts.hasRanks = !parts.ranks.empty();
					return parts.hasRanks;
				}

				parts.hasRanks = !parts.ranks.empty();
				if (!parts.hasRanks)
					return false;
			}

			if (cursor == sourcePos || (cursor < text.size() && text.compare(cursor, 7, "Source:") == 0))
			{
				cursor += 7;
				SkipAsciiWhitespace(text, cursor);

				const size_t sourceEnd = FindSourceExtensionEnd(text, cursor);
				if (sourceEnd == std::string::npos)
					return false;

				parts.source = TrimAsciiWhitespaceCopy(text.substr(cursor, sourceEnd - cursor));
				size_t bodyBegin = sourceEnd;
				SkipAsciiWhitespace(text, bodyBegin);
				parts.body = TrimAsciiWhitespaceCopy(text.substr(bodyBegin));
				parts.hasSource = !parts.source.empty();
				parts.hasBody = !parts.body.empty();
				return parts.hasSource;
			}

			return true;
		}

		const char* DefaultReqText()
		{
			return "\xE9\x9C\x80\xE6\xB1\x82";
		}

		const char* DefaultRanksText()
		{
			return "\xE6\x8A\x80\xE8\x83\xBD\xE7\xBA\xA7\xE5\x88\xAB";
		}

		const char* DefaultSourceText()
		{
			return "\xE6\x9D\xA5\xE6\xBA\x90";
		}

		const char* DefaultLevelSuffix()
		{
			return "\xE7\xBA\xA7";
		}

		const char* DefaultAndText()
		{
			return "\xE4\xB8\x94";
		}

		const char* DefaultOrText()
		{
			return "\xE6\x88\x96";
		}

		const char* DefaultNotText()
		{
			return "\xE6\xB2\xA1\xE6\x9C\x89";
		}

		std::string FindChildText(pugi::xml_node parent, const char* nodeName)
		{
			pugi::xml_node node = parent.child(nodeName);
			if (!node)
				return {};

			std::string text = node.text().as_string("");
			TrimAsciiWhitespace(text);
			return text;
		}

		std::string LoadPerkConfigText(pugi::xml_node perkNode, const char* nodeName, const char* defaultText)
		{
			std::string text = perkNode ? FindChildText(perkNode, nodeName) : std::string{};
			if (text.empty())
				text = defaultText;
			return PrepareTarget(std::move(text));
		}

		const char* TranslateSpecialNameDefault(const std::string& name)
		{
			if (EqualsIgnoreCase(name, "STR"))
				return "\xE5\x8A\x9B\xE9\x87\x8F";
			if (EqualsIgnoreCase(name, "PER"))
				return "\xE6\x84\x9F\xE7\x9F\xA5";
			if (EqualsIgnoreCase(name, "END"))
				return "\xE8\x80\x90\xE5\x8A\x9B";
			if (EqualsIgnoreCase(name, "CHR"))
				return "\xE9\xAD\x85\xE5\x8A\x9B";
			if (EqualsIgnoreCase(name, "INT"))
				return "\xE6\x99\xBA\xE5\x8A\x9B";
			if (EqualsIgnoreCase(name, "AGL"))
				return "\xE6\x95\x8F\xE6\x8D\xB7";
			if (EqualsIgnoreCase(name, "LCK"))
				return "\xE8\xBF\x90\xE6\xB0\x94";
			return nullptr;
		}

		std::string TranslateNameExactOrSpecial(const std::string& name, int depth)
		{
			std::string translated;
			if (depth < 4 && TryTranslateExactText(name, translated, depth + 1))
				return translated;

			if (const char* special = TranslateSpecialNameDefault(name))
				return PrepareTarget(special);

			return name;
		}

		bool TrySplitTrailingValue(const std::string& text, std::string& name, std::string& suffix)
		{
			size_t end = text.size();
			while (end > 0 && IsAsciiWhitespace(text[end - 1]))
				--end;
			if (end == 0)
				return false;

			size_t valueBegin = end;
			if (valueBegin > 0 && text[valueBegin - 1] == '%')
				--valueBegin;
			while (valueBegin > 0 && (IsAsciiDigit(text[valueBegin - 1]) || text[valueBegin - 1] == '.'))
				--valueBegin;
			if (valueBegin == end || (valueBegin + 1 == end && text[valueBegin] == '%'))
				return false;

			size_t suffixBegin = valueBegin;
			while (suffixBegin > 0 && IsAsciiWhitespace(text[suffixBegin - 1]))
				--suffixBegin;
			if (suffixBegin > 0)
			{
				size_t opBegin = suffixBegin;
				const char op = text[opBegin - 1];
				if (op == '<' || op == '>' || op == '=' || op == '+' || op == '-')
				{
					--opBegin;
					if (opBegin > 0 && text[opBegin - 1] == '=')
						--opBegin;
					while (opBegin > 0 && IsAsciiWhitespace(text[opBegin - 1]))
						--opBegin;
					suffixBegin = opBegin;
				}
			}

			name = TrimAsciiWhitespaceCopy(text.substr(0, suffixBegin));
			suffix = text.substr(suffixBegin, end - suffixBegin);
			return !name.empty() && !suffix.empty();
		}

		std::string TranslateAtomicRequirement(std::string text, int depth)
		{
			TrimAsciiWhitespace(text);
			CollapseAsciiWhitespace(text);
			if (text.empty())
				return {};

			if (StartsWithIgnoreCase(text, "NOT") &&
				(text.size() == 3 || IsAsciiWhitespace(text[3])))
			{
				std::string operand = TrimAsciiWhitespaceCopy(text.substr(3));
				const std::string translatedOperand = TranslateAtomicRequirement(std::move(operand), depth);
				if (s_perkRequirementConfig.notText.empty())
					return translatedOperand;
				if (translatedOperand.empty())
					return s_perkRequirementConfig.notText;
				return s_perkRequirementConfig.notText + " " + translatedOperand;
			}

			if (StartsWithIgnoreCase(text, "Level") &&
				(text.size() == 5 || IsAsciiWhitespace(text[5])))
			{
				std::string value = TrimAsciiWhitespaceCopy(text.substr(5));
				if (!value.empty() && !ContainsAsciiAlpha(value))
					return value + s_perkRequirementConfig.levelSuffix;
			}

			std::string exact;
			if (depth < 4 && TryTranslateExactText(text, exact, depth + 1))
				return exact;

			std::string name;
			std::string suffix;
			if (TrySplitTrailingValue(text, name, suffix))
				return TranslateNameExactOrSpecial(name, depth) + suffix;

			return TranslateNameExactOrSpecial(text, depth);
		}

		bool FindNextLogicOperator(const std::string& text, size_t start, size_t& pos, const char*& replacement, size_t& length)
		{
			for (size_t i = start; i < text.size(); ++i)
			{
				if (IsStandaloneWordAt(text, i, "AND"))
				{
					pos = i;
					replacement = s_perkRequirementConfig.andText.c_str();
					length = 3;
					return true;
				}
				if (IsStandaloneWordAt(text, i, "OR"))
				{
					pos = i;
					replacement = s_perkRequirementConfig.orText.c_str();
					length = 2;
					return true;
				}
			}
			return false;
		}

		std::string TranslateRequirementClause(const std::string& clause, int depth)
		{
			std::string result;
			size_t cursor = 0;
			while (cursor < clause.size())
			{
				size_t opPos = std::string::npos;
				const char* opReplacement = nullptr;
				size_t opLength = 0;
				if (!FindNextLogicOperator(clause, cursor, opPos, opReplacement, opLength))
					break;

				std::string left = clause.substr(cursor, opPos - cursor);
				TrimAsciiWhitespace(left);
				if (!left.empty())
				{
					if (!result.empty())
						result.push_back(' ');
					result += TranslateAtomicRequirement(std::move(left), depth);
				}

				if (opReplacement && *opReplacement)
				{
					if (!result.empty())
						result.push_back(' ');
					result += opReplacement;
				}
				cursor = opPos + opLength;
			}

			std::string tail = clause.substr(cursor);
			TrimAsciiWhitespace(tail);
			if (!tail.empty())
			{
				if (!result.empty())
					result.push_back(' ');
				result += TranslateAtomicRequirement(std::move(tail), depth);
			}
			return result;
		}

		std::vector<std::string> SplitRequirementClauses(const std::string& text)
		{
			std::vector<std::string> clauses;
			size_t cursor = 0;
			while (cursor <= text.size())
			{
				const size_t comma = text.find(',', cursor);
				std::string clause = comma == std::string::npos
					? text.substr(cursor)
					: text.substr(cursor, comma - cursor);
				TrimAsciiWhitespace(clause);
				if (!clause.empty())
					clauses.push_back(std::move(clause));
				if (comma == std::string::npos)
					break;
				cursor = comma + 1;
			}
			return clauses;
		}

		std::string TranslateRequirementText(std::string text, int depth)
		{
			TrimAsciiWhitespace(text);
			CollapseAsciiWhitespace(text);

			const std::vector<std::string> clauses = SplitRequirementClauses(text);
			std::string result;
			for (const std::string& clause : clauses)
			{
				std::string translatedClause = TranslateRequirementClause(clause, depth);
				if (translatedClause.empty())
					continue;
				if (!result.empty())
					result += ", ";
				result += translatedClause;
			}
			return result;
		}

		std::string TranslateBodyExactOrSelf(const std::string& body, int depth)
		{
			std::string translated;
			if (depth < 4 && TryTranslateExactText(body, translated, depth + 1))
				return translated;
			return body;
		}

		void AppendPerkLine(std::string& result, const std::string& label, const std::string& value)
		{
			if (!result.empty())
				result += "\n\n";
			result += label;
			result += ": ";
			result += value;
		}
	}

	void ResetPerkRequirementConfig()
	{
		s_perkRequirementConfig.reqText = PrepareTarget(DefaultReqText());
		s_perkRequirementConfig.ranksText = PrepareTarget(DefaultRanksText());
		s_perkRequirementConfig.sourceText = PrepareTarget(DefaultSourceText());
		s_perkRequirementConfig.levelSuffix = PrepareTarget(DefaultLevelSuffix());
		s_perkRequirementConfig.andText = PrepareTarget(DefaultAndText());
		s_perkRequirementConfig.orText = PrepareTarget(DefaultOrText());
		s_perkRequirementConfig.notText = PrepareTarget(DefaultNotText());
	}

	void LoadPerkRequirementConfig(pugi::xml_node root)
	{
		pugi::xml_node perkNode = root.child("perkrequirements");
		s_perkRequirementConfig.reqText = LoadPerkConfigText(perkNode, "req", DefaultReqText());
		s_perkRequirementConfig.ranksText = LoadPerkConfigText(perkNode, "ranks", DefaultRanksText());
		s_perkRequirementConfig.sourceText = LoadPerkConfigText(perkNode, "source", DefaultSourceText());
		s_perkRequirementConfig.levelSuffix = LoadPerkConfigText(perkNode, "levelSuffix", DefaultLevelSuffix());
		s_perkRequirementConfig.andText = LoadPerkConfigText(perkNode, "and", DefaultAndText());
		s_perkRequirementConfig.orText = LoadPerkConfigText(perkNode, "or", DefaultOrText());
		s_perkRequirementConfig.notText = LoadPerkConfigText(perkNode, "not", DefaultNotText());

		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: loaded perk requirement config:");
			gLog.FormattedMessage("tnvse_dictionary:   req=\"%s\" ranks=\"%s\" source=\"%s\"",
				s_perkRequirementConfig.reqText.c_str(),
				s_perkRequirementConfig.ranksText.c_str(),
				s_perkRequirementConfig.sourceText.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   levelSuffix=\"%s\" and=\"%s\" or=\"%s\" not=\"%s\"",
				s_perkRequirementConfig.levelSuffix.c_str(),
				s_perkRequirementConfig.andText.c_str(),
				s_perkRequirementConfig.orText.c_str(),
				s_perkRequirementConfig.notText.c_str());
		}
	}

	bool TryTranslatePerkDescription(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4 || source.empty())
			return false;

		PerkDescriptionParts parts;
		if (!ParsePerkDescription(source, parts))
			return false;

		std::string requirement = TranslateRequirementText(parts.requirement, depth);
		if (requirement.empty())
			return false;

		std::string result;
		AppendPerkLine(result, s_perkRequirementConfig.reqText, requirement);
		if (parts.hasRanks)
			AppendPerkLine(result, s_perkRequirementConfig.ranksText, parts.ranks);
		if (parts.hasSource)
			AppendPerkLine(result, s_perkRequirementConfig.sourceText, parts.source);
		if (parts.hasBody)
		{
			if (!result.empty())
				result += "\n\n";
			result += TranslateBodyExactOrSelf(parts.body, depth);
		}

		if (result.empty() || result == source)
			return false;

		translated = std::move(result);
		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: perk description match:");
			gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   result=\"%s\"", translated.c_str());
		}
		return true;
	}

} // namespace fonthook
