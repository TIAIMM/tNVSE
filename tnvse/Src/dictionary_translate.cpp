#include "dictionary_internal.h"
#include "load_config.h"
#include "native_calls.h"

#include <algorithm>
#include <cctype>

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

		struct MappedPreparedSource
		{
			std::string original;
			std::string text;
			std::string key;
			std::vector<size_t> rawBegin;
			std::vector<size_t> rawEnd;
		};

		struct MappedChar
		{
			char ch = 0;
			size_t rawBegin = 0;
			size_t rawEnd = 0;
		};

		bool IsSourceWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
		}

		bool IsAsciiAlpha(char ch)
		{
			return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
		}

		char ToLowerAsciiChar(char ch)
		{
			if (ch >= 'A' && ch <= 'Z')
				return static_cast<char>(ch + ('a' - 'A'));
			return ch;
		}

		bool StartsWithIgnoreCase(const std::string& text, size_t pos, std::string_view pattern)
		{
			if (pos + pattern.size() > text.size())
				return false;
			for (size_t i = 0; i < pattern.size(); ++i)
			{
				if (ToLowerAsciiChar(text[pos + i]) != ToLowerAsciiChar(pattern[i]))
					return false;
			}
			return true;
		}

		bool TryMatchEscapedSourceWhitespace(const std::string& text, size_t pos, size_t& end)
		{
			if (StartsWithIgnoreCase(text, pos, "[CRLF]"))
			{
				end = pos + 6;
				return true;
			}

			if (pos + 1 >= text.size() || text[pos] != '\\')
				return false;

			const char code = text[pos + 1];
			if ((code == 'r' || code == 'R') &&
				pos + 3 < text.size() &&
				text[pos + 2] == '\\' &&
				(text[pos + 3] == 'n' || text[pos + 3] == 'N'))
			{
				end = pos + 4;
				return true;
			}

			if (code == 'n' || code == 'N' || code == 'r' || code == 'R' || code == 't' || code == 'T')
			{
				end = pos + 2;
				return true;
			}

			return false;
		}

		bool IsSourceBreakTag(const std::string& text, size_t tagBegin, size_t tagEnd)
		{
			size_t pos = tagBegin + 1;
			while (pos < tagEnd && IsSourceWhitespace(text[pos]))
				++pos;
			if (pos < tagEnd && text[pos] == '/')
			{
				++pos;
				while (pos < tagEnd && IsSourceWhitespace(text[pos]))
					++pos;
			}

			const size_t nameBegin = pos;
			while (pos < tagEnd && IsAsciiAlpha(text[pos]))
				++pos;
			const size_t nameLength = pos - nameBegin;
			if (nameLength == 0)
				return false;

			const bool isParagraph = nameLength == 1 && ToLowerAsciiChar(text[nameBegin]) == 'p';
			const bool isDiv =
				nameLength == 3 &&
				ToLowerAsciiChar(text[nameBegin]) == 'd' &&
				ToLowerAsciiChar(text[nameBegin + 1]) == 'i' &&
				ToLowerAsciiChar(text[nameBegin + 2]) == 'v';
			if (!isParagraph && !isDiv)
				return false;

			return pos == tagEnd || IsSourceWhitespace(text[pos]) || text[pos] == '/';
		}

		char Correct1252Char(char ch)
		{
			static constexpr char table[129] =
				"                 ''\"\"                                           AAA A ACEEEEIIII NOOO O  UUUU  saaa a aceeeeiiii nooo o  uuuu   ";
			const UInt8 c = (UInt8)ch;
			if (c >= 128)
				return table[c - 128];
			return ch;
		}

		MappedPreparedSource PrepareSourceForLookupMapped(const std::string& source)
		{
			MappedPreparedSource mapped;
			mapped.original = source;

			std::vector<MappedChar> units;
			units.reserve(source.size());
			for (size_t i = 0; i < source.size();)
			{
				size_t escapedEnd = 0;
				if (TryMatchEscapedSourceWhitespace(source, i, escapedEnd))
				{
					units.push_back({ ' ', i, escapedEnd });
					i = escapedEnd;
					continue;
				}

				if (source[i] == '<')
				{
					const size_t end = source.find('>', i + 1);
					if (end != std::string::npos && IsSourceBreakTag(source, i, end))
					{
						units.push_back({ ' ', i, end + 1 });
						i = end + 1;
						continue;
					}
				}

				const unsigned char ch = static_cast<unsigned char>(source[i]);
				if (IsSourceWhitespace(source[i]))
				{
					units.push_back({ ' ', i, i + 1 });
				}
				else if (!std::iscntrl(ch))
				{
					units.push_back({ Correct1252Char(source[i]), i, i + 1 });
				}
				++i;
			}

			std::vector<MappedChar> collapsed;
			collapsed.reserve(units.size());
			for (const MappedChar& unit : units)
			{
				if (IsSourceWhitespace(unit.ch))
				{
					if (!collapsed.empty() && collapsed.back().ch == ' ')
					{
						collapsed.back().rawEnd = unit.rawEnd;
					}
					else
					{
						collapsed.push_back({ ' ', unit.rawBegin, unit.rawEnd });
					}
				}
				else
				{
					collapsed.push_back(unit);
				}
			}

			size_t begin = 0;
			while (begin < collapsed.size() && collapsed[begin].ch == ' ')
				++begin;
			size_t end = collapsed.size();
			while (end > begin && collapsed[end - 1].ch == ' ')
				--end;

			mapped.text.reserve(end - begin);
			mapped.key.reserve(end - begin);
			mapped.rawBegin.reserve(end - begin);
			mapped.rawEnd.reserve(end - begin);
			for (size_t i = begin; i < end; ++i)
			{
				mapped.text.push_back(collapsed[i].ch);
				mapped.key.push_back(ToLowerAsciiChar(collapsed[i].ch));
				mapped.rawBegin.push_back(collapsed[i].rawBegin);
				mapped.rawEnd.push_back(collapsed[i].rawEnd);
			}
			return mapped;
		}

		bool HasValidMappedSource(const MappedPreparedSource& mapped)
		{
			return mapped.text.size() == mapped.key.size() &&
				mapped.text.size() == mapped.rawBegin.size() &&
				mapped.text.size() == mapped.rawEnd.size();
		}

		std::string RawMappedRange(const MappedPreparedSource& mapped, size_t begin, size_t end)
		{
			if (!HasValidMappedSource(mapped) || begin >= end || end > mapped.text.size())
				return {};
			const size_t rawBegin = mapped.rawBegin[begin];
			const size_t rawEnd = mapped.rawEnd[end - 1];
			if (rawBegin >= rawEnd || rawEnd > mapped.original.size())
				return {};
			return mapped.original.substr(rawBegin, rawEnd - rawBegin);
		}

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

		struct FormattedLine
		{
			std::string leadingWhitespace;
			std::string text;
			std::string trailingWhitespace;
			std::string lineBreak;
		};

		bool IsLineWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t';
		}

		std::vector<FormattedLine> SplitFormattedLines(const std::string& text)
		{
			std::vector<FormattedLine> lines;
			size_t cursor = 0;
			while (cursor < text.size())
			{
				const size_t lineBegin = cursor;
				size_t lineEnd = cursor;
				while (lineEnd < text.size() && text[lineEnd] != '\r' && text[lineEnd] != '\n')
					++lineEnd;

				std::string lineBreak;
				if (lineEnd < text.size())
				{
					if (text[lineEnd] == '\r' && lineEnd + 1 < text.size() && text[lineEnd + 1] == '\n')
					{
						lineBreak = "\r\n";
						cursor = lineEnd + 2;
					}
					else
					{
						lineBreak.assign(1, text[lineEnd]);
						cursor = lineEnd + 1;
					}
				}
				else
				{
					cursor = lineEnd;
				}

				const std::string line = text.substr(lineBegin, lineEnd - lineBegin);
				size_t textBegin = 0;
				while (textBegin < line.size() && IsLineWhitespace(line[textBegin]))
					++textBegin;

				size_t textEnd = line.size();
				while (textEnd > textBegin && IsLineWhitespace(line[textEnd - 1]))
					--textEnd;

				FormattedLine formattedLine;
				formattedLine.leadingWhitespace = line.substr(0, textBegin);
				formattedLine.text = line.substr(textBegin, textEnd - textBegin);
				formattedLine.trailingWhitespace = line.substr(textEnd);
				formattedLine.lineBreak = std::move(lineBreak);
				lines.push_back(std::move(formattedLine));
			}
			return lines;
		}

		bool MatchWildcardWithCaptureSource(
			const DictionaryEntry& entry,
			const std::string& key,
			const MappedPreparedSource* mappedSource,
			std::vector<std::string>& captures)
		{
			if (!mappedSource || !HasValidMappedSource(*mappedSource) || mappedSource->key != key)
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
					captures.push_back(RawMappedRange(*mappedSource, cursor, found));
				else if (i > 0)
					captures.emplace_back();
				cursor = found + token.size();
			}

			if (!endsWithBind && cursor != key.size())
				return false;
			if (endsWithBind)
				captures.push_back(RawMappedRange(*mappedSource, cursor, key.size()));

			while (captures.size() < entry.bindCount)
				captures.emplace_back();
			return captures.size() == entry.bindCount;
		}

		bool TryTranslateWildcardKey(
			const std::string& key,
			PreparedTranslationMatch& match,
			int depth,
			const MappedPreparedSource* mappedSource = nullptr)
		{
			match = PreparedTranslationMatch{};
			if (!g_bEnableDictionaryWildcardTranslation)
				return false;

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

				if (MatchWildcardWithCaptureSource(entry, key, mappedSource, captures) &&
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

		bool TryTranslatePreparedKey(
			const std::string& key,
			PreparedTranslationMatch& match,
			int depth,
			const MappedPreparedSource* mappedSource = nullptr)
		{
			if (TryTranslateExactKey(key, match, depth))
				return true;
			return TryTranslateWildcardKey(key, match, depth, mappedSource);
		}

		bool TryTranslateBeforeLinebreakText(const std::string& source, std::string& translated, int depth)
		{
			if (depth >= 4 || (source.find('\n') == std::string::npos && source.find('\r') == std::string::npos))
				return false;

			const std::vector<FormattedLine> lines = SplitFormattedLines(source);
			if (lines.size() < 2)
				return false;

			std::string result;
			bool changed = false;

			for (const FormattedLine& line : lines)
			{
				result += line.leadingWhitespace;

				if (line.text.empty() || !HasAlphabet(line.text))
				{
					result += line.text;
					result += line.trailingWhitespace;
					result += line.lineBreak;
					continue;
				}

				const std::string key = PrepareSourceForLookup(line.text);
				PreparedTranslationMatch match;
				if (!key.empty() && TryTranslateExactKey(key, match, depth))
				{
					result += match.translated;
					changed = true;

					if (g_bEnableDictionaryTranslationLog)
					{
						gLog.FormattedMessage("tnvse_dictionary: before-linebreak exact hit:");
						gLog.FormattedMessage("tnvse_dictionary:   line=\"%s\"", line.text.c_str());
						gLog.FormattedMessage("tnvse_dictionary:   entry=\"%s\" ->\"%s\"",
							s_entries[match.entryIndex].key.c_str(), match.translated.c_str());
					}
				}
				else
				{
					result += line.text;
				}

				result += line.trailingWhitespace;
				result += line.lineBreak;
			}

			if (!changed)
				return false;

			translated = std::move(result);
			return true;
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

			MappedPreparedSource mappedSource = PrepareSourceForLookupMapped(candidateText);
			std::string key = mappedSource.key;
			if (key.empty() || key == fullKey)
				return false;
			if (!searchedKeys.insert(key).second)
				return false;

			PreparedTranslationMatch match;
			if (!TryTranslatePreparedKey(key, match, depth, &mappedSource))
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

	bool TryTranslateExactText(const std::string& source, std::string& translated, int depth)
	{
		translated.clear();
		if (source.empty() || depth >= 4 || !s_dictionaryLoaded || !HasAlphabet(source))
			return false;

		const MappedPreparedSource mappedSource = PrepareSourceForLookupMapped(source);
		if (mappedSource.key.empty())
			return false;

		PreparedTranslationMatch match;
		if (!TryTranslateExactKey(mappedSource.key, match, depth))
			return false;

		translated = std::move(match.translated);
		return true;
	}

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

		MappedPreparedSource mappedSource = PrepareSourceForLookupMapped(raw);
		std::string key = mappedSource.key;
		PreparedTranslationMatch fullMatch;
		if (TryTranslateExactKey(key, fullMatch, depth))
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

		if (g_bEnableMuxQuestPromptTranslation && TryTranslateMuxQuestPrompt(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryPerkDescriptionTranslation && TryTranslatePerkDescription(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryItemEffectTranslation && TryTranslateItemEffectList(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryMultiplierTextTranslation && TryTranslateMultiplierText(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryWildcardTranslation && TryTranslateWildcardKey(key, fullMatch, depth, &mappedSource))
		{
			translated = fullMatch.translated;
			if (g_bEnableDictionaryTranslationLog)
			{
				gLog.FormattedMessage("tnvse_dictionary: TranslateInternal wildcard match:");
				gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source);
				gLog.FormattedMessage("tnvse_dictionary:   entry=\"%s\" ->\"%s\"",
					s_entries[fullMatch.entryIndex].key.c_str(), translated.c_str());
			}
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryRegexTranslation && TryTranslateRegexText(raw, translated))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryBeforeLinebreakTranslation && TryTranslateBeforeLinebreakText(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryShrinkFuzzyTranslation && TryTranslateShrinkText(raw, translated, depth))
		{
			s_positiveCache.emplace(cacheKey, translated);
			TrimPositiveCache();
			return true;
		}

		if (g_bEnableDictionaryTrimBypassFuzzyTranslation && TryTranslateFuzzyText(originalRaw, translated, depth))
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
