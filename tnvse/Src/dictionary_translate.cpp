#include "dictionary_internal.h"
#include "load_config.h"
#include "native_calls.h"

#include <algorithm>

namespace fonthook
{
	namespace
	{
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

		// Resolve &variable; escape sequences in-place using the game engine.
		// Returns true if any variables were resolved.
		bool PreResolveGameVariables(std::string& text)
		{
			if (text.find('&') == std::string::npos)
				return false;

			std::string result;
			result.reserve(text.size());
			bool changed = false;

			for (size_t i = 0; i < text.size(); )
			{
				if (text[i] != '&' || i + 2 >= text.size())
				{
					result.push_back(text[i++]);
					continue;
				}

				// Extract variable name: & or &- followed by chars until ; \n or end
				size_t nameStart = i + 1;
				bool isNegative = false;
				if (text[nameStart] == '-')
				{
					isNegative = true;
					++nameStart;
				}
				size_t nameEnd = nameStart;
				while (nameEnd < text.size()
					&& text[nameEnd] != ';'
					&& text[nameEnd] != '\n'
					&& text[nameEnd] != '\0')
				{
					++nameEnd;
				}
				if (nameEnd == nameStart || nameEnd >= text.size())
				{
					result.push_back(text[i++]);
					continue;
				}
				bool hasSemicolon = text[nameEnd] == ';';

				std::string varName(text, nameStart, nameEnd - nameStart);
				char outBuf[1024] = {};
				if (Interface_FindTextReplacementString(
					varName.c_str(), outBuf,
					static_cast<UInt32>(sizeof(outBuf)), !isNegative))
				{
					result.append(outBuf);
					i = nameEnd + (hasSemicolon ? 1 : 0);
					changed = true;
				}
				else
				{
					result.push_back(text[i++]);
				}
			}

			if (changed)
				text.swap(result);
			return changed;
	}
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

		bool MatchWildcardWithCaptureSource(
			const DictionaryEntry& entry,
			const std::string& key,
			const std::string& captureSource,
			std::vector<std::string>& captures)
		{
			if (captureSource.size() != key.size())
				return MatchWildcard(entry, key, captures);

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
					captures.push_back(captureSource.substr(cursor, found - cursor));
				else if (i > 0)
					captures.emplace_back();
				cursor = found + token.size();
			}

			if (!endsWithBind && cursor != key.size())
				return false;
			if (endsWithBind)
				captures.push_back(captureSource.substr(cursor));

			while (captures.size() < entry.bindCount)
				captures.emplace_back();
			return captures.size() == entry.bindCount;
		}

		bool TryTranslatePreparedKey(
			const std::string& key,
			PreparedTranslationMatch& match,
			int depth,
			const std::string* captureSource = nullptr)
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

				const std::string& wildcardCaptureSource =
					(captureSource && captureSource->size() == key.size()) ? *captureSource : key;
				if (MatchWildcardWithCaptureSource(entry, key, wildcardCaptureSource, captures) &&
					ExpandTarget(entry, captures, match.translated, depth))
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

			std::string captureSource = PrepareSourceForLookupPreserveCase(candidateText);
			std::string key = captureSource;
			ToLowerAscii(key);
			if (key.empty() || key == fullKey)
				return false;
			if (!searchedKeys.insert(key).second)
				return false;

			PreparedTranslationMatch match;
			if (!TryTranslatePreparedKey(key, match, depth, &captureSource))
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

	// ---- main translation engine ----

	bool TranslateInternal(const char* source, std::string& translated, int depth)
	{
		if (!source || !*source || !s_dictionaryLoaded)
			return false;
		if (!HasAlphabet(source))
			return false;

		std::string raw(source);
		PreResolveGameVariables(raw);
		const std::string originalRaw = raw;

		// Cache by the original text because wildcard captures preserve source casing.
		std::string cacheKey(raw);

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

		std::string captureSource = PrepareSourceForLookupPreserveCase(raw);
		std::string key = captureSource;
		ToLowerAscii(key);
		PreparedTranslationMatch fullMatch;
		if (TryTranslatePreparedKey(key, fullMatch, depth, &captureSource))
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
