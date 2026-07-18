#include "dictionary_internal.h"

namespace fonthook
{
	namespace
	{
		std::string TranslateCapture(const std::string& capture, int depth)
		{
			std::string translated;
			if (depth < 4 && TranslateInternal(capture.c_str(), translated, depth + 1))
				return translated;
			return capture;
		}
	}

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
