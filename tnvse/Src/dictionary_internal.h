#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../include/pugixml/pugixml.hpp"

namespace fonthook
{

	// ---- bind symbol ----
	inline constexpr const char* kBindSymbol = "[%]";

	// ---- cache limits ----
	inline constexpr size_t kPositiveCacheLimit = 10000;
	inline constexpr size_t kNegativeCacheLimit = 20000;

	// ---- entry struct ----
	struct DictionaryEntry
	{
		std::string key;
		std::string target;
		std::vector<std::string> tokens;
		int priority = 10;
		size_t order = 0;
		size_t bindCount = 0;
		size_t lengthWithoutBinds = 0;
		bool isExact = true;
	};

	// ---- record type for XML auto-entry generation ----
	enum class RecordType : UInt8
	{
		Unknown = 0,
		Bptd,          // body part data (BPTD:BPTN or GRUP=BPTD)
		Door,          // door record (DOOR:FULL or GRUP=DOOR)
		ChallengeName, // challenge name (CHAL:FULL)
		ChallengeDesc, // challenge description (CHAL:DESC)
		Message,       // message (MESG:DESC)
		Script,        // script (SCPT:SCDA)
	};

	// ---- UI hint format for auto-generated entries ----
	struct UiHintFormat
	{
		std::string targetFormat; // e.g. "[%]的[@]残废了" — [@] is replaced at registration time
		bool enabled = false;
	};

	// ---- global state (defined in dictionary.cpp) ----
	extern std::vector<DictionaryEntry> s_entries;
	extern std::unordered_map<std::string, std::vector<size_t>> s_exactIndex;
	extern std::vector<size_t> s_wildcardIndex;
	extern std::unordered_map<std::string, size_t> s_idIndex;
	extern std::unordered_map<std::string, std::string> s_positiveCache;
	extern std::unordered_set<std::string> s_negativeCache;
	extern std::unordered_map<std::string, UiHintFormat> s_uiHintFormats;
	extern bool s_dictionaryLoaded;

	// ---- OS / file utilities (dictionary_utils.cpp) ----
	std::string GetGameDirectory();
	bool IsAbsolutePath(const std::string& path);
	std::string ResolvePath(std::string path);
	bool FileExists(const std::string& path);
	bool DirectoryExists(const std::string& path);
	std::string WideToUtf8(const std::wstring& value);
	std::wstring Utf8ToWide(const std::string& str);
	bool ReadWholeFile(const std::string& path, std::string& out);
	std::vector<std::string> FindFiles(const std::string& directory, const char* pattern);

	// ---- string utilities (dictionary_utils.cpp) ----
	void ReplaceAll(std::string& text, std::string_view from, std::string_view to);
	size_t CountToken(std::string_view text, std::string_view token);
	void StripUtf8Bom(std::string& text);
	void Trim(std::string& text);
	void CollapseSpaces(std::string& text);
	void ToLowerAscii(std::string& text);
	bool HasAlphabet(std::string_view text);
	void RemoveControlChars(std::string& text);
	void Correct1252ToAscii(std::string& text);
	void Replace1252ForXml(std::wstring& text);
	void RemoveAlignmentTag(std::string& text);
	std::vector<std::string> SplitByToken(std::string_view text, std::string_view token);
	std::vector<std::string> SplitByCharDBCS(const std::string& text, char delimiter);
	std::vector<std::string> SplitLines(const std::string& text);
	std::pair<std::string, std::string> SplitIdLine(const std::string& line);

	// ---- text preparation (dictionary_prepare.cpp) ----
	size_t CountTargetBindToken(const std::string& text);
	std::vector<std::string> SplitTargetByBindToken(const std::string& text);
	size_t LengthWithoutBinds(std::string_view text);
	bool ContainsDoubleByteText(const char* text);
	void ConvertGameVariablesToBind(std::string& str);
	void ConvertFormatSpecifiersToBind(std::string& str);
	std::string PrepareSourceForRegistration(std::string text);
	std::string PrepareSourceForLookup(std::string text);
	std::string PrepareTarget(std::string text);

	// ---- entry management (dictionary_entry.cpp) ----
	bool EntryLess(const DictionaryEntry& left, const DictionaryEntry& right);
	bool AddEntry(const std::string& source, const std::string& target, int priority, const std::string& id);
	bool RegisterText(std::string source, std::string target, int priority, const std::string& id);
	void RegisterXmlNodes(pugi::xml_node parent, const char* nodeName, const char* sourceName, const char* targetName, const char* fieldName, int priority);
	void RegisterXmlEntry(const std::string& source, const std::string& target, RecordType type, int priority);
	void RegisterXmlNodesTyped(pugi::xml_node parent, const char* nodeName, const char* sourceName, const char* targetName, const char* champName, int priority, bool isEET);
	void SortIndexes();

	// ---- record type detection (dictionary_entry.cpp) ----
	RecordType DetectRecordTypeGrupChamp(const char* grup, const char* champ);
	RecordType DetectRecordTypeFromRec(const char* rec);

	// ---- translation (dictionary_translate.cpp) ----
	bool MatchWildcard(const DictionaryEntry& entry, const std::string& key, std::vector<std::string>& captures);
	bool ExpandTarget(const DictionaryEntry& entry, const std::vector<std::string>& captures, std::string& translated, int depth);
	bool TranslateInternal(const char* source, std::string& translated, int depth);
	void ResetFuzzyTextConfig();
	void LoadFuzzyTextConfig(pugi::xml_node root);
	void LoadUiHintConfig(pugi::xml_node root);

} // namespace fonthook
