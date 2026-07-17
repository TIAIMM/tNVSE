#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "font_manager.h"
#include "font_vector.h"
#include "native_calls.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fonthook
{
	// Recognize whether the previous encoded unit occupies two bytes.
	static bool TryGetDoubleByteAt(const char* buffer, UInt32 byteIndex, UInt32 bufferLen)
	{
		if (byteIndex + 1 >= bufferLen) return false;

		UInt32 dbCode = 0;
		return TryDecodeDoubleByte(&buffer[byteIndex], dbCode);
	}

	// ==================== Shared PrepText implementation ====================
	// PrepTextForTerminal and PrepText differ only in how iCharCount is set at the end:
	//   PrepTextForTerminal: iCharCount = origConsumed
	//   PrepText:            iCharCount = processedTextLen

	// Pass 1: process escape sequences (&variable;) in-place
	static void EnsureTextScratchSize(std::vector<char>& buffer, size_t size)
	{
		if (buffer.size() < size)
			buffer.resize(size, 0);
	}

	static bool ProcessEscapeSequences(
		std::vector<char>& processedOriginalBuffer,
		std::vector<char>& dynamicTextBuffer,
		UInt32& textBufferSize, UInt32& processedTextLen,
		UInt32& origConsumed, UInt32& sourceTextLen,
		FontEx* font, Font::TextData* axData)
	{
		char parsedTextBuffer[1028] = {};
		bool hasEscapeSequence = false;
		char* processedOriginalText = processedOriginalBuffer.data();

		for (UInt32 srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
		{
			if (processedOriginalText[srcTextIndex] != '&')
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
				continue;
			}

			int varNameLen = 0;
			int escapeSeqPrefixLen = 1;
			bool isPositiveEscape = true;
			if (processedOriginalText[srcTextIndex + 1] == '-')
			{
				isPositiveEscape = false;
				escapeSeqPrefixLen = 2;
			}
			char varNameBuffer[128];
			int rawLen = 0;
			while (processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex]
				&& rawLen < 127
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != ';'
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != '\n'
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != axData->cLineSep)
			{
				varNameBuffer[rawLen] = processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex];
				++rawLen;
			}
			int effectiveLen = rawLen;
			varNameBuffer[effectiveLen] = 0;
			UInt32 totalEscapeSeqLen = rawLen + escapeSeqPrefixLen; // prefix + variable name
			if (processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] == ';')
				totalEscapeSeqLen += 1;

			if (Interface_FindTextReplacementString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
				|| Interface_TestConstantForGameSettings(varNameBuffer, parsedTextBuffer))
			{
				UInt32 postEscapeTextLen = strlen(parsedTextBuffer);
				int escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;

				if (postEscapeTextLen > 0 && parsedTextBuffer[postEscapeTextLen - 1] == '\\')
				{
					int charScanIndex = 0;
					while (parsedTextBuffer[charScanIndex] != '\\') ++charScanIndex;

					char iconPath[264] = {};
					char textureNameBuffer[268];
					strcpy_s(iconPath, sizeof(iconPath), &parsedTextBuffer[charScanIndex + 1]);
					if (font->iFontNum == 7)
					{
						strcpy_s(textureNameBuffer, MAX_PATH, iconPath);
						sprintf_s(iconPath, MAX_PATH, "glow_%s", textureNameBuffer);
					}
					font->AddTextIcon(iconPath);
					parsedTextBuffer[charScanIndex] = 1;
					parsedTextBuffer[charScanIndex + 1] = 0;
					postEscapeTextLen = charScanIndex + 2;
					escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
				}
				if (escapeSeqSizeDiff > 0)
				{
					textBufferSize += escapeSeqSizeDiff;
					EnsureTextScratchSize(dynamicTextBuffer, textBufferSize + 1);
				}
				memcpy(&dynamicTextBuffer[processedTextLen], parsedTextBuffer, postEscapeTextLen);
				processedTextLen += postEscapeTextLen;
				origConsumed += totalEscapeSeqLen;
				srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
			}
			else
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
			}
			hasEscapeSequence = true;
		}

		dynamicTextBuffer[processedTextLen] = 0;
		if (hasEscapeSequence)
		{
			sourceTextLen = processedTextLen;
			processedOriginalBuffer.resize(processedTextLen + 4, 0);
			memcpy(processedOriginalBuffer.data(), dynamicTextBuffer.data(),
				processedTextLen + 1);
		}
		dynamicTextBuffer[0] = 0;
		processedTextLen = 0;
		return hasEscapeSequence;
	}

	struct FreeTypeClusterAdvanceMap
	{
		static constexpr UInt32 kNoOwner = UINT32_MAX;
		std::vector<float> advances;
		std::vector<UInt32> owners;
		std::vector<std::pair<UInt32, float>> clusters;

		void Reset(UInt32 length)
		{
			advances.assign(length, 0.0f);
			owners.assign(length, kNoOwner);
			clusters.clear();
		}

		void TrimRetainedCapacity()
		{
			constexpr size_t kMaximumRetainedUnits = 65536;
			if (advances.capacity() > kMaximumRetainedUnits)
				std::vector<float>().swap(advances);
			if (owners.capacity() > kMaximumRetainedUnits)
				std::vector<UInt32>().swap(owners);
			if (clusters.capacity() > kMaximumRetainedUnits)
				std::vector<std::pair<UInt32, float>>().swap(clusters);
		}
	};

	struct PrepTextScratch
	{
		std::vector<char> original;
		std::vector<char> processed;
		FreeTypeClusterAdvanceMap clusterAdvances;

		void Prepare(const char* source, size_t length)
		{
			const size_t bytes = length + 4;
			original.resize(bytes);
			memcpy(original.data(), source, length + 1);
			processed.resize(bytes);
			processed[0] = 0;
		}

		void TrimRetainedCapacity()
		{
			constexpr size_t kMaximumRetainedTextBytes = 64 * 1024;
			if (original.capacity() > kMaximumRetainedTextBytes)
				std::vector<char>().swap(original);
			if (processed.capacity() > kMaximumRetainedTextBytes)
				std::vector<char>().swap(processed);
			clusterAdvances.TrimRetainedCapacity();
		}
	};

	struct PrepTextScratchPool
	{
		std::array<PrepTextScratch, 4> slots;
		size_t depth = 0;
	};

	class PrepTextScratchLease
	{
	public:
		explicit PrepTextScratchLease(PrepTextScratchPool& pool) : m_pool(pool)
		{
			m_scratch = m_pool.depth < m_pool.slots.size()
				? &m_pool.slots[m_pool.depth] : &m_fallback;
			++m_pool.depth;
		}

		~PrepTextScratchLease()
		{
			m_scratch->TrimRetainedCapacity();
			--m_pool.depth;
		}

		PrepTextScratch& Get() { return *m_scratch; }

	private:
		PrepTextScratchPool& m_pool;
		PrepTextScratch m_fallback;
		PrepTextScratch* m_scratch = nullptr;
	};

	struct PreparedTextCacheLookupKey
	{
		const Font* font = nullptr;
		const FontData* fontData = nullptr;
		UInt64 layoutIdentity = 0;
		UInt64 metricSignature = 0;
		UInt64 iconSignature = 0;
		UInt32 codePage = 0;
		int width = 0;
		int height = 0;
		int lineStart = 0;
		int lineEnd = 0;
		char lineSeparator = 0;
		bool terminal = false;
		std::string_view source;
		std::string_view resolved;
	};

	struct PreparedTextCacheKey
	{
		const Font* font = nullptr;
		const FontData* fontData = nullptr;
		UInt64 layoutIdentity = 0;
		UInt64 metricSignature = 0;
		UInt64 iconSignature = 0;
		UInt32 codePage = 0;
		int width = 0;
		int height = 0;
		int lineStart = 0;
		int lineEnd = 0;
		char lineSeparator = 0;
		bool terminal = false;
		std::string source;
		std::string resolved;
	};

	template <class Key>
	static size_t HashPreparedTextCacheKey(const Key& key)
	{
		UInt64 hash = 1469598103934665603ull;
		auto addBytes = [&](const void* data, size_t size)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ull;
			}
		};
		const uintptr_t font = reinterpret_cast<uintptr_t>(key.font);
		const uintptr_t fontData = reinterpret_cast<uintptr_t>(key.fontData);
		addBytes(&font, sizeof(font));
		addBytes(&fontData, sizeof(fontData));
		addBytes(&key.layoutIdentity, sizeof(key.layoutIdentity));
		addBytes(&key.metricSignature, sizeof(key.metricSignature));
		addBytes(&key.iconSignature, sizeof(key.iconSignature));
		addBytes(&key.codePage, sizeof(key.codePage));
		addBytes(&key.width, sizeof(key.width));
		addBytes(&key.height, sizeof(key.height));
		addBytes(&key.lineStart, sizeof(key.lineStart));
		addBytes(&key.lineEnd, sizeof(key.lineEnd));
		addBytes(&key.lineSeparator, sizeof(key.lineSeparator));
		addBytes(&key.terminal, sizeof(key.terminal));
		addBytes(key.source.data(), key.source.size());
		const UInt8 separator = 0xFF;
		addBytes(&separator, sizeof(separator));
		addBytes(key.resolved.data(), key.resolved.size());
		return static_cast<size_t>(hash ^ (hash >> 32));
	}

	template <class Left, class Right>
	static bool EqualPreparedTextCacheKey(const Left& left, const Right& right)
	{
		return left.font == right.font && left.fontData == right.fontData
			&& left.layoutIdentity == right.layoutIdentity
			&& left.metricSignature == right.metricSignature
			&& left.iconSignature == right.iconSignature
			&& left.codePage == right.codePage && left.width == right.width
			&& left.height == right.height && left.lineStart == right.lineStart
			&& left.lineEnd == right.lineEnd
			&& left.lineSeparator == right.lineSeparator
			&& left.terminal == right.terminal && left.source == right.source
			&& left.resolved == right.resolved;
	}

	struct PreparedTextCacheKeyHash
	{
		using is_transparent = void;

		size_t operator()(const PreparedTextCacheKey& key) const
		{
			return HashPreparedTextCacheKey(key);
		}

		size_t operator()(const PreparedTextCacheLookupKey& key) const
		{
			return HashPreparedTextCacheKey(key);
		}
	};

	struct PreparedTextCacheKeyEqual
	{
		using is_transparent = void;

		bool operator()(const PreparedTextCacheKey& left,
			const PreparedTextCacheKey& right) const
		{
			return EqualPreparedTextCacheKey(left, right);
		}

		bool operator()(const PreparedTextCacheKey& left,
			const PreparedTextCacheLookupKey& right) const
		{
			return EqualPreparedTextCacheKey(left, right);
		}

		bool operator()(const PreparedTextCacheLookupKey& left,
			const PreparedTextCacheKey& right) const
		{
			return EqualPreparedTextCacheKey(left, right);
		}
	};

	struct PreparedTextCacheValue
	{
		std::string text;
		std::vector<int> lineWidths;
		int width = 0;
		int height = 0;
		int lineStart = 0;
		int lineEnd = 0;
		int charCount = 0;
	};

	struct PreparedTextCacheEntry
	{
		std::shared_ptr<const PreparedTextCacheValue> value;
		size_t bytes = 0;
		std::list<PreparedTextCacheKey>::iterator lru;
	};

	struct PreparedTextCacheState
	{
		std::mutex mutex;
		std::unordered_map<PreparedTextCacheKey, PreparedTextCacheEntry,
			PreparedTextCacheKeyHash, PreparedTextCacheKeyEqual> entries;
		std::list<PreparedTextCacheKey> lru;
		size_t bytes = 0;
	};

	static PreparedTextCacheState& GetPreparedTextCacheState()
	{
		static PreparedTextCacheState state;
		return state;
	}

	static size_t GetPreparedTextCacheLimit()
	{
		return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
			* 1024u * 1024u / 16u;
	}

	static UInt64 HashPreparedTextBytes(UInt64 hash, const void* data, size_t size)
	{
		const UInt8* bytes = static_cast<const UInt8*>(data);
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static UInt64 GetPreparedTextMetricSignature(const FontEx* font,
		float linePadding)
	{
		UInt64 hash = 1469598103934665603ull;
		hash = HashPreparedTextBytes(hash, &linePadding, sizeof(linePadding));
		if (!font || !font->pFontData)
			return hash;
		hash = HashPreparedTextBytes(hash, &font->pFontData->fBaseLine,
			sizeof(font->pFontData->fBaseLine));
		hash = HashPreparedTextBytes(hash, &font->fFontHeight,
			sizeof(font->fFontHeight));
		const FontLetter& space = font->pFontData->pFontLetters[kSpaceChar];
		hash = HashPreparedTextBytes(hash, &space, sizeof(space));
		return hash;
	}

	static UInt64 GetPreparedTextIconSignature(const FontEx* font)
	{
		UInt64 hash = 1469598103934665603ull;
		const UInt32 count = font ? font->ButtonIcons.uiSize : 0;
		hash = HashPreparedTextBytes(hash, &count, sizeof(count));
		if (font && font->ButtonIcons.pBuffer)
		{
			for (UInt32 index = 0; index < count; ++index)
			{
				const ButtonIcon& icon = font->ButtonIcons.pBuffer[index];
				hash = HashPreparedTextBytes(hash, &icon.fWidth, sizeof(icon.fWidth));
				hash = HashPreparedTextBytes(hash, &icon.fSpacing, sizeof(icon.fSpacing));
			}
		}
		return hash;
	}

	static void ApplyPreparedTextCacheValue(const PreparedTextCacheValue& value,
		Font::TextData& data)
	{
		data.xNewText.Set(value.text.c_str(), 0);
		data.iWidth = value.width;
		data.iHeight = value.height;
		data.iLineStart = value.lineStart;
		data.iLineEnd = value.lineEnd;
		data.iCharCount = value.charCount;
		data.xLineWidths.RemoveAll();
		for (int width : value.lineWidths)
		{
			int mutableWidth = width;
			data.xLineWidths.AddTail(mutableWidth);
		}
	}

	static std::shared_ptr<const PreparedTextCacheValue> FindPreparedTextCacheValue(
		const PreparedTextCacheLookupKey& lookup)
	{
		PreparedTextCacheState& state = GetPreparedTextCacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.entries.find(lookup);
		if (found == state.entries.end())
		{
			RecordFreeTypePreparedTextCacheResult(false);
			return nullptr;
		}
		state.lru.splice(state.lru.begin(), state.lru, found->second.lru);
		RecordFreeTypePreparedTextCacheResult(true);
		return found->second.value;
	}

	static std::shared_ptr<const PreparedTextCacheValue> CapturePreparedTextCacheValue(
		const Font::TextData& data)
	{
		auto value = std::make_shared<PreparedTextCacheValue>();
		value->text = data.xNewText.pString ? data.xNewText.pString : "";
		value->width = data.iWidth;
		value->height = data.iHeight;
		value->lineStart = data.iLineStart;
		value->lineEnd = data.iLineEnd;
		value->charCount = data.iCharCount;
		const BSSimpleList<int>* line = &data.xLineWidths;
		for (;;)
		{
			value->lineWidths.push_back(line->m_item);
			line = line->m_pkNext;
			if (!line)
				break;
		}
		return value;
	}

	static void StorePreparedTextCacheValue(const PreparedTextCacheLookupKey& lookup,
		const std::shared_ptr<const PreparedTextCacheValue>& value)
	{
		if (!value)
			return;
		PreparedTextCacheKey key = {
			lookup.font, lookup.fontData, lookup.layoutIdentity,
			lookup.metricSignature, lookup.iconSignature, lookup.codePage,
			lookup.width, lookup.height, lookup.lineStart, lookup.lineEnd,
			lookup.lineSeparator, lookup.terminal,
			std::string(lookup.source), std::string(lookup.resolved)
		};
		const size_t bytes = sizeof(PreparedTextCacheEntry)
			+ sizeof(PreparedTextCacheValue) + sizeof(PreparedTextCacheKey) * 2
			+ (key.source.size() + key.resolved.size()) * 2
			+ value->text.size() + value->lineWidths.size() * sizeof(int);
		const size_t limit = GetPreparedTextCacheLimit();
		if (!limit || bytes > limit)
			return;

		PreparedTextCacheState& state = GetPreparedTextCacheState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const auto existing = state.entries.find(key);
		if (existing != state.entries.end())
		{
			state.lru.splice(state.lru.begin(), state.lru, existing->second.lru);
			return;
		}
		state.lru.push_front(key);
		const auto [inserted, success] = state.entries.emplace(std::move(key),
			PreparedTextCacheEntry{ value, bytes, state.lru.begin() });
		if (!success)
		{
			state.lru.pop_front();
			return;
		}
		state.bytes += bytes;
		while (state.bytes > limit && !state.lru.empty())
		{
			const auto oldest = state.entries.find(state.lru.back());
			if (oldest != state.entries.end())
			{
				state.bytes -= oldest->second.bytes;
				state.entries.erase(oldest);
			}
			state.lru.pop_back();
		}
	}

	static void BuildFreeTypeClusterAdvanceMap(FontEx* font, const char* text,
		UInt32 length, FreeTypeClusterAdvanceMap& result)
	{
		if (!font || !text || !IsFreeTypeFontActive(font))
		{
			result.Reset(0);
			return;
		}
		result.Reset(length);
		for (UInt32 runStart = 0; runStart < length;)
		{
			const UInt8 current = static_cast<UInt8>(text[runStart]);
			if (!current)
				break;
			if (current < 0x20 || current == kDelChar || current == '~')
			{
				++runStart;
				continue;
			}
			UInt32 runEnd = runStart;
			while (runEnd < length && text[runEnd])
			{
				const UInt8 value = static_cast<UInt8>(text[runEnd]);
				if (value < 0x20 || value == kDelChar || value == '~')
					break;
				UInt32 dbcsCode = 0;
				runEnd += TryDecodeDoubleByte(text + runEnd, dbcsCode) ? 2 : 1;
			}
			FreeTypeLayoutRun layout;
			if (!LayoutFreeTypeRun(font, text + runStart, runEnd - runStart, layout, true))
			{
				runStart = runEnd > runStart ? runEnd : runStart + 1;
				continue;
			}
			result.clusters.clear();
			for (const FreeTypeLayoutGlyph& glyph : *layout.glyphs)
			{
				if (result.clusters.empty()
					|| result.clusters.back().first != glyph.cluster)
				{
					result.clusters.emplace_back(glyph.cluster, 0.0f);
				}
				result.clusters.back().second += glyph.xAdvance;
			}
			for (size_t clusterIndex = 0;
				clusterIndex < result.clusters.size(); ++clusterIndex)
			{
				const UInt32 clusterStart = runStart
					+ result.clusters[clusterIndex].first;
				const UInt32 clusterEnd = std::min<UInt32>(length,
					clusterIndex + 1 < result.clusters.size()
						? runStart + result.clusters[clusterIndex + 1].first : runEnd);
				if (clusterStart >= length || clusterStart >= clusterEnd)
					continue;
				result.advances[clusterStart] = result.clusters[clusterIndex].second;
				for (UInt32 unitOffset = clusterStart; unitOffset < clusterEnd;)
				{
					result.owners[unitOffset] = clusterStart;
					UInt32 dbcsCode = 0;
					unitOffset += TryDecodeDoubleByte(text + unitOffset, dbcsCode) ? 2 : 1;
				}
			}
			runStart = runEnd > runStart ? runEnd : runStart + 1;
		}
	}

	static void PrepTextImpl(FontEx* font, const char* apOrigString, Font::TextData* axData, bool isTerminal)
	{
		if (!apOrigString)
			return;

		if (axData->iWidth <= 0)
			axData->iWidth = kSentinelMax;
		if (axData->iHeight <= 0)
			axData->iHeight = kSentinelMax;
		if (axData->iLineEnd <= 0)
			axData->iLineEnd = kSentinelMax;
		const UInt32 originalTextLen = static_cast<UInt32>(strlen(apOrigString));
		const float lineSpacingAdjust = FontManager::GetLinePadding(font->iFontNum);
		UInt64 layoutIdentity = 0;
		const bool cacheable = GetFreeTypeLayoutIdentity(font, layoutIdentity);
		PreparedTextCacheLookupKey cacheLookup;
		if (cacheable)
		{
			cacheLookup = {
				font, font->pFontData, layoutIdentity,
				GetPreparedTextMetricSignature(font, lineSpacingAdjust),
				GetPreparedTextIconSignature(font), GetFreeTypeTextCodePage(),
				axData->iWidth, axData->iHeight, axData->iLineStart,
				axData->iLineEnd, axData->cLineSep, isTerminal,
				std::string_view(apOrigString, originalTextLen), {}
			};
			if (!std::memchr(apOrigString, '&', originalTextLen))
			{
				if (const std::shared_ptr<const PreparedTextCacheValue> cached =
					FindPreparedTextCacheValue(cacheLookup))
				{
					ApplyPreparedTextCacheValue(*cached, *axData);
					return;
				}
			}
		}

		auto* extraGlyphs = GetExtraGlyphs(font->iFontNum);
		UInt32 origConsumed = 0;

		// Cache frequently accessed font data to avoid repeated pointer chasing
		auto* pFontLetters = font->pFontData->pFontLetters;
		float fBaseLine = font->pFontData->fBaseLine;
		float lineHeight = fBaseLine + lineSpacingAdjust;
		LayoutWrapState wrapState;
		UInt32 softWrapPosition = 0;
		int maxLineWidth = 0;
		float totalTextHeight = pFontLetters[kSpaceChar].fHeight;
		int currentLineCount = 1;
		UInt32 sourceTextLen = originalTextLen;
		int maxAllowedLines = axData->iLineEnd;

		thread_local PrepTextScratchPool scratchPool;
		PrepTextScratchLease scratchLease(scratchPool);
		PrepTextScratch& scratch = scratchLease.Get();
		scratch.Prepare(apOrigString, sourceTextLen);
		char* processedOriginalText = scratch.original.data();
		char* dynamicTextBuffer = scratch.processed.data();

		UInt32 processedTextLen = 0;
		UInt32 textBufferSize = sourceTextLen + 4;

		// ---- Pass 1: Process escape sequences (&variable;) ----
		const bool processedEscapes = ProcessEscapeSequences(scratch.original, scratch.processed,
			textBufferSize, processedTextLen, origConsumed, sourceTextLen, font, axData);
		processedOriginalText = scratch.original.data();
		dynamicTextBuffer = scratch.processed.data();
		if (cacheable && processedEscapes)
		{
			cacheLookup.iconSignature = GetPreparedTextIconSignature(font);
			cacheLookup.resolved = std::string_view(processedOriginalText, sourceTextLen);
			if (const std::shared_ptr<const PreparedTextCacheValue> cached =
				FindPreparedTextCacheValue(cacheLookup))
			{
				ApplyPreparedTextCacheValue(*cached, *axData);
				return;
			}
		}

		UInt32 buttonIconIndex = 0;
		FreeTypeClusterAdvanceMap& freeTypeAdvances = scratch.clusterAdvances;
		BuildFreeTypeClusterAdvanceMap(font, processedOriginalText,
			sourceTextLen, freeTypeAdvances);
		UInt32 previousClusterOutputStart = 0;
		bool hasPreviousClusterOutput = false;

		// ---- Pass 2: Text layout with wrapping ----
		bool bIsDBCharacter;
		UInt32 uiDoubleByteCode;
		for (UInt32 charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
		{
			if (processedOriginalText[charIndex] == axData->cLineSep)
			{
				dynamicTextBuffer[processedTextLen] = axData->cLineSep;
				origConsumed += 1;
				if (++processedTextLen >= textBufferSize)
				{
					textBufferSize = processedTextLen + 4;
					EnsureTextScratchSize(scratch.processed, textBufferSize);
					dynamicTextBuffer = scratch.processed.data();
				}
				totalTextHeight = lineHeight + totalTextHeight;
				int completedLineWidth = static_cast<int>(std::ceil(
					std::max(0.0, wrapState.currentLineWidth)));
				axData->xLineWidths.AddTail(completedLineWidth);
				maxLineWidth = MaxInt(maxLineWidth, completedLineWidth);
				wrapState.ResetLine();
				softWrapPosition = 0;
				hasPreviousClusterOutput = false;
				++currentLineCount;
			}
			else
			{
				bIsDBCharacter = false;
				if (extraGlyphs && (charIndex + 1) <= sourceTextLen)
				{
					bIsDBCharacter = TryDecodeDoubleByte((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
				}

				if (processedOriginalText[charIndex] == '\t')
				{
					wrapState.AdvanceTab(kTabWidth);
					origConsumed += 1;
					continue;
				}

				UInt8 currentChar;
				FontLetter* pCurrentGlyph = nullptr;
				double unitWidth = 0.0;
				bool isSoftMarker = false;
				bool isClusterContinuation = false;
				const UInt32 layoutClusterOwner = charIndex < freeTypeAdvances.owners.size()
					? freeTypeAdvances.owners[charIndex]
					: FreeTypeClusterAdvanceMap::kNoOwner;

				if (!bIsDBCharacter)
				{
					currentChar = processedOriginalText[charIndex];
					origConsumed += 1;
					ConvertToAsciiQuotes(&currentChar);
					pCurrentGlyph = &pFontLetters[currentChar];
					if (currentChar == 1)
					{
						if (buttonIconIndex < font->ButtonIcons.uiSize)
						{
							pCurrentGlyph->fWidth = font->ButtonIcons.pBuffer[buttonIconIndex].fWidth;
							pCurrentGlyph->fSpacing = font->ButtonIcons.pBuffer[buttonIconIndex].fSpacing;
						}
						++buttonIconIndex;
					}
					if (currentChar == '~')
					{
						wrapState.MarkSoftWrap();
						if (wrapState.hasSoftWrap)
							softWrapPosition = processedTextLen;
						isSoftMarker = true;
					}
					else
						unitWidth = GetGlyphRenderAdvance(pCurrentGlyph);
				}
				else
				{
					origConsumed += 2;
					if (layoutClusterOwner == FreeTypeClusterAdvanceMap::kNoOwner)
					{
						EnsureFreeTypeDoubleByteMetrics(font, uiDoubleByteCode);
						FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
						if (glyph)
						{
							pCurrentGlyph = glyph;
							unitWidth = GetGlyphRenderAdvance(pCurrentGlyph);
						}
					}
				}
				if (!isSoftMarker
					&& layoutClusterOwner != FreeTypeClusterAdvanceMap::kNoOwner)
				{
					isClusterContinuation = layoutClusterOwner != charIndex;
					if (!isClusterContinuation)
						unitWidth = freeTypeAdvances.advances[charIndex];
				}

				LayoutWrapResult wrapResult;
				if (!isSoftMarker && !isClusterContinuation)
					wrapResult = wrapState.AddUnit(unitWidth, static_cast<float>(axData->iWidth));

				if (wrapResult.kind != LayoutWrapKind::None)
				{
					if (processedTextLen + 4 >= textBufferSize)
					{
						textBufferSize = processedTextLen + 8;
						EnsureTextScratchSize(scratch.processed, textBufferSize);
						dynamicTextBuffer = scratch.processed.data();
					}

					if (wrapResult.kind == LayoutWrapKind::Soft)
					{
						const UInt32 insertPos = softWrapPosition;
						memmove(&dynamicTextBuffer[insertPos + 1], &dynamicTextBuffer[insertPos],
							(processedTextLen - insertPos) + 1);
						dynamicTextBuffer[insertPos] = axData->cLineSep;
						++processedTextLen;
					}
					else
					{
						UInt32 tailStart = hasPreviousClusterOutput
							? previousClusterOutputStart : processedTextLen - 1;
						UInt32 tailBytes = processedTextLen - tailStart;
						if (!hasPreviousClusterOutput && processedTextLen >= 2)
						{
							UInt32 dbStart = processedTextLen - 2;
							if (extraGlyphs && TryGetDoubleByteAt(dynamicTextBuffer, dbStart, processedTextLen))
							{
								tailStart = dbStart;
								tailBytes = processedTextLen - tailStart;
							}
						}

						memmove(&dynamicTextBuffer[tailStart + 1], &dynamicTextBuffer[tailStart], tailBytes);
						dynamicTextBuffer[tailStart] = axData->cLineSep;
						++processedTextLen;
					}

					totalTextHeight += lineHeight;
					int completedLineWidth = static_cast<int>(std::ceil(std::max(0.0, wrapResult.completedWidth)));
					axData->xLineWidths.AddTail(completedLineWidth);
					maxLineWidth = MaxInt(maxLineWidth, completedLineWidth);
					softWrapPosition = 0;
					++currentLineCount;
				}
				const UInt32 currentClusterOutputStart = processedTextLen;

				if (bIsDBCharacter)
				{
					if (processedTextLen + 4 >= textBufferSize)
					{
						textBufferSize = processedTextLen + 8;
						EnsureTextScratchSize(scratch.processed, textBufferSize);
						dynamicTextBuffer = scratch.processed.data();
					}
					dynamicTextBuffer[processedTextLen++] = processedOriginalText[charIndex];
					dynamicTextBuffer[processedTextLen++] = processedOriginalText[charIndex + 1];
					dynamicTextBuffer[processedTextLen] = 0;
					++charIndex;
				}
				else
				{
					if (currentChar != '~')
					{
						if (processedTextLen + 1 >= textBufferSize)
						{
							textBufferSize = processedTextLen + 8;
							EnsureTextScratchSize(scratch.processed, textBufferSize);
							dynamicTextBuffer = scratch.processed.data();
						}
						dynamicTextBuffer[processedTextLen++] = (char)currentChar;
						dynamicTextBuffer[processedTextLen] = 0;
					}
				}
				if (!isSoftMarker && !isClusterContinuation)
				{
					previousClusterOutputStart = currentClusterOutputStart;
					hasPreviousClusterOutput = true;
				}

				if (processedTextLen >= textBufferSize)
				{
					textBufferSize = processedTextLen + 4;
					EnsureTextScratchSize(scratch.processed, textBufferSize);
					dynamicTextBuffer = scratch.processed.data();
				}
			}

			if (maxAllowedLines > 0 && currentLineCount > maxAllowedLines && processedTextLen)
			{
				while (processedTextLen > 0 && dynamicTextBuffer[processedTextLen - 1] != axData->cLineSep)
				{
					--processedTextLen;
					--origConsumed;
				}
				dynamicTextBuffer[processedTextLen] = 0;
				currentLineCount = maxAllowedLines;
				wrapState.ResetLine();
				totalTextHeight = totalTextHeight - lineHeight;
				break;
			}
		}

		if (*dynamicTextBuffer && axData->iLineStart)
		{
			UInt32 truncatedTextLen = 0;
			SInt32 lineCounter = 0;
			for (UInt32 truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
			{
				if (lineCounter >= axData->iLineStart && lineCounter < axData->iLineEnd)
					dynamicTextBuffer[truncatedTextLen++] = dynamicTextBuffer[truncateCharCounter];
				if (dynamicTextBuffer[truncateCharCounter] == axData->cLineSep)
					++lineCounter;
			}
			dynamicTextBuffer[truncatedTextLen] = 0;
			processedTextLen = truncatedTextLen;
			origConsumed = truncatedTextLen;
		}

		if (!*dynamicTextBuffer)
		{
			strcpy_s(dynamicTextBuffer, textBufferSize, " ");
			origConsumed = 1;
			processedTextLen = 1;
			currentLineCount = 1;
			totalTextHeight = pFontLetters[kSpaceChar].fHeight;
			wrapState.currentLineWidth = pFontLetters[kSpaceChar].fWidth;
		}
		int completedLineWidth = static_cast<int>(std::ceil(std::max(0.0, wrapState.currentLineWidth)));
		axData->xLineWidths.AddTail(completedLineWidth);
		maxLineWidth = MaxInt(maxLineWidth, completedLineWidth);
		dynamicTextBuffer[processedTextLen] = 0;
		axData->xNewText.Set(dynamicTextBuffer, 0);
		axData->iWidth = maxLineWidth;
		axData->iHeight = totalTextHeight;
		axData->iLineStart = 0;
		axData->iLineEnd = currentLineCount;
		axData->iCharCount = isTerminal ? origConsumed : processedTextLen;
		if (cacheable)
			StorePreparedTextCacheValue(cacheLookup, CapturePreparedTextCacheValue(*axData));
	}

	// ==================== FontEx::PrepTextForTerminal ====================
	void __thiscall FontEx::PrepTextForTerminal(const char* apOrigString, Font::TextData* axData)
	{
		PrepTextImpl(this, apOrigString, axData, true);
	}

	// ==================== FontEx::PrepText ====================
	void __thiscall FontEx::PrepText(const char* apOrigString, Font::TextData* axData)
	{
		PrepTextImpl(this, apOrigString, axData, false);
	}

} // namespace fonthook
