#include "dictionary_internal.h"
#include "load_config.h"

#include <cstring>
#include <utility>

namespace fonthook
{
	namespace implementation::dictionary_mux {}
	using namespace implementation::dictionary_mux;

	namespace implementation::dictionary_mux
	{
		struct MuxQuestSegment
		{
			std::string status;
			std::string body;
		};

		bool IsAsciiWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
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

		bool MatchStatusAt(const std::string& text, size_t pos, std::string& status)
		{
			static constexpr const char* kStatuses[] = { "Added:", "Complete:", "Fail:" };
			for (const char* candidate : kStatuses)
			{
				const size_t length = std::strlen(candidate);
				if (pos + length <= text.size() && text.compare(pos, length, candidate) == 0)
				{
					status.assign(candidate, length);
					return true;
				}
			}
			return false;
		}

		size_t FindNextStatusSeparator(const std::string& text, size_t pos)
		{
			while (pos < text.size())
			{
				while (pos < text.size() && !IsAsciiWhitespace(text[pos]))
					++pos;
				const size_t whitespaceBegin = pos;
				while (pos < text.size() && IsAsciiWhitespace(text[pos]))
					++pos;

				std::string status;
				if (pos > whitespaceBegin &&
					MatchStatusAt(text, pos, status) &&
					pos + status.size() < text.size() &&
					IsAsciiWhitespace(text[pos + status.size()]))
				{
					return whitespaceBegin;
				}
			}
			return std::string::npos;
		}

		bool ParseMuxQuestPrompt(const std::string& source, std::vector<MuxQuestSegment>& segments)
		{
			segments.clear();

			std::string text = source;
			TrimAsciiWhitespace(text);
			if (text.empty())
				return false;

			size_t cursor = 0;
			while (cursor < text.size())
			{
				std::string status;
				if (!MatchStatusAt(text, cursor, status))
					return false;

				size_t bodyBegin = cursor + status.size();
				if (bodyBegin >= text.size() || !IsAsciiWhitespace(text[bodyBegin]))
					return false;
				while (bodyBegin < text.size() && IsAsciiWhitespace(text[bodyBegin]))
					++bodyBegin;
				if (bodyBegin >= text.size())
					return false;

				const size_t separator = FindNextStatusSeparator(text, bodyBegin);
				std::string body = separator == std::string::npos
					? text.substr(bodyBegin)
					: text.substr(bodyBegin, separator - bodyBegin);
				TrimAsciiWhitespace(body);
				if (body.empty())
					return false;

				segments.push_back({ std::move(status), std::move(body) });
				if (separator == std::string::npos)
					break;

				cursor = separator;
				while (cursor < text.size() && IsAsciiWhitespace(text[cursor]))
					++cursor;
			}

			return !segments.empty();
		}

		std::string TranslatePartOrSelf(const std::string& text, int depth)
		{
			if (text.empty() || depth >= 4)
				return text;

			std::string translated;
			if (TranslateInternal(text.c_str(), translated, depth + 1))
				return translated;
			return text;
		}
	}

	bool TryTranslateMuxQuestPrompt(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4)
			return false;

		std::vector<MuxQuestSegment> segments;
		if (!ParseMuxQuestPrompt(source, segments))
			return false;

		std::string result;
		for (const MuxQuestSegment& segment : segments)
		{
			if (!result.empty())
				result += '\n';

			result += TranslatePartOrSelf(segment.status, depth);
			result += ' ';
			result += TranslatePartOrSelf(segment.body, depth);
		}

		translated = std::move(result);
		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: mux quest prompt match:");
			gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   result=\"%s\"", translated.c_str());
		}
		return true;
	}

} // namespace fonthook
