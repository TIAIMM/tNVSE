#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <cstring>

namespace fonthook
{
	namespace implementation::dictionary_entry {}
	using namespace implementation::dictionary_entry;

	namespace implementation::dictionary_entry
	{
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
			if (g_bEnableDictionaryTranslationLog)
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
		{
			entry.tokens = SplitByToken(source, kBindSymbol);
			entry.hasAsciiLiteralAnchor = std::any_of(
				entry.tokens.begin(), entry.tokens.end(),
				[](const std::string& token)
				{
					return HasAlphabet(token);
				});
		}

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

			// Deduplicate: same auto-source key from different XML nodes (BDD/BP/ESP)
			if (!s_registeredAutoKeys.insert(autoSource).second)
				return;
		std::string autoTarget(targetFormat);
		ReplaceAll(autoTarget, "[@]", cleanTarget);

		if (g_bEnableDictionaryTranslationLog)
			{
				const bool toMb = IsEastAsianUiMode()
					&& IsValidUTF8With3ByteMin(autoTarget.c_str());
				const std::string logTarget = toMb ? UTF8ToMultiByteStr(autoTarget, g_usingWinEncoding) : autoTarget;
				gLog.FormattedMessage("tnvse_dictionary:   uihint %s: \"%s\" ->\"%s\"",
					typeKey, autoSource.c_str(), logTarget.c_str());
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
