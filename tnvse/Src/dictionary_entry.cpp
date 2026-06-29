#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <cstring>

namespace fonthook
{
	namespace
	{
		std::vector<std::string> GenerateBraceVariants(const std::string& text)
		{
			const std::string startToken = "%{";
			const std::string endToken = "%}";
			const size_t count = CountToken(text, startToken);
			if (count == 0 || count != CountToken(text, endToken) || count > 12)
				return {};

			std::vector<std::string> variants;
			const size_t limit = size_t(1) << count;
			variants.reserve(limit);
			for (size_t mask = 0; mask < limit; ++mask)
			{
				std::string copy = text;
				size_t search = 0;
				size_t bit = 1;
				while (true)
				{
					const size_t begin = copy.find(startToken, search);
					if (begin == std::string::npos)
						break;
					const size_t end = copy.find(endToken, begin);
					if (end == std::string::npos)
						break;
					if (mask & bit)
					{
						copy.erase(begin, end - begin);
						search = begin;
					}
					else
					{
						search = begin + startToken.size();
					}
					bit <<= 1;
				}
				ReplaceAll(copy, startToken, "");
				ReplaceAll(copy, endToken, "");
				variants.push_back(std::move(copy));
			}
			return variants;
		}

		void RegisterMessageVariants(const std::string& source, const std::string& target, int priority)
		{
			auto sourceParts = SplitByToken(source, "|");
			auto targetParts = SplitByCharDBCS(target, '|');
			if (sourceParts.size() == targetParts.size() && sourceParts.size() > 1)
			{
				for (size_t i = 0; i < sourceParts.size(); ++i)
					RegisterText(sourceParts[i], targetParts[i], priority, {});
			}

			auto sourceVariants = GenerateBraceVariants(source);
			auto targetVariants = GenerateBraceVariants(target);
			if (!sourceVariants.empty() && sourceVariants.size() == targetVariants.size())
			{
				for (size_t i = 0; i < sourceVariants.size(); ++i)
					RegisterText(sourceVariants[i], targetVariants[i], priority, {});
			}
		}

		void RegisterXmlText(const std::string& source, const std::string& target, int priority)
		{
			if (source.empty() || target.empty())
				return;

			std::wstring sourceWide = Utf8ToWide(source);
			std::wstring targetWide = Utf8ToWide(target);
			Replace1252ForXml(sourceWide);
			std::string processedSource = WideToUtf8(sourceWide);
			std::string processedTarget = WideToUtf8(targetWide);
			RegisterText(processedSource, processedTarget, priority, {});
		}

		void RemoveBraceComments(std::string& text)
		{
			std::string result;
			result.reserve(text.size());

			size_t cursor = 0;
			while (cursor < text.size())
			{
				if (text[cursor] != '{')
				{
					result.push_back(text[cursor++]);
					continue;
				}

				size_t depth = 1;
				size_t end = cursor + 1;
				for (; end < text.size(); ++end)
				{
					if (text[end] == '{')
					{
						++depth;
					}
					else if (text[end] == '}')
					{
						--depth;
						if (depth == 0)
							break;
					}
				}

				if (end == text.size())
				{
					result.append(text.substr(cursor));
					break;
				}

				cursor = end + 1;
			}

			text.swap(result);
			CollapseSpaces(text);
			Trim(text);
		}
	}

	// ---- entry comparison ----

	bool EntryLess(const DictionaryEntry& left, const DictionaryEntry& right)
	{
		if (left.isExact != right.isExact)
			return left.isExact;
		if (left.priority != right.priority)
			return left.priority < right.priority;
		if (left.lengthWithoutBinds != right.lengthWithoutBinds)
			return left.lengthWithoutBinds > right.lengthWithoutBinds;
		if (left.bindCount != right.bindCount)
			return left.bindCount < right.bindCount;
		return left.order < right.order;
	}

	// ---- entry registration ----

	bool AddEntry(const std::string& source, const std::string& target, int priority, const std::string& id)
	{
		if (source.empty() || target.empty())
			return false;

		const size_t sourceBindCount = CountToken(source, kBindSymbol);
		const size_t targetBindCount = CountTargetBindToken(target);
		if (sourceBindCount != targetBindCount)
		{
			gLog.FormattedMessage("tnvse_dictionary: skipped bind-count mismatch: %s", source.c_str());
			return false;
		}

		DictionaryEntry entry;
		entry.key = source;
		entry.target = target;
		entry.priority = priority;
		entry.order = s_entries.size();
		entry.bindCount = sourceBindCount;
		entry.lengthWithoutBinds = LengthWithoutBinds(source);
		entry.isExact = sourceBindCount == 0;
		if (!entry.isExact)
			entry.tokens = SplitByToken(source, kBindSymbol);

		const size_t index = s_entries.size();
		s_entries.push_back(std::move(entry));
		if (s_entries[index].isExact)
			s_exactIndex[s_entries[index].key].push_back(index);
		else
			s_wildcardIndex.push_back(index);

		if (!id.empty())
			s_idIndex[id] = index;

		return true;
	}

	bool RegisterText(std::string source, std::string target, int priority, const std::string& id)
	{
		source = PrepareSourceForRegistration(std::move(source));
		target = PrepareTarget(std::move(target));
		return AddEntry(source, target, priority, id);
	}

	// ---- XML helpers ----

	void RegisterXmlNodes(pugi::xml_node parent, const char* nodeName, const char* sourceName, const char* targetName, const char* fieldName, int priority)
	{
		for (auto node : parent.children(nodeName))
		{
			const char* field = node.child(fieldName).text().as_string("");
			if (strstr(field, "SCTX"))
				continue;
			std::string source = node.child(sourceName).text().as_string("");
			std::string target = node.child(targetName).text().as_string("");
			if (strstr(field, "INFO GRUP"))
			{
				RemoveBraceComments(source);
				RemoveBraceComments(target);
			}
			RegisterXmlText(source, target, priority);
		}
	}

	// ---- index sorting ----

	void SortIndexes()
	{
		for (auto& pair : s_exactIndex)
		{
			auto& indexes = pair.second;
			std::sort(indexes.begin(), indexes.end(), [](size_t a, size_t b)
				{
					return EntryLess(s_entries[a], s_entries[b]);
				});
		}

		std::sort(s_wildcardIndex.begin(), s_wildcardIndex.end(), [](size_t a, size_t b)
			{
				return EntryLess(s_entries[a], s_entries[b]);
			});
	}

} // namespace fonthook
