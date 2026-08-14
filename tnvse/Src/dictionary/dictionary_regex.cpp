#include "dictionary_internal.h"
#include "load_config.h"

#include <cctype>
#include <regex>

namespace fonthook
{
	namespace implementation::dictionary_regex {}
	using namespace implementation::dictionary_regex;

	namespace implementation::dictionary_regex
	{
		struct FuzzyRegexRule
		{
			std::regex regex;
			std::string pattern;
			bool enabled = false;
		};

		struct TranslationRegexRule
		{
			std::regex regex;
			std::string fromPattern;
			std::string toPattern;
			std::string replacement;
		};

		struct FuzzySplit
		{
			std::string prefix;
			std::string stem;
			std::string suffix;
			bool changed = false;
		};

		struct RegexMappedSource
		{
			std::string original;
			std::string text;
			std::vector<size_t> rawBegin;
			std::vector<size_t> rawEnd;
		};

		FuzzyRegexRule s_trimPrefixRegex;
		FuzzyRegexRule s_trimSuffixRegex;
		FuzzyRegexRule s_bypassPrefixRegex;
		FuzzyRegexRule s_bypassSuffixRegex;
		std::vector<TranslationRegexRule> s_translationRegexRules;

		void TrimAsciiWhitespace(std::string& text)
		{
			const auto isTrim = [](unsigned char ch)
				{
					return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
				};

			size_t first = 0;
			while (first < text.size() && isTrim((unsigned char)text[first]))
				++first;
			size_t last = text.size();
			while (last > first && isTrim((unsigned char)text[last - 1]))
				--last;
			text = text.substr(first, last - first);
		}

		void CompileFuzzyRegex(FuzzyRegexRule& rule, const std::string& pattern, const char* id)
		{
			rule = FuzzyRegexRule{};
			if (pattern.empty())
				return;

			try
			{
				rule.regex = std::regex(pattern, std::regex_constants::ECMAScript | std::regex_constants::optimize);
				rule.pattern = pattern;
				rule.enabled = rule.regex.mark_count() > 0;
				if (g_bEnableDictionaryTranslationLog)
					gLog.FormattedMessage("tnvse_dictionary: compiled fuzzy regex %s = \"%s\"  groups=%u",
						id, pattern.c_str(), static_cast<UInt32>(rule.regex.mark_count()));
				if (!rule.enabled)
					gLog.FormattedMessage("tnvse_dictionary: ignored fuzzy regex without capture group: %s", id);
			}
			catch (const std::regex_error& e)
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to compile fuzzy regex %s: %s", id, e.what());
			}
		}

