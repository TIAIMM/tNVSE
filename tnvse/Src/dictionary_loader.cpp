#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>

namespace fonthook
{
	namespace
	{
		struct StageFilter
		{
			bool enabled = false;
			std::unordered_set<int> stages;
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

		void LoadFileDictionaryType1(const std::string& sourcePath, const std::string& targetPath, int priority)
		{
			std::string sourceText;
			std::string targetText;
			if (!ReadWholeFile(sourcePath, sourceText) || !ReadWholeFile(targetPath, targetText))
				return;

			auto sourceLines = SplitLines(sourceText);
			auto targetLines = SplitLines(targetText);
			const size_t count = std::min(sourceLines.size(), targetLines.size());
			for (size_t i = 0; i < count; ++i)
				RegisterText(sourceLines[i], targetLines[i], priority, {});
		}

		void LoadFileDictionaryType2(const std::string& sourcePath, const std::string& targetPath, int priority)
		{
			std::string sourceText;
			std::string targetText;
			if (!ReadWholeFile(sourcePath, sourceText) || !ReadWholeFile(targetPath, targetText))
				return;

			std::unordered_map<std::string, std::string> sourceById;
			for (auto& line : SplitLines(sourceText))
			{
				StripUtf8Bom(line);
				const auto [id, text] = SplitIdLine(line);
				if (!id.empty())
					sourceById.emplace(id, text);
			}

			for (auto& line : SplitLines(targetText))
			{
				StripUtf8Bom(line);
				const auto [id, target] = SplitIdLine(line);
				if (id.empty())
					continue;
				auto it = sourceById.find(id);
				if (it != sourceById.end())
					RegisterText(it->second, target, priority, id);
			}
		}

		void LoadDirectoryDictionary(const std::string& sourceDir, const std::string& targetDir, int priority)
		{
			if (!DirectoryExists(sourceDir) || !DirectoryExists(targetDir))
				return;

			for (const auto& fileName : FindFiles(sourceDir, "*.txt"))
			{
				std::string sourceText;
				std::string targetText;
				const std::string sourcePath = sourceDir + "\\" + fileName;
				const std::string targetPath = targetDir + "\\" + fileName;
				if (ReadWholeFile(sourcePath, sourceText) && ReadWholeFile(targetPath, targetText))
					RegisterText(sourceText, targetText, priority, {});
			}
		}

		void LoadXmlDictionary(const std::string& path, int priority)
		{
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
				RegisterXmlNodes(root, "BDD", "ORIGINAL", "TRADUIT", "CHAMP", priority);
				RegisterXmlNodes(root, "BP", "ORIGINAL", "TRADUIT", "CHAMP", priority);
				RegisterXmlNodes(root, "ESP", "ORIGINAL", "TRADUIT", "CHAMP", priority);
			}
			else if (rootName == "SSTXMLRessources")
			{
				auto content = root.child("Content");
				if (content)
					RegisterXmlNodes(content, "String", "Source", "Dest", "REC", priority);
			}
			else
			{
				gLog.FormattedMessage("tnvse_dictionary: unsupported XML dictionary root in %s", path.c_str());
			}
		}

		void LoadJsonDictionary(const std::string& path, int priority, const StageFilter& stageFilter)
		{
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
				if (RegisterText(original, translation, priority, id))
					++registered;
				else
					++skippedInvalid;
			}

			gLog.FormattedMessage(
				"tnvse_dictionary: loaded JSON dictionary: %s, registered=%u, skipped_stage=%u, skipped_malformed=%u, skipped_invalid=%u",
				path.c_str(),
				registered,
				skippedStage,
				skippedMalformed,
				skippedInvalid);
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
				LoadFileDictionaryType2(source, target, priority);
			else
				LoadFileDictionaryType1(source, target, priority);
		}

		void LoadDirectoryNode(pugi::xml_node node)
		{
			const std::string source = ResolvePath(GetAttr(node, "src"));
			const std::string target = ResolvePath(GetAttr(node, "target"));
			const int priority = GetAttrInt(node, "priority", 10);
			LoadDirectoryDictionary(source, target, priority);
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

	// ---- main config loader (public API) ----

	void LoadDictionaryConfig()
	{
		s_entries.clear();
		s_exactIndex.clear();
		s_wildcardIndex.clear();
		s_idIndex.clear();
		s_positiveCache.clear();
		s_negativeCache.clear();
		s_dictionaryLoaded = false;
		ResetFuzzyTextConfig();

		if (!g_bEnableDictionaryTranslation)
		{
			gLog.FormattedMessage("tnvse_dictionary: disabled by tnvse.ini");
			return;
		}

		if (g_usingWinEncoding == 0)
		{
			gLog.FormattedMessage("tnvse_dictionary: disabled because uiEncoding is English");
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

		pugi::xml_node dictNode = tnvseRoot.child("dictionary");
		if (!dictNode)
		{
			gLog.FormattedMessage("tnvse_dictionary: missing <dictionary> node");
			return;
		}

		LoadFuzzyTextConfig(tnvseRoot);

		for (auto node : dictNode.children("file"))
			LoadFileNode(node);

		for (auto node : dictNode.children("directory"))
			LoadDirectoryNode(node);

		for (auto node : dictNode.children("xml"))
			LoadXmlNode(node);

		for (auto node : dictNode.children("json"))
			LoadJsonNode(node);

		SortIndexes();
		s_dictionaryLoaded = !s_entries.empty();
		gLog.FormattedMessage("tnvse_dictionary: loaded %u entries", static_cast<UInt32>(s_entries.size()));
	}

} // namespace fonthook
