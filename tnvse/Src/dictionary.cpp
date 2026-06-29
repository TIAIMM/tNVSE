#include "dictionary.h"
#include "dictionary_internal.h"

namespace fonthook
{

	// ---- global state definitions ----
	std::vector<DictionaryEntry> s_entries;
	std::unordered_map<std::string, std::vector<size_t>> s_exactIndex;
	std::vector<size_t> s_wildcardIndex;
	std::unordered_map<std::string, std::vector<size_t>> s_wildcardPrefixIndex;
	std::unordered_map<std::string, std::vector<size_t>> s_wildcardSuffixIndex;
	std::vector<size_t> s_wildcardLooseIndex;
	std::unordered_map<std::string, size_t> s_idIndex;
	std::unordered_map<std::string, std::string> s_positiveCache;
	std::unordered_set<std::string> s_negativeCache;
	std::unordered_map<std::string, UiHintFormat> s_uiHintFormats;
	std::unordered_set<std::string> s_registeredAutoKeys;
	bool s_dictionaryLoaded = false;

} // namespace fonthook