		void LoadTranslationRegexRule(pugi::xml_node node)
		{
			std::string from = node.attribute("from").as_string("");
			std::string to = node.attribute("to").as_string("");
			TrimAsciiWhitespace(from);
			if (from.empty())
			{
				gLog.FormattedMessage("tnvse_dictionary: ignored regex rule with empty from");
				return;
			}

			try
			{
				TranslationRegexRule rule;
				rule.regex = std::regex(from, std::regex_constants::ECMAScript | std::regex_constants::optimize);
				rule.fromPattern = std::move(from);
				rule.toPattern = to;
				rule.replacement = PrepareTarget(std::move(to));
				s_translationRegexRules.push_back(std::move(rule));

				if (g_bEnableDictionaryTranslationLog)
				{
					const auto& loadedRule = s_translationRegexRules.back();
					gLog.FormattedMessage("tnvse_dictionary: loaded regex rule:");
					gLog.FormattedMessage("tnvse_dictionary:   from=\"%s\"", loadedRule.fromPattern.c_str());
					gLog.FormattedMessage("tnvse_dictionary:   to=\"%s\"", loadedRule.replacement.c_str());
				}
			}
			catch (const std::regex_error& e)
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to compile regex rule \"%s\": %s", from.c_str(), e.what());
			}
		}

		std::string GetConfigText(pugi::xml_node root, const char* id)
		{
			for (auto node : root.children("config"))
			{
				if (std::string(node.attribute("id").as_string("")) == id)
				{
					std::string text = node.text().as_string("");
					TrimAsciiWhitespace(text);
					return text;
				}
			}
			return {};
		}

		bool ApplyCapturedRegex(std::string& text, const FuzzyRegexRule& rule, std::string* captured)
		{
			if (!rule.enabled)
				return false;

			std::smatch match;
			if (!std::regex_search(text, match, rule.regex))
				return false;
			if (match.size() < 2 || !match[1].matched || match.position(1) < 0 || match.length(1) <= 0)
				return false;

			if (captured)
				*captured = match.str(1);
			text.erase(static_cast<size_t>(match.position(1)), static_cast<size_t>(match.length(1)));
			return true;
		}

		bool HasFuzzyTextConfig()
		{
			return s_trimPrefixRegex.enabled || s_trimSuffixRegex.enabled ||
				s_bypassPrefixRegex.enabled || s_bypassSuffixRegex.enabled;
		}

		FuzzySplit SplitFuzzyText(std::string text)
		{
			FuzzySplit result;

			std::string before = text;
			if (ApplyCapturedRegex(text, s_trimPrefixRegex, nullptr))
			{
				if (g_bEnableDictionaryTranslationLog && !result.changed)
					gLog.FormattedMessage("tnvse_dictionary: ");
				if (g_bEnableDictionaryTranslationLog)
					gLog.FormattedMessage("tnvse_dictionary:  trimPrefix: \"%s\" ->\"%s\"", before.c_str(), text.c_str());
				result.changed = true;
				before = text;
			}

			if (ApplyCapturedRegex(text, s_trimSuffixRegex, nullptr))
			{
				if (g_bEnableDictionaryTranslationLog && !result.changed)
					gLog.FormattedMessage("tnvse_dictionary: ");
				if (g_bEnableDictionaryTranslationLog)
					gLog.FormattedMessage("tnvse_dictionary:  trimSuffix: \"%s\" ->\"%s\"", before.c_str(), text.c_str());
				result.changed = true;
				before = text;
			}

			if (ApplyCapturedRegex(text, s_bypassPrefixRegex, &result.prefix))
			{
				if (g_bEnableDictionaryTranslationLog && !result.changed)
					gLog.FormattedMessage("tnvse_dictionary: ");
				if (g_bEnableDictionaryTranslationLog)
					gLog.FormattedMessage("tnvse_dictionary:  bypassPrefix: \"%s\" ->\"%s\" + \"%s\"",
						before.c_str(), text.c_str(), result.prefix.c_str());
				result.changed = true;
				before = text;
			}

			if (ApplyCapturedRegex(text, s_bypassSuffixRegex, &result.suffix))
			{
				if (g_bEnableDictionaryTranslationLog && !result.changed)
					gLog.FormattedMessage("tnvse_dictionary: ");
				if (g_bEnableDictionaryTranslationLog)
					gLog.FormattedMessage("tnvse_dictionary:  bypassSuffix: \"%s\" ->\"%s\" + \"%s\"",
						before.c_str(), text.c_str(), result.suffix.c_str());
				result.changed = true;
			}

			result.stem = std::move(text);

			if (g_bEnableDictionaryTranslationLog && result.changed)
			{
				std::string parts;
				if (!result.prefix.empty()) parts += " prefix=\"" + result.prefix + "\"";
				parts += " stem=\"" + result.stem + "\"";
				if (!result.suffix.empty()) parts += " suffix=\"" + result.suffix + "\"";
				gLog.FormattedMessage("tnvse_dictionary: fuzzy split:%s", parts.c_str());
			}

			return result;
		}

		std::string TranslateIfPossible(const std::string& text, int depth)
		{
			if (text.empty())
				return {};

			std::string translated;
			if (depth < 4 && TranslateInternal(text.c_str(), translated, depth + 1))
				return translated;
			return text;
		}

		std::string TranslateBracketInnerIfPossible(const std::string& text, int depth)
		{
			const size_t begin = text.find('[');
			const size_t end = text.rfind(']');
			if (begin == std::string::npos || end == std::string::npos || begin >= end)
				return text;

			const std::string inner = text.substr(begin + 1, end - begin - 1);
			const std::string translatedInner = TranslateIfPossible(inner, depth);
			if (translatedInner == inner)
				return text;
			return text.substr(0, begin + 1) + translatedInner + text.substr(end);
		}

		RegexMappedSource PrepareSourceForRegexLookup(const std::string& source)
		{
			RegexMappedSource mapped;
			mapped.original = source;
			mapped.text.reserve(source.size());
			mapped.rawBegin.reserve(source.size());
			mapped.rawEnd.reserve(source.size());

			for (size_t i = 0; i < source.size();)
			{
				const unsigned char ch = static_cast<unsigned char>(source[i]);
				if (source[i] == '\r')
				{
					const bool crlf = i + 1 < source.size() && source[i + 1] == '\n';
					mapped.text.push_back('\n');
					mapped.rawBegin.push_back(i);
					mapped.rawEnd.push_back(i + (crlf ? 2 : 1));
					i += crlf ? 2 : 1;
					continue;
				}

				if (source[i] == '\n' || source[i] == '\t' || !std::iscntrl(ch))
				{
					mapped.text.push_back(source[i]);
					mapped.rawBegin.push_back(i);
					mapped.rawEnd.push_back(i + 1);
				}
				++i;
			}

			return mapped;
		}

		bool HasValidRegexMappedSource(const RegexMappedSource& mapped)
		{
			return mapped.text.size() == mapped.rawBegin.size() &&
				mapped.text.size() == mapped.rawEnd.size();
		}

		std::string RawMappedRange(const RegexMappedSource& mapped, size_t begin, size_t length)
		{
			if (!HasValidRegexMappedSource(mapped) || length == 0 || begin >= mapped.text.size())
				return {};
			const size_t end = begin + length;
			if (end > mapped.text.size())
				return {};

			const size_t rawBegin = mapped.rawBegin[begin];
			const size_t rawEnd = mapped.rawEnd[end - 1];
			if (rawBegin >= rawEnd || rawEnd > mapped.original.size())
				return {};
			return mapped.original.substr(rawBegin, rawEnd - rawBegin);
		}

		const char* DetectSourceLineBreak(const std::string& source)
		{
			if (source.find("\r\n") != std::string::npos)
				return "\r\n";
			if (source.find('\r') != std::string::npos)
				return "\r";
			return "\n";
		}

		std::string ApplyLineBreakStyle(std::string text, const char* lineBreak)
		{
			if (std::string(lineBreak) == "\n" || text.find('\n') == std::string::npos)
				return text;

			std::string result;
			result.reserve(text.size());
			for (char ch : text)
			{
				if (ch == '\n')
					result += lineBreak;
				else
					result.push_back(ch);
			}
			return result;
		}
	}

	bool TryTranslateRegexText(const std::string& source, std::string& translated, bool mixedSource)
	{
		if (s_translationRegexRules.empty())
			return false;

		const RegexMappedSource regexSource = PrepareSourceForRegexLookup(source);
		const char* sourceLineBreak = DetectSourceLineBreak(source);
		for (const auto& rule : s_translationRegexRules)
		{
			std::smatch match;
			if (!std::regex_match(regexSource.text, match, rule.regex))
				continue;

			std::string result = ApplyLineBreakStyle(rule.replacement, sourceLineBreak);
			for (int i = static_cast<int>(match.size()) - 1; i >= 1; --i)
			{
				std::string capture;
				if (match[i].matched && match.position(i) >= 0)
				{
					capture = RawMappedRange(
						regexSource,
						static_cast<size_t>(match.position(i)),
						static_cast<size_t>(match.length(i)));
				}
				std::string translatedCapture;
				if (!ContainsDbcs(capture))
					TranslateInternal(capture.c_str(), translatedCapture, 0);
				const std::string& replacement = translatedCapture.empty() ? capture : translatedCapture;
				ReplaceAll(result, "$" + std::to_string(i), replacement);
			}

			translated = std::move(result);

			if (g_bEnableDictionaryTranslationLog)
			{
				gLog.FormattedMessage("tnvse_dictionary: %sregex match:",
					mixedSource ? "mixed-source " : "");
				gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
				gLog.FormattedMessage("tnvse_dictionary:   from=\"%s\"", rule.fromPattern.c_str());
				gLog.FormattedMessage("tnvse_dictionary:   to=\"%s\" ->\"%s\"",
					rule.replacement.c_str(), translated.c_str());
			}
			return true;
		}

		return false;
	}

	bool TryTranslateFuzzyText(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4 || !HasFuzzyTextConfig())
			return false;

		FuzzySplit split = SplitFuzzyText(source);
		if (!split.changed || split.stem.empty())
		{
			if (g_bEnableDictionaryTranslationLog && split.changed) {
				gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText skip: stem empty after split");
				gLog.FormattedMessage("tnvse_dictionary: ");
			}
			return false;
		}

		std::string stem = split.stem;
		TrimAsciiWhitespace(stem);
		if (stem.empty())
		{
			if (g_bEnableDictionaryTranslationLog) {
				gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText skip: stem empty after trim");
				gLog.FormattedMessage("tnvse_dictionary: ");
			}
			return false;
		}

		std::string sourceKey = PrepareSourceForLookup(source);
		std::string stemKey = PrepareSourceForLookup(stem);
		if (stemKey.empty() || stemKey == sourceKey)
		{
			if (g_bEnableDictionaryTranslationLog) {
				gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText skip: stemKey=\"%s\" == sourceKey=\"%s\"",
					stemKey.c_str(), sourceKey.c_str());
				gLog.FormattedMessage("tnvse_dictionary: ");
			}
			return false;
		}

		std::string stemTranslated;
		if (!TranslateInternal(stem.c_str(), stemTranslated, depth + 1))
		{
			if (g_bEnableDictionaryTranslationLog) {
				gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText fail: stem \"%s\" not translated", stem.c_str());
				gLog.FormattedMessage("tnvse_dictionary: ");
			}
			return false;
		}

		std::string prefixTranslated = TranslateIfPossible(split.prefix, depth + 1);
		std::string suffixTranslated = TranslateIfPossible(split.suffix, depth + 1);

		if (g_bEnableDictionaryTranslationLog)
		{
			std::string details = " stem=\"" + std::string(stem.c_str()) + "\"->\"" + stemTranslated + "\"";
			if (!split.prefix.empty())
				details += " prefix=\"" + split.prefix + "\"->\"" + prefixTranslated + "\"";
			if (!split.suffix.empty())
				details += " suffix=\"" + split.suffix + "\"->\"" + suffixTranslated + "\"";
			gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText%s", details.c_str());
		}

		if (!split.prefix.empty() && prefixTranslated == split.prefix)
		{
			auto bracketResult = TranslateBracketInnerIfPossible(split.prefix, depth + 1);
			if (g_bEnableDictionaryTranslationLog && bracketResult != split.prefix)
				gLog.FormattedMessage("tnvse_dictionary:  bracketTranslate: \"%s\" ->\"%s\"",
					split.prefix.c_str(), bracketResult.c_str());
			prefixTranslated = bracketResult;
		}

		if (!split.prefix.empty() && !prefixTranslated.empty() &&
			split.prefix.back() == ' ' && prefixTranslated.back() != ' ')
		{
			const size_t lastNonSpace = split.prefix.find_last_not_of(' ');
			const size_t trailingStart = lastNonSpace == std::string::npos ? split.prefix.size() : lastNonSpace + 1;
			prefixTranslated += split.prefix.substr(trailingStart);
		}

		translated = prefixTranslated + stemTranslated + suffixTranslated;
		if (g_bEnableDictionaryTranslationLog) {
			gLog.FormattedMessage("tnvse_dictionary: TryTranslateFuzzyText result: \"%s\" ->\"%s\"",
				source.c_str(), translated.c_str());
			gLog.FormattedMessage("tnvse_dictionary: ");
		}
		return true;
	}

	void ResetFuzzyTextConfig()
	{
		s_trimPrefixRegex = FuzzyRegexRule{};
		s_trimSuffixRegex = FuzzyRegexRule{};
		s_bypassPrefixRegex = FuzzyRegexRule{};
		s_bypassSuffixRegex = FuzzyRegexRule{};
		s_translationRegexRules.clear();
	}

	void LoadFuzzyTextConfig(pugi::xml_node root)
	{
		auto reNode = root.child("regularexpressions");
		if (!reNode)
			return;
		CompileFuzzyRegex(s_trimPrefixRegex, GetConfigText(reNode, "TrimPrefixRegularExpressions"), "TrimPrefixRegularExpressions");
		CompileFuzzyRegex(s_trimSuffixRegex, GetConfigText(reNode, "TrimSuffixRegularExpressions"), "TrimSuffixRegularExpressions");
		CompileFuzzyRegex(s_bypassPrefixRegex, GetConfigText(reNode, "BypassPrefixRegularExpressions"), "BypassPrefixRegularExpressions");
		CompileFuzzyRegex(s_bypassSuffixRegex, GetConfigText(reNode, "BypassSuffixRegularExpressions"), "BypassSuffixRegularExpressions");

		for (auto node : reNode.children("regex"))
			LoadTranslationRegexRule(node);
	}

	bool HasTranslationRegexRules()
	{
		return !s_translationRegexRules.empty();
	}

	size_t GetTranslationRegexRuleCount()
	{
		return s_translationRegexRules.size();
	}

} // namespace fonthook
