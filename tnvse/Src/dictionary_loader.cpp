#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <cstring>

namespace fonthook
{
	namespace
	{
		std::string GetAttr(pugi::xml_node node, const char* name)
		{
			return node.attribute(name).as_string("");
		}

		int GetAttrInt(pugi::xml_node node, const char* name, int fallback)
		{
			return node.attribute(name).as_int(fallback);
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

		pugi::xml_node root = doc.first_child();
		if (!root || std::string(root.name()) != "dictionary")
		{
			gLog.FormattedMessage("tnvse_dictionary: unsupported config root");
			return;
		}

		for (auto node : root.children("file"))
			LoadFileNode(node);

		for (auto node : root.children("directory"))
			LoadDirectoryNode(node);

		for (auto node : root.children("xml"))
			LoadXmlNode(node);

		SortIndexes();
		s_dictionaryLoaded = !s_entries.empty();
		gLog.FormattedMessage("tnvse_dictionary: loaded %u entries", static_cast<UInt32>(s_entries.size()));
	}

} // namespace fonthook
