#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <regex>

namespace fonthook
{
	namespace
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

		struct PreparedTranslationMatch
		{
			std::string translated;
			size_t entryIndex = 0;
			bool exact = false;
			bool found = false;
		};

		struct ShrinkTranslationMatch
		{
			std::string side;
			std::string candidateText;
			std::string key;
			std::string translated;
			size_t entryIndex = 0;
			bool exact = false;
			bool found = false;
		};

		FuzzyRegexRule s_trimPrefixRegex;
		FuzzyRegexRule s_trimSuffixRegex;
		FuzzyRegexRule s_bypassPrefixRegex;
		FuzzyRegexRule s_bypassSuffixRegex;
		std::vector<TranslationRegexRule> s_translationRegexRules;

		std::string StripLeadingId(std::string text, std::string& id)
		{
			id.clear();
			if (text.empty() || text[0] != '<')
				return text;
			const size_t end = text.find('>');
			if (end == std::string::npos)
				return text;

			std::string tag = text.substr(1, end - 1);
			if (tag.rfind("tNVSE ", 0) == 0)
				tag.erase(0, 6);
			id = tag;
			text.erase(0, end + 1);
			return text;
		}

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
			const std::string originalText = text;
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

		std::string TranslateCapture(const std::string& capture, int depth)
		{
			std::string translated;
			if (depth < 4 && TranslateInternal(capture.c_str(), translated, depth + 1))
				return translated;
			return capture;
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

		bool IsAsciiLetter(char ch)
		{
			return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
		}

		size_t ShrinkRight(const std::string& text, size_t end)
		{
			if (end == 0)
				return 0;

			if (IsAsciiLetter(text[end - 1]))
			{
				while (end > 0 && IsAsciiLetter(text[end - 1]))
					--end;
				return end;
			}

			return end - 1;
		}

		size_t ShrinkLeft(const std::string& text, size_t begin)
		{
			if (begin >= text.size())
				return text.size();

			if (IsAsciiLetter(text[begin]))
			{
				while (begin < text.size() && IsAsciiLetter(text[begin]))
					++begin;
				return begin;
			}

			return begin + 1;
		}

		void TrimCandidateRange(const std::string& text, size_t& begin, size_t& end)
		{
			const auto isTrim = [](unsigned char ch)
				{
					return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
				};

			while (begin < end && isTrim((unsigned char)text[begin]))
				++begin;
			while (end > begin && isTrim((unsigned char)text[end - 1]))
				--end;
		}

		void AddWildcardCandidates(
			const std::vector<size_t>& indexes,
			std::vector<size_t>& candidates,
			std::unordered_set<size_t>& seen)
		{
			for (size_t index : indexes)
			{
				if (seen.insert(index).second)
					candidates.push_back(index);
			}
		}

		void AddWildcardCandidatesForKey(
			const std::unordered_map<std::string, std::vector<size_t>>& indexMap,
			const std::string& candidateKey,
			std::vector<size_t>& candidates,
			std::unordered_set<size_t>& seen)
		{
			const auto it = indexMap.find(candidateKey);
			if (it != indexMap.end())
				AddWildcardCandidates(it->second, candidates, seen);
		}

		void CollectPrefixWildcardCandidates(
			const std::string& key,
			std::vector<size_t>& candidates,
			std::unordered_set<size_t>& seen)
		{
			for (size_t end = key.size(); end > 0; --end)
				AddWildcardCandidatesForKey(s_wildcardPrefixIndex, key.substr(0, end), candidates, seen);
		}

		void CollectSuffixWildcardCandidates(
			const std::string& key,
			std::vector<size_t>& candidates,
			std::unordered_set<size_t>& seen)
		{
			for (size_t begin = 0; begin < key.size(); ++begin)
				AddWildcardCandidatesForKey(s_wildcardSuffixIndex, key.substr(begin), candidates, seen);
		}

		bool TryTranslateExactKey(const std::string& key, PreparedTranslationMatch& match, int depth)
		{
			match = PreparedTranslationMatch{};

			auto exactIt = s_exactIndex.find(key);
			if (exactIt == s_exactIndex.end())
				return false;

			for (size_t index : exactIt->second)
			{
				if (ExpandTarget(s_entries[index], {}, match.translated, depth))
				{
					match.entryIndex = index;
					match.exact = true;
					match.found = true;
					return true;
				}
			}

			return false;
		}

		std::vector<std::string> SplitNonEmptyLines(std::string text)
		{
			std::vector<std::string> lines;
			size_t cursor = 0;
			while (cursor <= text.size())
			{
				const size_t end = text.find('\n', cursor);
				std::string line = end == std::string::npos
					? text.substr(cursor)
					: text.substr(cursor, end - cursor);
				TrimAsciiWhitespace(line);
				if (!line.empty())
					lines.push_back(std::move(line));
				if (end == std::string::npos)
					break;
				cursor = end + 1;
			}
			return lines;
		}

		std::string JoinLines(const std::vector<std::string>& lines, size_t begin, size_t end, const char* separator)
		{
			std::string result;
			for (size_t i = begin; i < end && i < lines.size(); ++i)
			{
				if (!result.empty())
					result += separator;
				result += lines[i];
			}
			return result;
		}

		std::string PrepareSourceForRegexLookup(std::string text)
		{
			RemoveControlChars(text);
			ReplaceAll(text, "\r\n", "\n");
			ReplaceAll(text, "\r", "\n");
			return text;
		}

		bool TryTranslatePreparedKey(const std::string& key, PreparedTranslationMatch& match, int depth)
		{
			match = PreparedTranslationMatch{};

			if (TryTranslateExactKey(key, match, depth))
				return true;

			std::vector<size_t> candidateIndexes;
			std::unordered_set<size_t> seenCandidates;
			CollectPrefixWildcardCandidates(key, candidateIndexes, seenCandidates);
			CollectSuffixWildcardCandidates(key, candidateIndexes, seenCandidates);
			AddWildcardCandidates(s_wildcardLooseIndex, candidateIndexes, seenCandidates);

			std::sort(candidateIndexes.begin(), candidateIndexes.end(), [](size_t left, size_t right)
				{
					return EntryLess(s_entries[left], s_entries[right]);
				});

			std::vector<std::string> captures;
			for (size_t index : candidateIndexes)
			{
				const auto& entry = s_entries[index];
				if (key.size() < entry.lengthWithoutBinds)
					continue;
				if (MatchWildcard(entry, key, captures) && ExpandTarget(entry, captures, match.translated, depth))
				{
					match.entryIndex = index;
					match.exact = false;
					match.found = true;
					return true;
				}
			}

			return false;
		}

		bool TryTranslateBeforeLinebreakText(const std::string& source, const std::string& fullKey, std::string& translated, int depth)
		{
			if (depth >= 4 || source.find('\n') == std::string::npos)
				return false;

			std::vector<std::string> lines = SplitNonEmptyLines(source);
			if (lines.size() < 2)
				return false;

			std::unordered_set<std::string> searchedKeys;
			searchedKeys.insert(fullKey);

			for (size_t prefixLineCount = lines.size() - 1; prefixLineCount > 0; --prefixLineCount)
			{
				const std::string candidateText = JoinLines(lines, 0, prefixLineCount, " ");
				if (!HasAlphabet(candidateText))
					continue;

				const std::string key = PrepareSourceForLookup(candidateText);
				if (key.empty() || !searchedKeys.insert(key).second)
					continue;

				PreparedTranslationMatch match;
				if (!TryTranslateExactKey(key, match, depth))
					continue;

				const std::string remainder = JoinLines(lines, prefixLineCount, lines.size(), "\n");
				std::string translatedRemainder;
				const bool translatedRest = !remainder.empty() && TranslateInternal(remainder.c_str(), translatedRemainder, depth + 1);
				translated = match.translated;
				if (!remainder.empty())
				{
					translated += '\n';
					translated += translatedRest ? translatedRemainder : remainder;
				}

				if (g_bEnableDictionaryTranslationLog)
				{
					gLog.FormattedMessage("tnvse_dictionary: before-linebreak exact hit:");
					gLog.FormattedMessage("tnvse_dictionary:   candidate=\"%s\"", candidateText.c_str());
					gLog.FormattedMessage("tnvse_dictionary:   entry=\"%s\" ->\"%s\"",
						s_entries[match.entryIndex].key.c_str(), match.translated.c_str());
				}
				return true;
			}

			return false;
		}

		bool TryTranslateRegexText(const std::string& source, std::string& translated)
		{
			if (s_translationRegexRules.empty())
				return false;

			const std::string regexSource = PrepareSourceForRegexLookup(source);
			for (const auto& rule : s_translationRegexRules)
			{
				std::smatch match;
				if (!std::regex_match(regexSource, match, rule.regex))
					continue;

				// Build result: substitute $N placeholders with translated captures
				std::string result = rule.replacement;
				for (int i = static_cast<int>(match.size()) - 1; i >= 1; --i)
				{
					std::string capture = match[i].str();
					std::string translatedCapture;
					TranslateInternal(capture.c_str(), translatedCapture, 0);
					const std::string& replacement = translatedCapture.empty() ? capture : translatedCapture;
					ReplaceAll(result, "$" + std::to_string(i), replacement);
				}


				translated = std::move(result);

				if (g_bEnableDictionaryTranslationLog)
				{
					gLog.FormattedMessage("tnvse_dictionary: regex match:");
					gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
					gLog.FormattedMessage("tnvse_dictionary:   from=\"%s\"", rule.fromPattern.c_str());
					gLog.FormattedMessage("tnvse_dictionary:   to=\"%s\" ->\"%s\"",
						rule.replacement.c_str(), translated.c_str());
				}
				return true;
			}

			return false;
		}

		bool TryBuildShrinkMatch(
			const std::string& source,
			size_t begin,
			size_t end,
			const char* side,
			const std::string& fullKey,
			std::unordered_set<std::string>& searchedKeys,
			ShrinkTranslationMatch& result,
			int depth)
		{
			result = ShrinkTranslationMatch{};
			if (begin >= end)
				return false;

			size_t trimBegin = begin;
			size_t trimEnd = end;
			TrimCandidateRange(source, trimBegin, trimEnd);
			if (trimBegin >= trimEnd)
				return false;

			std::string candidateText = source.substr(trimBegin, trimEnd - trimBegin);
			if (!HasAlphabet(candidateText))
				return false;

			std::string key = PrepareSourceForLookup(candidateText);
			if (key.empty() || key == fullKey)
				return false;
			if (!searchedKeys.insert(key).second)
				return false;

			PreparedTranslationMatch match;
			if (!TryTranslatePreparedKey(key, match, depth))
				return false;

			result.side = side;
			result.candidateText = std::move(candidateText);
			result.key = std::move(key);
			result.entryIndex = match.entryIndex;
			result.exact = match.exact;
			result.found = true;
			result.translated = source.substr(0, trimBegin) + match.translated + source.substr(trimEnd);

			if (g_bEnableDictionaryTranslationLog)
			{
				gLog.FormattedMessage("tnvse_dictionary: shrink fuzzy hit[%s]:", result.side.c_str());
				gLog.FormattedMessage("tnvse_dictionary:   candidate=\"%s\"", result.candidateText.c_str());
				gLog.FormattedMessage("tnvse_dictionary:   entry=\"%s\" ->\"%s\"",
					s_entries[result.entryIndex].key.c_str(), result.translated.c_str());
			}

			return true;
		}

		bool IsBetterShrinkMatch(const ShrinkTranslationMatch& left, const ShrinkTranslationMatch& right)
		{
			if (!right.found)
				return true;
			return EntryLess(s_entries[left.entryIndex], s_entries[right.entryIndex]);
		}

		bool TryTranslateShrinkText(const std::string& source, std::string& translated, int depth)
		{
			if (depth >= 4)
				return false;

			std::string fullKey = PrepareSourceForLookup(source);
			if (fullKey.empty())
				return false;

			std::unordered_set<std::string> searchedKeys;
			searchedKeys.insert(fullKey);

			size_t rightEnd = source.size();
			size_t leftBegin = 0;
			while (rightEnd > 0 || leftBegin < source.size())
			{
				bool progressed = false;
				ShrinkTranslationMatch bestMatch;

				if (rightEnd > 0)
				{
					const size_t nextRightEnd = ShrinkRight(source, rightEnd);
					if (nextRightEnd != rightEnd)
					{
						rightEnd = nextRightEnd;
						progressed = true;

						ShrinkTranslationMatch match;
						if (TryBuildShrinkMatch(source, 0, rightEnd, "prefix", fullKey, searchedKeys, match, depth) &&
							IsBetterShrinkMatch(match, bestMatch))
						{
							bestMatch = std::move(match);
						}
					}
				}

				if (leftBegin < source.size())
				{
					const size_t nextLeftBegin = ShrinkLeft(source, leftBegin);
					if (nextLeftBegin != leftBegin)
					{
						leftBegin = nextLeftBegin;
						progressed = true;

						ShrinkTranslationMatch match;
						if (TryBuildShrinkMatch(source, leftBegin, source.size(), "suffix", fullKey, searchedKeys, match, depth) &&
							IsBetterShrinkMatch(match, bestMatch))
						{
							bestMatch = std::move(match);
						}
					}
				}

				if (bestMatch.found)
				{
					translated = bestMatch.translated;
					if (g_bEnableDictionaryTranslationLog)
						gLog.FormattedMessage("tnvse_dictionary: shrink fuzzy result[%s]: \"%s\" ->\"%s\"",
							bestMatch.side.c_str(), source.c_str(), translated.c_str());
					return true;
				}

				if (!progressed)
					break;
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

		void TrimPositiveCache()
		{
			if (s_positiveCache.size() > kPositiveCacheLimit)
			{
				gLog.FormattedMessage("tnvse_dictionary: positive cache exceeded %u, clearing",
					static_cast<UInt32>(kPositiveCacheLimit));
				s_positiveCache.clear();
			}
		}

		void TrimNegativeCache()
		{
			if (s_negativeCache.size() > kNegativeCacheLimit)
			{
				gLog.FormattedMessage("tnvse_dictionary: negative cache exceeded %u, clearing",
					static_cast<UInt32>(kNegativeCacheLimit));
				s_negativeCache.clear();
			}
		}
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

	// ---- wildcard matching ----

	bool MatchWildcard(const DictionaryEntry& entry, const std::string& key, std::vector<std::string>& captures)
	{
		captures.clear();
		size_t cursor = 0;
		const bool startsWithBind = entry.tokens.size() > 0 && entry.tokens.front().empty();
		const bool endsWithBind = entry.tokens.size() > 0 && entry.tokens.back().empty();

		for (size_t i = 0; i < entry.tokens.size(); ++i)
		{
			const std::string& token = entry.tokens[i];
			if (token.empty())
				continue;

			const size_t found = key.find(token, cursor);
			if (found == std::string::npos)
				return false;
			if (i == 0 && !startsWithBind && found != 0)
				return false;

			if (found > cursor)
				captures.push_back(key.substr(cursor, found - cursor));
			else if (i > 0)
				captures.emplace_back();
			cursor = found + token.size();
		}

		if (!endsWithBind && cursor != key.size())
			return false;
		if (endsWithBind)
			captures.push_back(key.substr(cursor));

		while (captures.size() < entry.bindCount)
			captures.emplace_back();
		return captures.size() == entry.bindCount;
	}

	// ---- target expansion ----

	bool ExpandTarget(const DictionaryEntry& entry, const std::vector<std::string>& captures, std::string& translated, int depth)
	{
		if (entry.bindCount == 0)
		{
			translated = entry.target;
			return true;
		}

		auto parts = SplitTargetByBindToken(entry.target);
		if (parts.size() != captures.size() + 1)
			return false;

		translated.clear();
		for (size_t i = 0; i < captures.size(); ++i)
		{
			translated += parts[i];
			translated += TranslateCapture(captures[i], depth);
		}
		translated += parts.back();
		return true;
	}

	// ---- main translation engine ----

	bool TranslateInternal(const char* source, std::string& translated, int depth)
	{
		if (!source || !*source || !s_dictionaryLoaded)
			return false;
		if (!HasAlphabet(source))
			return false;

		std::string raw(source);
		const std::string originalRaw = raw;

		// Normalize cache key once: lowercase the original source so that
		// "Hello", "HELLO", and "hello" share one cache entry instead of three.
		std::string cacheKey(raw);
		ToLowerAscii(cacheKey);

		auto positive = s_positiveCache.find(cacheKey);
		if (positive != s_positiveCache.end())
		{
			translated = positive->second;
			return true;
		}
		if (s_negativeCache.find(cacheKey) != s_negativeCache.end())
			return false;

		std::string id;
		std::string withoutId = StripLeadingId(raw, id);
		if (!id.empty())
		{
			auto idIt = s_idIndex.find(id);
			if (idIt != s_idIndex.end())
			{
				if (ExpandTarget(s_entries[idIt->second], {}, translated, depth))
				{
					s_positiveCache.emplace(std::move(cacheKey), translated);
					TrimPositiveCache();
					return true;
				}
			}
			raw = withoutId;
		}

		const std::string key = PrepareSourceForLookup(raw);
		PreparedTranslationMatch fullMatch;
		if (TryTranslatePreparedKey(key, fullMatch, depth))
		{
			translated = fullMatch.translated;
			if (g_bEnableDictionaryTranslationLog)
			{
				gLog.FormattedMessage("tnvse_dictionary: TranslateInternal %s match:", fullMatch.exact ? "exact" : "wildcard");
				gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source);
				gLog.FormattedMessage("tnvse_dictionary:   entry=\"%s\" ->\"%s\"",
					s_entries[fullMatch.entryIndex].key.c_str(), translated.c_str());
			}
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (TryTranslateRegexText(raw, translated))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (TryTranslateBeforeLinebreakText(raw, key, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (TryTranslateShrinkText(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (TryTranslateFuzzyText(originalRaw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryTranslationLog)
			gLog.FormattedMessage("tnvse_dictionary: TranslateInternal no match: \"%s\" ->negative cache", source);

		s_negativeCache.insert(std::move(cacheKey));
		TrimNegativeCache();
		return false;
	}

	// ---- public API ----

	bool TranslateText(const char* source, std::string& translated)
	{
		if (!g_bEnableDictionaryTranslation)
			return false;
		const bool result = TranslateInternal(source, translated, 0);
		return result;
	}

} // namespace fonthook
