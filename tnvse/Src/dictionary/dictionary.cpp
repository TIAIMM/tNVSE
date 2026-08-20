#include "dictionary.h"
#include "dictionary_internal.h"

namespace fonthook
{

	// ---- global state definitions ----
	std::vector<DictionaryEntry> s_entries;
	std::unordered_map<std::string, std::vector<size_t>> s_exactIndex;
	std::unordered_map<std::string, std::vector<size_t>> s_windows1252ExactIndex;
	std::vector<Windows1252WildcardAlias> s_windows1252WildcardIndex;
	std::vector<size_t> s_wildcardIndex;
	std::unordered_map<std::string, std::vector<size_t>> s_wildcardPrefixIndex;
	std::unordered_map<std::string, std::vector<size_t>> s_wildcardSuffixIndex;
	std::vector<size_t> s_wildcardLooseIndex;
	std::unordered_map<std::string, size_t> s_idIndex;
	PositiveTranslationCache s_positiveCache;
	NegativeTranslationCache s_negativeCache;
	std::deque<const std::string*> s_positiveCacheOrder;
	std::deque<const std::string*> s_negativeCacheOrder;
	std::unordered_map<std::string, UiHintFormat> s_uiHintFormats;
	std::unordered_set<std::string> s_registeredAutoKeys;
	bool s_dictionaryLoaded = false;

} // namespace fonthook
