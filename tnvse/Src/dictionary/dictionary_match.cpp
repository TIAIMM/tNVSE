#include "dictionary_internal.h"

namespace fonthook
{
	namespace implementation::dictionary_match {}
	using namespace implementation::dictionary_match;

	namespace implementation::dictionary_match
	{
		std::string TranslateCapture(const std::string& capture, int depth)
		{
			std::string translated;
			if (depth < 4 &&
				TryTranslateWindows1252Text(capture, translated, depth + 1))
			{
				return translated;
			}
			if (ContainsDbcs(capture))
				return capture;

			if (depth < 4 && TranslateInternal(capture.c_str(), translated, depth + 1))
				return translated;
			return capture;
		}
	}

	bool MatchWildcardTokens(const std::vector<std::string>& tokens, size_t bindCount,
		const std::string& key, std::vector<std::string>& captures)
	{
		captures.clear();
		size_t cursor = 0;
		const bool startsWithBind = !tokens.empty() && tokens.front().empty();
		const bool endsWithBind = !tokens.empty() && tokens.back().empty();

		for (size_t i = 0; i < tokens.size(); ++i)
		{
			const std::string& token = tokens[i];
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

		while (captures.size() < bindCount)
			captures.emplace_back();
		return captures.size() == bindCount;
	}

	bool MatchWildcard(const DictionaryEntry& entry, const std::string& key, std::vector<std::string>& captures)
	{
		return MatchWildcardTokens(entry.tokens, entry.bindCount, key, captures);
	}

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

} // namespace fonthook
