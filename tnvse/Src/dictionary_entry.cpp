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

		std::string BuildDoorSourceFormat()
		{
			switch (g_uiReorderDoorPrompt)
			{
			case 1:
				return "to[%]{}";
			case 2:
				return "[%]to{}";
			default:
				return "{} to [%]";
			}
		}

		void AddWildcardIndex(size_t index)
		{
			const auto& entry = s_entries[index];
			const std::string* prefix = entry.tokens.empty() ? nullptr : &entry.tokens.front();
			const std::string* suffix = entry.tokens.empty() ? nullptr : &entry.tokens.back();

			if (prefix && !prefix->empty() && prefix->size() >= (suffix ? suffix->size() : 0))
			{
				s_wildcardPrefixIndex[*prefix].push_back(index);
			}
			else if (suffix && !suffix->empty())
			{
				s_wildcardSuffixIndex[*suffix].push_back(index);
			}
			else
			{
				s_wildcardLooseIndex.push_back(index);
			}
		}

		void SortIndexVector(std::vector<size_t>& indexes)
		{
			std::sort(indexes.begin(), indexes.end(), [](size_t a, size_t b)
				{
					return EntryLess(s_entries[a], s_entries[b]);
				});
		}

		void SortIndexMap(std::unordered_map<std::string, std::vector<size_t>>& indexMap)
		{
			for (auto& pair : indexMap)
				SortIndexVector(pair.second);
		}
	}

	// ---- record type detection ----

	// Parse GRUP + CHAMP fields (ESO-ESM Translator / DocumentElement format).
	RecordType DetectRecordTypeGrupChamp(const char* grup, const char* champ)
	{
		if (!grup || !*grup)
			return RecordType::Unknown;

		if (strcmp(grup, "BPTD") == 0)
			return RecordType::Bptd;
		if (strcmp(grup, "DOOR") == 0)
			return RecordType::Door;
		if (strcmp(grup, "INFO") == 0)
			return RecordType::Info;

		if (strcmp(grup, "CHAL") == 0)
		{
			if (champ && strcmp(champ, "FULL") == 0)
				return RecordType::ChallengeName;
			if (champ && strcmp(champ, "DESC") == 0)
				return RecordType::ChallengeDesc;
		}

		if (strcmp(grup, "MESG") == 0 && champ && strcmp(champ, "DESC") == 0)
			return RecordType::Message;
		if (strcmp(grup, "SCPT") == 0 && champ && strcmp(champ, "SCDA") == 0)
			return RecordType::Script;

		return RecordType::Unknown;
	}

	// Parse REC field (xTranslator / SSTXMLRessources format).
	RecordType DetectRecordTypeFromRec(const char* rec)
	{
		if (!rec || !*rec)
			return RecordType::Unknown;

		if (strncmp(rec, "BPTD:", 5) == 0)
			return RecordType::Bptd;
		if (strncmp(rec, "DOOR:", 5) == 0)
			return RecordType::Door;
		if (strcmp(rec, "CHAL:FULL") == 0)
			return RecordType::ChallengeName;
		if (strcmp(rec, "CHAL:DESC") == 0)
			return RecordType::ChallengeDesc;
		if (strcmp(rec, "MESG:DESC") == 0)
			return RecordType::Message;
		if (strcmp(rec, "SCPT:SCDA") == 0)
			return RecordType::Script;
		if (strncmp(rec, "INFO:", 5) == 0)
			return RecordType::Info;

		return RecordType::Unknown;
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
		{
			s_wildcardIndex.push_back(index);
			AddWildcardIndex(index);
		}

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

	// ---- XML record-type-aware registration ----

	void GenerateAutoEntries(const std::string& cleanSource,
	                         const std::string& cleanTarget,
	                         RecordType type,
	                         int priority)
	{
		const char* typeKey = nullptr;
		std::string sourceFormat;
		switch (type)
		{
		case RecordType::Bptd:          typeKey = "bptd";      sourceFormat = "[%]{} Crippled"; break;
		case RecordType::Door:          typeKey = "door";      sourceFormat = BuildDoorSourceFormat(); break;
		case RecordType::ChallengeName: typeKey = "chal_name"; sourceFormat = "{} [%]";         break;
		case RecordType::ChallengeDesc: typeKey = "chal_desc"; sourceFormat = "[%] {}";         break;
		default: return;
		}

		auto it = s_uiHintFormats.find(typeKey);
		if (it == s_uiHintFormats.end() || !it->second.enabled)
			return;

		const std::string& targetFormat = it->second.targetFormat;

		std::string autoSource(sourceFormat);
		ReplaceAll(autoSource, "{}", cleanSource);

		std::string autoTarget(targetFormat);
		ReplaceAll(autoTarget, "[@]", cleanTarget);

		{
				const bool toMb = g_usingWinEncoding != 0 && IsValidUTF8With3ByteMin(autoTarget.c_str());
				gLog.FormattedMessage("tnvse_dictionary:   uihint %s: \"%s\" ->\"%s\"",
					typeKey, autoSource.c_str(),
					toMb ? UTF8ToMultiByteStr(autoTarget, g_usingWinEncoding).c_str() : autoTarget.c_str());
			}

		RegisterText(std::move(autoSource), std::move(autoTarget), priority, {});
	}

	bool RegisterXmlEntry(const std::string& source, const std::string& target,
	                      RecordType type, int priority, const std::string& id)
	{
		if (source.empty() || target.empty())
			return false;

		std::string sourceText = source;
		std::string targetText = target;
		if (type == RecordType::Info)
		{
			RemoveBraceComments(sourceText);
			RemoveBraceComments(targetText);
		}

		std::wstring sourceWide = Utf8ToWide(sourceText);
		std::wstring targetWide = Utf8ToWide(targetText);
		Replace1252ForXml(sourceWide);
		std::string cleanSource = WideToUtf8(sourceWide);
		std::string cleanTarget = WideToUtf8(targetWide);

		const bool registered = RegisterText(cleanSource, cleanTarget, priority, id);

		if (registered && type != RecordType::Unknown)
			GenerateAutoEntries(cleanSource, cleanTarget, type, priority);

		return registered;
	}

	void RegisterXmlNodesTyped(pugi::xml_node parent,
	                           const char* nodeName,
	                           const char* sourceName,
	                           const char* targetName,
	                           const char* champName,
	                           int priority,
	                           bool isEET)
	{
		for (auto node : parent.children(nodeName))
		{
			const char* field = node.child(champName).text().as_string("");
			if (strstr(field, "SCTX"))
				continue;

			std::string source = node.child(sourceName).text().as_string("");
			std::string target = node.child(targetName).text().as_string("");
			if (strstr(field, "INFO GRUP"))
			{
				RemoveBraceComments(source);
				RemoveBraceComments(target);
			}

			RecordType type = RecordType::Unknown;
			if (isEET)
			{
				const char* grup = node.child("GRUP").text().as_string("");
				type = DetectRecordTypeGrupChamp(grup, field);
			}
			else
			{
				type = DetectRecordTypeFromRec(field);
			}

			RegisterXmlEntry(source, target, type, priority);
		}
	}

	// ---- index sorting ----

	void SortIndexes()
	{
		for (auto& pair : s_exactIndex)
			SortIndexVector(pair.second);

		SortIndexVector(s_wildcardIndex);
		SortIndexMap(s_wildcardPrefixIndex);
		SortIndexMap(s_wildcardSuffixIndex);
		SortIndexVector(s_wildcardLooseIndex);
	}

} // namespace fonthook
