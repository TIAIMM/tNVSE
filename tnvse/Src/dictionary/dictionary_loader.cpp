#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>

namespace fonthook
{
	namespace implementation::dictionary_loader {}
	using namespace implementation::dictionary_loader;

	namespace implementation::dictionary_loader
	{
		struct StageFilter
		{
			bool enabled = false;
			std::unordered_set<int> stages;
		};

		struct TextFileLoadStats
		{
			bool loaded = false;
			UInt32 registered = 0;
			UInt32 sourceLines = 0;
			UInt32 targetLines = 0;
			UInt32 paired = 0;
			UInt32 sourceIds = 0;
			UInt32 malformedSource = 0;
			UInt32 malformedTarget = 0;
			UInt32 missingSource = 0;
			UInt32 invalid = 0;
		};

		struct DirectoryLoadStats
		{
			bool loaded = false;
			UInt32 files = 0;
			UInt32 registered = 0;
			UInt32 missingTarget = 0;
			UInt32 invalid = 0;
		};

		std::string GetAttr(pugi::xml_node node, const char* name)
		{
			return node.attribute(name).as_string("");
		}

		int GetAttrInt(pugi::xml_node node, const char* name, int fallback)
		{
			return node.attribute(name).as_int(fallback);
		}

		bool ParseInt(const std::string& text, int& value)
		{
			if (text.empty())
				return false;

			char* end = nullptr;
			const long parsed = std::strtol(text.c_str(), &end, 10);
			if (end == text.c_str() || *end != '\0')
				return false;

			value = static_cast<int>(parsed);
			return true;
		}

		StageFilter ParseStageFilter(pugi::xml_node node)
		{
			StageFilter filter;
			auto attr = node.attribute("stage");
			if (!attr)
				return filter;

			std::string stageText = attr.as_string("");
			Trim(stageText);
			if (stageText.empty() || stageText == "*")
				return filter;

			filter.enabled = true;
			for (auto token : SplitByToken(stageText, ","))
			{
				Trim(token);
				int stage = 0;
				if (ParseInt(token, stage))
					filter.stages.insert(stage);
				else if (!token.empty())
					gLog.FormattedMessage("tnvse_dictionary: ignored invalid JSON stage filter token: %s", token.c_str());
			}
			return filter;
		}

		bool ReadJsonString(const nlohmann::json& item, const char* name, std::string& value)
		{
			auto it = item.find(name);
			if (it == item.end() || !it->is_string())
				return false;

			value = it->get<std::string>();
			return true;
		}

		bool ReadJsonStage(const nlohmann::json& item, int& stage)
		{
			auto it = item.find("stage");
			if (it == item.end())
				return false;

			if (it->is_number_integer())
			{
				stage = it->get<int>();
				return true;
			}

			if (it->is_string())
			{
				std::string value = it->get<std::string>();
				Trim(value);
				return ParseInt(value, stage);
			}

			return false;
		}

		bool AllowsStage(const StageFilter& filter, const nlohmann::json& item)
		{
			if (!filter.enabled)
				return true;

			int stage = 0;
			return ReadJsonStage(item, stage) && filter.stages.find(stage) != filter.stages.end();
		}

		RecordType DetectRecordTypeFromJsonKey(const std::string& key)
		{
			const size_t firstHash = key.find('#');
			if (firstHash == std::string::npos)
				return RecordType::Unknown;

			const size_t secondHash = key.find('#', firstHash + 1);
			const size_t recLength = secondHash == std::string::npos
				? std::string::npos
				: secondHash - firstHash - 1;
			std::string rec = key.substr(firstHash + 1, recLength);
			if (rec.size() < 4)
				return RecordType::Unknown;

			const size_t colon = rec.find(':');
			std::string grup = colon == std::string::npos ? rec.substr(0, 4) : rec.substr(0, colon);
			std::string field = colon == std::string::npos ? std::string() : rec.substr(colon + 1);
			if (grup.size() != 4)
				return RecordType::Unknown;

			return DetectRecordTypeGrupChamp(grup.c_str(), field.c_str());
		}

