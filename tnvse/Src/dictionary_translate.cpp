#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

namespace fonthook
{
	namespace
	{
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

		std::string TranslateCapture(const std::string& capture, int depth)
		{
			std::string translated;
			if (depth < 4 && TranslateInternal(capture.c_str(), translated, depth + 1))
				return translated;
			return capture;
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
		if (ContainsDoubleByteText(source))
			return false;
		if (!HasAlphabet(source))
			return false;

		std::string raw(source);

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
		auto exactIt = s_exactIndex.find(key);
		if (exactIt != s_exactIndex.end())
		{
			for (size_t index : exactIt->second)
			{
				if (ExpandTarget(s_entries[index], {}, translated, depth))
				{
					s_positiveCache.emplace(cacheKey, translated);
					TrimPositiveCache();
					return true;
				}
			}
		}

		std::vector<std::string> captures;
		for (size_t index : s_wildcardIndex)
		{
			const auto& entry = s_entries[index];
			if (key.size() < entry.lengthWithoutBinds)
				continue;
			if (MatchWildcard(entry, key, captures) && ExpandTarget(entry, captures, translated, depth))
			{
				s_positiveCache.emplace(cacheKey, translated);
				TrimPositiveCache();
				return true;
			}
		}

		s_negativeCache.insert(std::move(cacheKey));
		TrimNegativeCache();
		return false;
	}

	// ---- public API ----

	bool TranslateText(const char* source, std::string& translated)
	{
		if (!g_bEnableDictionaryTranslation)
			return false;
		return TranslateInternal(source, translated, 0);
	}

} // namespace fonthook