		TextFileLoadStats LoadFileDictionaryType1(const std::string& sourcePath, const std::string& targetPath, int priority)
		{
			TextFileLoadStats stats;
			std::string sourceText;
			std::string targetText;
			if (!ReadWholeFile(sourcePath, sourceText) || !ReadWholeFile(targetPath, targetText))
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to read file dictionary type=1: %s / %s",
					sourcePath.c_str(), targetPath.c_str());
				return stats;
			}
			stats.loaded = true;

			auto sourceLines = SplitLines(sourceText);
			auto targetLines = SplitLines(targetText);
			stats.sourceLines = static_cast<UInt32>(sourceLines.size());
			stats.targetLines = static_cast<UInt32>(targetLines.size());
			const size_t count = std::min(sourceLines.size(), targetLines.size());
			stats.paired = static_cast<UInt32>(count);
			for (size_t i = 0; i < count; ++i)
			{
				if (RegisterText(sourceLines[i], targetLines[i], priority, {}))
					++stats.registered;
				else
					++stats.invalid;
			}
			return stats;
		}

		TextFileLoadStats LoadFileDictionaryType2(const std::string& sourcePath, const std::string& targetPath, int priority)
		{
			TextFileLoadStats stats;
			std::string sourceText;
			std::string targetText;
			if (!ReadWholeFile(sourcePath, sourceText) || !ReadWholeFile(targetPath, targetText))
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to read file dictionary type=2: %s / %s",
					sourcePath.c_str(), targetPath.c_str());
				return stats;
			}
			stats.loaded = true;

			std::unordered_map<std::string, std::string> sourceById;
			for (auto& line : SplitLines(sourceText))
			{
				++stats.sourceLines;
				StripUtf8Bom(line);
				const auto [id, text] = SplitIdLine(line);
				if (!id.empty())
				{
					if (sourceById.emplace(id, text).second)
						++stats.sourceIds;
				}
				else
				{
					++stats.malformedSource;
				}
			}

			for (auto& line : SplitLines(targetText))
			{
				++stats.targetLines;
				StripUtf8Bom(line);
				const auto [id, target] = SplitIdLine(line);
				if (id.empty())
				{
					++stats.malformedTarget;
					continue;
				}
				auto it = sourceById.find(id);
				if (it != sourceById.end())
				{
					++stats.paired;
					if (RegisterText(it->second, target, priority, id))
						++stats.registered;
					else
						++stats.invalid;
				}
				else
				{
					++stats.missingSource;
				}
			}
			return stats;
		}

		DirectoryLoadStats LoadDirectoryDictionary(const std::string& sourceDir, const std::string& targetDir, int priority)
		{
			DirectoryLoadStats stats;
			if (!DirectoryExists(sourceDir) || !DirectoryExists(targetDir))
			{
				gLog.FormattedMessage("tnvse_dictionary: missing directory dictionary: %s / %s",
					sourceDir.c_str(), targetDir.c_str());
				return stats;
			}
			stats.loaded = true;

			for (const auto& fileName : FindFiles(sourceDir, "*.txt"))
			{
				++stats.files;
				std::string sourceText;
				std::string targetText;
				const std::string sourcePath = sourceDir + "\\" + fileName;
				const std::string targetPath = targetDir + "\\" + fileName;
				if (ReadWholeFile(sourcePath, sourceText) && ReadWholeFile(targetPath, targetText))
				{
					if (RegisterText(sourceText, targetText, priority, {}))
						++stats.registered;
					else
						++stats.invalid;
				}
				else
				{
					++stats.missingTarget;
					gLog.FormattedMessage("tnvse_dictionary: missing directory dictionary pair: %s / %s",
						sourcePath.c_str(), targetPath.c_str());
				}
			}
			return stats;
		}

		void LoadXmlDictionary(const std::string& path, int priority)
		{
			const size_t beforeEntries = s_entries.size();
			pugi::xml_document doc;
			pugi::xml_parse_result result = doc.load_file(path.c_str());
			if (!result)
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to load XML dictionary: %s", path.c_str());
				return;
			}

			pugi::xml_node root = doc.first_child();
			if (!root)
			{
				gLog.FormattedMessage("tnvse_dictionary: empty XML dictionary: %s", path.c_str());
				return;
			}

			const std::string rootName = root.name();
			if (rootName == "DocumentElement")
			{
				RegisterXmlNodesTyped(root, "BDD", "ORIGINAL", "TRADUIT", "CHAMP", priority, true);
				RegisterXmlNodesTyped(root, "BP", "ORIGINAL", "TRADUIT", "CHAMP", priority, true);
				RegisterXmlNodesTyped(root, "ESP", "ORIGINAL", "TRADUIT", "CHAMP", priority, true);
			}
			else if (rootName == "SSTXMLRessources")
			{
				auto content = root.child("Content");
				if (content)
					RegisterXmlNodesTyped(content, "String", "Source", "Dest", "REC", priority, false);
			}
			else
			{
				gLog.FormattedMessage("tnvse_dictionary: unsupported XML dictionary root in %s", path.c_str());
				return;
			}

			gLog.FormattedMessage("tnvse_dictionary: loaded XML dictionary: %s, root=%s, priority=%d, entries_added=%u",
				path.c_str(),
				rootName.c_str(),
				priority,
				static_cast<UInt32>(s_entries.size() - beforeEntries));
		}

		void LoadJsonDictionary(const std::string& path, int priority, const StageFilter& stageFilter)
		{
			const size_t beforeEntries = s_entries.size();
			std::string text;
			if (!ReadWholeFile(path, text))
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to read JSON dictionary: %s", path.c_str());
				return;
			}

			StripUtf8Bom(text);

			nlohmann::json doc;
			try
			{
				doc = nlohmann::json::parse(text);
			}
			catch (const std::exception& e)
			{
				gLog.FormattedMessage("tnvse_dictionary: failed to parse JSON dictionary: %s (%s)", path.c_str(), e.what());
				return;
			}

			if (!doc.is_array())
			{
				gLog.FormattedMessage("tnvse_dictionary: unsupported JSON dictionary root: %s", path.c_str());
				return;
			}

			UInt32 registered = 0;
			UInt32 skippedStage = 0;
			UInt32 skippedMalformed = 0;
			UInt32 skippedInvalid = 0;
			for (const auto& item : doc)
			{
				if (!item.is_object())
				{
					++skippedMalformed;
					continue;
				}

				if (!AllowsStage(stageFilter, item))
				{
					++skippedStage;
					continue;
				}

				std::string original;
				std::string translation;
				if (!ReadJsonString(item, "original", original) || !ReadJsonString(item, "translation", translation))
				{
					++skippedMalformed;
					continue;
				}

				std::string id;
				ReadJsonString(item, "key", id);
				const RecordType type = DetectRecordTypeFromJsonKey(id);
				if (RegisterXmlEntry(original, translation, type, priority, id))
					++registered;
				else
					++skippedInvalid;
			}

			gLog.FormattedMessage(
				"tnvse_dictionary: loaded JSON dictionary: %s, priority=%d, entries_added=%u, registered=%u, skipped_stage=%u, skipped_malformed=%u, skipped_invalid=%u, stage_filter=%s",
				path.c_str(),
				priority,
				static_cast<UInt32>(s_entries.size() - beforeEntries),
				registered,
				skippedStage,
				skippedMalformed,
				skippedInvalid,
				stageFilter.enabled ? "on" : "off");
		}

		void LoadFileNode(pugi::xml_node node)
		{
			const std::string source = ResolvePath(GetAttr(node, "src"));
			const std::string target = ResolvePath(GetAttr(node, "target"));
			const int type = GetAttrInt(node, "type", 1);
			const int priority = GetAttrInt(node, "priority", 10);
			if (!FileExists(source) || !FileExists(target))
			{
				gLog.FormattedMessage("tnvse_dictionary: missing file dictionary: %s / %s", source.c_str(), target.c_str());
				return;
			}
			if (type == 2)
			{
				const TextFileLoadStats stats = LoadFileDictionaryType2(source, target, priority);
				gLog.FormattedMessage(
					"tnvse_dictionary: file dictionary type=2 status=%s priority=%d entries=%u source_ids=%u source_lines=%u target_lines=%u paired=%u skipped_no_source=%u skipped_malformed=%u skipped_invalid=%u source=%s target=%s",
					stats.loaded ? "loaded" : "read_failed",
					priority,
					stats.registered,
					stats.sourceIds,
					stats.sourceLines,
					stats.targetLines,
					stats.paired,
					stats.missingSource,
					stats.malformedSource + stats.malformedTarget,
					stats.invalid,
					source.c_str(),
					target.c_str());
			}
			else
			{
				const TextFileLoadStats stats = LoadFileDictionaryType1(source, target, priority);
				gLog.FormattedMessage(
					"tnvse_dictionary: file dictionary type=1 status=%s priority=%d entries=%u source_lines=%u target_lines=%u paired=%u skipped_unpaired=%u skipped_invalid=%u source=%s target=%s",
					stats.loaded ? "loaded" : "read_failed",
					priority,
					stats.registered,
					stats.sourceLines,
					stats.targetLines,
					stats.paired,
					static_cast<UInt32>((stats.sourceLines > stats.targetLines)
						? stats.sourceLines - stats.targetLines
						: stats.targetLines - stats.sourceLines),
					stats.invalid,
					source.c_str(),
					target.c_str());
			}
		}

		void LoadDirectoryNode(pugi::xml_node node)
		{
			const std::string source = ResolvePath(GetAttr(node, "src"));
			const std::string target = ResolvePath(GetAttr(node, "target"));
			const int priority = GetAttrInt(node, "priority", 10);
			const DirectoryLoadStats stats = LoadDirectoryDictionary(source, target, priority);
			gLog.FormattedMessage(
				"tnvse_dictionary: directory dictionary status=%s priority=%d entries=%u files=%u missing_pairs=%u skipped_invalid=%u source=%s target=%s",
				stats.loaded ? "loaded" : "missing_directory",
				priority,
				stats.registered,
				stats.files,
				stats.missingTarget,
				stats.invalid,
				source.c_str(),
				target.c_str());
		}

		void LoadXmlNode(pugi::xml_node node)
		{
			std::string path = GetAttr(node, "src");
			if (path.empty())
				path = GetAttr(node, "path");
			const int priority = GetAttrInt(node, "priority", 10);
			path = ResolvePath(path);
			if (!FileExists(path))
			{
				gLog.FormattedMessage("tnvse_dictionary: missing XML dictionary: %s", path.c_str());
				return;
			}
			LoadXmlDictionary(path, priority);
		}

		void LoadJsonNode(pugi::xml_node node)
		{
			std::string path = GetAttr(node, "path");
			if (path.empty())
				path = GetAttr(node, "src");
			const int priority = GetAttrInt(node, "priority", 10);
			const StageFilter stageFilter = ParseStageFilter(node);
			path = ResolvePath(path);
			if (!FileExists(path))
			{
				gLog.FormattedMessage("tnvse_dictionary: missing JSON dictionary: %s", path.c_str());
				return;
			}
			LoadJsonDictionary(path, priority, stageFilter);
		}
	}

	// ---- UI hint config loader ----

	void LoadUiHintConfig(pugi::xml_node root)
	{
		s_uiHintFormats.clear();
		auto uiHintNode = root.child("uihint");
		if (!uiHintNode)
			return;

		static const char* kTypeNames[] = { "bptd", "door", "chal_name", "chal_desc" };
		for (const char* typeName : kTypeNames)
		{
			auto node = uiHintNode.child(typeName);
			if (!node)
				continue;

			std::string format = node.attribute("format").as_string("");
			Trim(format);
			if (format.empty())
				continue;

			UiHintFormat hint;
			hint.targetFormat = std::move(format);
			hint.enabled = true;
			s_uiHintFormats[typeName] = std::move(hint);

			const std::string& fmt = s_uiHintFormats[typeName].targetFormat;
			const bool toMb = IsEastAsianUiMode()
				&& IsValidUTF8With3ByteMin(fmt.c_str());
			gLog.FormattedMessage("tnvse_dictionary: loaded uihint type=%s target_format=\"%s\"",
				typeName, toMb ? UTF8ToMultiByteStr(fmt, g_usingWinEncoding).c_str() : fmt.c_str());
		}
	}

	// ---- main config loader (public API) ----

	void LoadDictionaryConfig()
	{
		s_entries.clear();
		s_exactIndex.clear();
		s_windows1252ExactIndex.clear();
		s_windows1252WildcardIndex.clear();
		s_wildcardIndex.clear();
		s_wildcardPrefixIndex.clear();
		s_wildcardSuffixIndex.clear();
		s_wildcardLooseIndex.clear();
		s_idIndex.clear();
		s_positiveCache.clear();
		s_negativeCache.clear();
		s_positiveCacheOrder.clear();
		s_negativeCacheOrder.clear();
		s_registeredAutoKeys.clear();
		s_dictionaryLoaded = false;
		ResetFuzzyTextConfig();
		ResetPerkRequirementConfig();

		if (!g_bEnableDictionaryTranslation)
		{
			gLog.FormattedMessage("tnvse_dictionary: disabled by tnvse.ini");
			return;
		}

		if (!IsEastAsianUiMode())
		{
			gLog.FormattedMessage(
				"tnvse_dictionary: disabled because uiEncoding=0 is Windows-1252 single-byte mode");
			return;
		}

		const std::string configPath = GetGameDirectory() + "\\Data\\nvse\\plugins\\tnvse_dictionary.xml";
		if (!FileExists(configPath))
		{
			gLog.FormattedMessage("tnvse_dictionary: config not found: %s", configPath.c_str());
			return;
		}

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file(configPath.c_str());
		if (!result)
		{
			gLog.FormattedMessage("tnvse_dictionary: failed to load config: %s", configPath.c_str());
			return;
		}

		pugi::xml_node tnvseRoot = doc.first_child();
		if (!tnvseRoot || std::string(tnvseRoot.name()) != "tNVSE")
		{
			gLog.FormattedMessage("tnvse_dictionary: unsupported config root");
			return;
		}

		LoadFuzzyTextConfig(tnvseRoot);
		LoadPerkRequirementConfig(tnvseRoot);
		LoadUiHintConfig(tnvseRoot);

		pugi::xml_node dictNode = tnvseRoot.child("dictionary");
		if (dictNode)
		{
			for (auto node : dictNode.children("file"))
				LoadFileNode(node);

			for (auto node : dictNode.children("directory"))
				LoadDirectoryNode(node);

			for (auto node : dictNode.children("xml"))
				LoadXmlNode(node);

			for (auto node : dictNode.children("json"))
				LoadJsonNode(node);
		}
		else
		{
			gLog.FormattedMessage("tnvse_dictionary: missing <dictionary> node");
		}

		SortIndexes();
		s_dictionaryLoaded = !s_entries.empty() || HasTranslationRegexRules();
		size_t windows1252AliasCount = 0;
		for (const auto& pair : s_windows1252ExactIndex)
			windows1252AliasCount += pair.second.size();
		gLog.FormattedMessage(
			"tnvse_dictionary: Windows-1252 raw index exact_keys=%u exact_aliases=%u wildcard_aliases=%u",
			static_cast<UInt32>(s_windows1252ExactIndex.size()),
			static_cast<UInt32>(windows1252AliasCount),
			static_cast<UInt32>(s_windows1252WildcardIndex.size()));
		gLog.FormattedMessage("tnvse_dictionary: loaded %u entries, %u regex rules",
			static_cast<UInt32>(s_entries.size()),
			static_cast<UInt32>(GetTranslationRegexRuleCount()));
	}

} // namespace fonthook
