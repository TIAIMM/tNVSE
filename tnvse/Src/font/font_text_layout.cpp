#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "font_manager.h"
#include "font_vector.h"
#include "font_vector_internal.h"
#include "load_config.h"
#include "native_calls.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <limits>
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
		UInt64& resolvedTextHash, FontEx* font, Font::TextData* axData)
	{
		char parsedTextBuffer[1028] = {};
		bool hasEscapeSequence = false;
		char* processedOriginalText = processedOriginalBuffer.data();
		resolvedTextHash = 1469598103934665603ull;
		auto addResolvedByte = [&](UInt8 value)
		{
			resolvedTextHash ^= value;
			resolvedTextHash *= 1099511628211ull;
		};

		for (UInt32 srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
		{
			if (processedOriginalText[srcTextIndex] != '&')
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
				addResolvedByte(static_cast<UInt8>(
					processedOriginalText[srcTextIndex]));
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
				for (UInt32 index = 0; index < postEscapeTextLen; ++index)
					addResolvedByte(static_cast<UInt8>(parsedTextBuffer[index]));
				processedTextLen += postEscapeTextLen;
				origConsumed += totalEscapeSeqLen;
				srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
			}
			else
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
				addResolvedByte(static_cast<UInt8>(
					processedOriginalText[srcTextIndex]));
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

	struct PrepTextScratch
	{
		std::vector<char> original;
		std::vector<char> processed;

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
		UInt64 directProfilePublicationEpoch = 0;
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
		UInt64 sourceHash = 0;
		UInt64 resolvedHash = 0;
		size_t precomputedHash = 0;
	};

	struct PreparedTextCacheKey
	{
		const Font* font = nullptr;
		const FontData* fontData = nullptr;
		UInt64 layoutIdentity = 0;
		UInt64 directProfilePublicationEpoch = 0;
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
		UInt64 sourceHash = 0;
		UInt64 resolvedHash = 0;
		size_t precomputedHash = 0;
	};

	template <class Key>
	static size_t HashPreparedTextCacheKey(const Key& key)
	{
		if (key.precomputedHash)
			return key.precomputedHash;
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
		addBytes(&key.directProfilePublicationEpoch,
			sizeof(key.directProfilePublicationEpoch));
		addBytes(&key.metricSignature, sizeof(key.metricSignature));
		addBytes(&key.iconSignature, sizeof(key.iconSignature));
		addBytes(&key.codePage, sizeof(key.codePage));
		addBytes(&key.width, sizeof(key.width));
		addBytes(&key.height, sizeof(key.height));
		addBytes(&key.lineStart, sizeof(key.lineStart));
		addBytes(&key.lineEnd, sizeof(key.lineEnd));
		addBytes(&key.lineSeparator, sizeof(key.lineSeparator));
		addBytes(&key.terminal, sizeof(key.terminal));
		addBytes(&key.sourceHash, sizeof(key.sourceHash));
		const size_t sourceSize = key.source.size();
		addBytes(&sourceSize, sizeof(sourceSize));
		const UInt8 separator = 0xFF;
		addBytes(&separator, sizeof(separator));
		addBytes(&key.resolvedHash, sizeof(key.resolvedHash));
		const size_t resolvedSize = key.resolved.size();
		addBytes(&resolvedSize, sizeof(resolvedSize));
		return static_cast<size_t>(hash ^ (hash >> 32));
	}

	template <class Left, class Right>
	static bool EqualPreparedTextCacheKey(const Left& left, const Right& right)
	{
		return left.font == right.font && left.fontData == right.fontData
			&& left.layoutIdentity == right.layoutIdentity
			&& left.directProfilePublicationEpoch
				== right.directProfilePublicationEpoch
			&& left.metricSignature == right.metricSignature
			&& left.iconSignature == right.iconSignature
			&& left.codePage == right.codePage && left.width == right.width
			&& left.height == right.height && left.lineStart == right.lineStart
			&& left.lineEnd == right.lineEnd
			&& left.lineSeparator == right.lineSeparator
			&& left.terminal == right.terminal
			&& left.sourceHash == right.sourceHash
			&& left.resolvedHash == right.resolvedHash
			&& left.source == right.source
			&& left.resolved == right.resolved;
	}

	struct PreparedTextCacheKeyHash
	{
		using is_transparent = void;

		size_t operator()(
			const std::shared_ptr<const PreparedTextCacheKey>& key) const
		{
			return key ? HashPreparedTextCacheKey(*key) : 0;
		}

		size_t operator()(const PreparedTextCacheLookupKey& key) const
		{
			return HashPreparedTextCacheKey(key);
		}

		size_t operator()(const PreparedTextCacheKey& key) const
		{
			return HashPreparedTextCacheKey(key);
		}
	};

	struct PreparedTextCacheKeyEqual
	{
		using is_transparent = void;

		bool operator()(
			const std::shared_ptr<const PreparedTextCacheKey>& left,
			const std::shared_ptr<const PreparedTextCacheKey>& right) const
		{
			return left && right
				&& EqualPreparedTextCacheKey(*left, *right);
		}

		bool operator()(
			const std::shared_ptr<const PreparedTextCacheKey>& left,
			const PreparedTextCacheLookupKey& right) const
		{
			return left
				&& EqualPreparedTextCacheKey(*left, right);
		}

		bool operator()(const PreparedTextCacheLookupKey& left,
			const std::shared_ptr<const PreparedTextCacheKey>& right) const
		{
			return right
				&& EqualPreparedTextCacheKey(left, *right);
		}

		bool operator()(
			const std::shared_ptr<const PreparedTextCacheKey>& left,
			const PreparedTextCacheKey& right) const
		{
			return left
				&& EqualPreparedTextCacheKey(*left, right);
		}

		bool operator()(const PreparedTextCacheKey& left,
			const std::shared_ptr<const PreparedTextCacheKey>& right) const
		{
			return right
				&& EqualPreparedTextCacheKey(left, *right);
		}
	};

	using PreparedTextCacheKeyPtr =
		std::shared_ptr<const PreparedTextCacheKey>;

	struct PreparedTextCacheValue
	{
		vectorfont::CpuMemoryLease cpuMemory;
		std::string text;
		std::vector<int> lineWidths;
		std::shared_ptr<const PreparedDirectTextSidecar> directSidecar;
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
		std::list<PreparedTextCacheKeyPtr>::iterator lru;
		vectorfont::CpuMemoryLease cpuMemory;
	};

	struct PreparedTextCacheState
	{
		std::mutex mutex;
		std::atomic<UInt64> generation{ 1 };
		std::unordered_map<PreparedTextCacheKeyPtr, PreparedTextCacheEntry,
			PreparedTextCacheKeyHash, PreparedTextCacheKeyEqual> entries;
		std::list<PreparedTextCacheKeyPtr> lru;
		size_t bytes = 0;
	};

	static PreparedTextCacheState& GetPreparedTextCacheState()
	{
		static PreparedTextCacheState state;
		return state;
	}

	struct PreparedTextTlsEntry
	{
		PreparedTextCacheKeyPtr key;
		std::shared_ptr<const PreparedTextCacheValue> value;
	};

	constexpr size_t kPreparedTextTlsFrontSize = 8;
	constexpr UInt8 kPreparedTextAdmissionRetryCooldown = 8;
	static_assert((kPreparedTextTlsFrontSize
		& (kPreparedTextTlsFrontSize - 1)) == 0,
		"Prepared-text TLS front size must be a power of two");
	thread_local std::array<PreparedTextTlsEntry,
		kPreparedTextTlsFrontSize>
		s_preparedTextTlsFront;
	thread_local UInt64 s_preparedTextTlsGeneration = 0;
	thread_local std::array<size_t, 256>
		s_preparedTextAdmissionHashes = {};
	thread_local std::array<UInt8, 256>
		s_preparedTextAdmissionCounts = {};
	thread_local std::array<UInt8, 256>
		s_preparedTextAdmissionCooldowns = {};
	struct PreparedTextMetricSignatureEntry
	{
		const FontEx* font = nullptr;
		const FontData* fontData = nullptr;
		float linePadding = 0.0f;
		float baseline = 0.0f;
		float fontHeight = 0.0f;
		FontLetter space = {};
		UInt64 signature = 0;
	};
	thread_local std::array<PreparedTextMetricSignatureEntry, 4>
		s_preparedTextMetricSignatures;
	thread_local size_t s_preparedTextMetricSignatureNext = 0;

	struct PreparedTextSidecarHandoff
	{
		const Font::TextData* data = nullptr;
		const Font* font = nullptr;
		const char* preparedText = nullptr;
		std::shared_ptr<const PreparedDirectTextSidecar> sidecar;
	};

	thread_local std::array<PreparedTextSidecarHandoff, 4>
		s_preparedTextSidecarHandoffs;
	thread_local size_t s_preparedTextSidecarHandoffNext = 0;

	struct PreparedTextScan
	{
		UInt32 length = 0;
		UInt64 hash = 1469598103934665603ull;
		bool hasEscape = false;
	};

	static PreparedTextScan ScanPreparedText(const char* text)
	{
		PreparedTextScan result;
		if (!text)
			return result;
		while (result.length != std::numeric_limits<UInt32>::max()
			&& text[result.length])
		{
			const UInt8 value = static_cast<UInt8>(text[result.length]);
			result.hasEscape = result.hasEscape || value == '&';
			result.hash ^= value;
			result.hash *= 1099511628211ull;
			++result.length;
		}
		return result;
	}

	static void PublishPreparedTextSidecar(const Font::TextData* data,
		const Font* font,
		std::shared_ptr<const PreparedDirectTextSidecar> sidecar)
	{
		if (!data || !font || !sidecar)
			return;
		PreparedTextSidecarHandoff& handoff =
			s_preparedTextSidecarHandoffs[
				s_preparedTextSidecarHandoffNext++
					% s_preparedTextSidecarHandoffs.size()];
		handoff.data = data;
		handoff.font = font;
		handoff.preparedText = data->xNewText.c_str();
		handoff.sidecar = std::move(sidecar);
	}

	std::shared_ptr<const PreparedDirectTextSidecar>
		ConsumeFreeTypePreparedTextSidecar(
			const Font::TextData* data, const Font* font,
			const char* preparedText)
	{
		if (!data || !font || !preparedText)
			return {};
		UInt64 layoutIdentity = 0;
		if (!GetFreeTypeLayoutIdentity(font, layoutIdentity))
			return {};
		for (PreparedTextSidecarHandoff& handoff :
			s_preparedTextSidecarHandoffs)
		{
			if (handoff.data != data || handoff.font != font
				|| handoff.preparedText != preparedText
				|| !handoff.sidecar)
			{
				continue;
			}
			std::shared_ptr<const PreparedDirectTextSidecar>
				sidecar = std::move(handoff.sidecar);
			handoff = {};
			if (sidecar->layoutIdentity != layoutIdentity)
			{
				return {};
			}
			return sidecar;
		}
		return {};
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

	static void RefreshPreparedTextLookupHash(
		PreparedTextCacheLookupKey& lookup)
	{
		lookup.precomputedHash = 0;
		lookup.precomputedHash =
			HashPreparedTextCacheKey(lookup);
		if (!lookup.precomputedHash)
			lookup.precomputedHash = 1;
	}

	enum class PreparedTextAdmissionAction : UInt8
	{
		ColdBypass,
		RejectedBypass,
		Promote,
		Probe
	};

	// The first observation stays lock-free. The second observation is allowed
	// to build and publish a resident value without first paying a global
	// lookup. Only a later TLS miss for an already observed key needs a global
	// probe.
	static PreparedTextAdmissionAction TouchPreparedTextAdmission(size_t hash)
	{
		if (!hash)
			return PreparedTextAdmissionAction::ColdBypass;
		const size_t index =
			hash % s_preparedTextAdmissionHashes.size();
		size_t& slot = s_preparedTextAdmissionHashes[index];
		UInt8& count = s_preparedTextAdmissionCounts[index];
		UInt8& cooldown = s_preparedTextAdmissionCooldowns[index];
		if (slot != hash)
		{
			slot = hash;
			count = 1;
			cooldown = 0;
			return PreparedTextAdmissionAction::ColdBypass;
		}
		if (cooldown)
		{
			--cooldown;
			return PreparedTextAdmissionAction::RejectedBypass;
		}
		if (count < 3)
			++count;
		if (count <= 1)
			return PreparedTextAdmissionAction::ColdBypass;
		if (count == 2)
			return PreparedTextAdmissionAction::Promote;
		return PreparedTextAdmissionAction::Probe;
	}

	static void RejectPreparedTextAdmission(size_t hash)
	{
		if (!hash)
			return;
		const size_t index =
			hash % s_preparedTextAdmissionHashes.size();
		if (s_preparedTextAdmissionHashes[index] != hash)
			return;
		s_preparedTextAdmissionCounts[index] = 0;
		s_preparedTextAdmissionCooldowns[index] =
			kPreparedTextAdmissionRetryCooldown;
		vectorfont::RecordFreeTypePerf(
			vectorfont::FreeTypePerfCounter::
				PreparedTextAdmissionRejected);
	}

	static void ResetPreparedTextTlsFront(UInt64 generation)
	{
		if (s_preparedTextTlsGeneration == generation)
			return;
		for (PreparedTextTlsEntry& entry : s_preparedTextTlsFront)
			entry = {};
		s_preparedTextTlsGeneration = generation;
	}

	static void PublishPreparedTextTlsEntry(
		const PreparedTextCacheKeyPtr& key,
		const std::shared_ptr<const PreparedTextCacheValue>& value,
		UInt64 generation)
	{
		if (!key || !value)
			return;
		ResetPreparedTextTlsFront(generation);
		PreparedTextTlsEntry& front =
			s_preparedTextTlsFront[key->precomputedHash
				& (s_preparedTextTlsFront.size() - 1)];
		front.key = key;
		front.value = value;
	}

	static UInt64 GetPreparedTextMetricSignature(const FontEx* font,
		float linePadding)
	{
		if (font && font->pFontData)
		{
			const FontLetter& space =
				font->pFontData->pFontLetters[kSpaceChar];
			for (const PreparedTextMetricSignatureEntry& entry :
				s_preparedTextMetricSignatures)
			{
				if (entry.font == font
					&& entry.fontData == font->pFontData
					&& std::memcmp(&entry.linePadding, &linePadding,
						sizeof(linePadding)) == 0
					&& std::memcmp(&entry.baseline,
						&font->pFontData->fBaseLine,
						sizeof(entry.baseline)) == 0
					&& std::memcmp(&entry.fontHeight,
						&font->fFontHeight,
						sizeof(entry.fontHeight)) == 0
					&& std::memcmp(&entry.space, &space,
						sizeof(space)) == 0)
				{
					return entry.signature;
				}
			}
		}
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
		PreparedTextMetricSignatureEntry& cached =
			s_preparedTextMetricSignatures[
				s_preparedTextMetricSignatureNext++
					% s_preparedTextMetricSignatures.size()];
		cached.font = font;
		cached.fontData = font->pFontData;
		cached.linePadding = linePadding;
		cached.baseline = font->pFontData->fBaseLine;
		cached.fontHeight = font->fFontHeight;
		cached.space = space;
		cached.signature = hash;
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
		Font::TextData& data, const Font* font)
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
		PublishPreparedTextSidecar(
			&data, font, value.directSidecar);
		if (value.directSidecar && value.directSidecar->rejectBatch)
		{
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::PreparedTextRejectCacheHit);
		}
	}

	static std::shared_ptr<const PreparedTextCacheValue> FindPreparedTextCacheValue(
		const PreparedTextCacheLookupKey& lookup, bool& storeCandidate)
	{
		storeCandidate = false;
		size_t lookupHash = lookup.precomputedHash
			? lookup.precomputedHash
			: HashPreparedTextCacheKey(lookup);
		if (!lookupHash)
			lookupHash = 1;
		PreparedTextCacheState& state = GetPreparedTextCacheState();
		const UInt64 generation =
			state.generation.load(std::memory_order_acquire);
		ResetPreparedTextTlsFront(generation);
		PreparedTextTlsEntry& front =
			s_preparedTextTlsFront[lookupHash
				& (s_preparedTextTlsFront.size() - 1)];
		if (front.value
			&& front.key
			&& front.key->precomputedHash == lookupHash
			&& EqualPreparedTextCacheKey(*front.key, lookup))
		{
			RecordFreeTypePreparedTextCacheResult(true);
			return front.value;
		}
		const PreparedTextAdmissionAction admission =
			TouchPreparedTextAdmission(lookupHash);
		if (admission == PreparedTextAdmissionAction::ColdBypass
			|| admission == PreparedTextAdmissionAction::RejectedBypass)
		{
			if (admission == PreparedTextAdmissionAction::RejectedBypass)
			{
				vectorfont::RecordFreeTypePerf(
					vectorfont::FreeTypePerfCounter::
						PreparedTextRejectionBypass);
			}
			RecordFreeTypePreparedTextCacheResult(false);
			return nullptr;
		}
		storeCandidate = true;
		if (admission == PreparedTextAdmissionAction::Promote)
		{
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::
					PreparedTextPromotionBypass);
			RecordFreeTypePreparedTextCacheResult(false);
			return nullptr;
		}
		vectorfont::RecordFreeTypePerf(
			vectorfont::FreeTypePerfCounter::PreparedTextGlobalProbe);
		std::unique_lock<std::mutex> lock(state.mutex);
		const auto found = state.entries.find(lookup);
		if (found == state.entries.end())
		{
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::
					PreparedTextGlobalProbeMiss);
			RecordFreeTypePreparedTextCacheResult(false);
			return nullptr;
		}
		state.lru.splice(state.lru.begin(), state.lru, found->second.lru);
		const PreparedTextCacheKeyPtr key = found->first;
		const std::shared_ptr<const PreparedTextCacheValue> value =
			found->second.value;
		const UInt64 residentGeneration =
			state.generation.load(std::memory_order_relaxed);
		lock.unlock();
		PublishPreparedTextTlsEntry(key, value, residentGeneration);
		RecordFreeTypePreparedTextCacheResult(true);
		return value;
	}

	static std::shared_ptr<const PreparedTextCacheValue> CapturePreparedTextCacheValue(
		const Font::TextData& data,
		std::shared_ptr<const PreparedDirectTextSidecar> directSidecar = {})
	{
		auto value = std::make_shared<PreparedTextCacheValue>();
		value->text = data.xNewText.pString ? data.xNewText.pString : "";
		value->width = data.iWidth;
		value->height = data.iHeight;
		value->lineStart = data.iLineStart;
		value->lineEnd = data.iLineEnd;
		value->charCount = data.iCharCount;
		value->directSidecar = std::move(directSidecar);
		const BSSimpleList<int>* line = &data.xLineWidths;
		for (;;)
		{
			value->lineWidths.push_back(line->m_item);
			line = line->m_pkNext;
			if (!line)
				break;
		}
		value->cpuMemory.Reset(vectorfont::CpuMemoryCategory::PreparedText,
			sizeof(PreparedTextCacheValue) + value->text.capacity()
				+ value->lineWidths.capacity() * sizeof(int)
				+ (value->directSidecar
					? sizeof(PreparedDirectTextSidecar)
						+ value->directSidecar->units.capacity()
							* sizeof(DirectTextUnit)
					: 0));
		return value;
	}

	static bool StorePreparedTextCacheValue(const PreparedTextCacheLookupKey& lookup,
		const std::shared_ptr<const PreparedTextCacheValue>& value)
	{
		if (!value)
			return false;
		const size_t limit = vectorfont::GetCpuMemoryCategoryHeadroom(
			vectorfont::CpuMemoryCategory::PreparedText,
			GetPreparedTextCacheLimit());
		if (!limit)
			return false;

		PreparedTextCacheState& state = GetPreparedTextCacheState();
		std::unique_lock<std::mutex> lock(state.mutex);
		const auto existing = state.entries.find(lookup);
		if (existing != state.entries.end())
		{
			state.lru.splice(state.lru.begin(), state.lru, existing->second.lru);
			const PreparedTextCacheKeyPtr key = existing->first;
			const std::shared_ptr<const PreparedTextCacheValue> residentValue =
				existing->second.value;
			const UInt64 generation =
				state.generation.load(std::memory_order_relaxed);
			lock.unlock();
			PublishPreparedTextTlsEntry(key, residentValue, generation);
			return true;
		}
		size_t lookupHash = lookup.precomputedHash
			? lookup.precomputedHash
			: HashPreparedTextCacheKey(lookup);
		if (!lookupHash)
			lookupHash = 1;

		PreparedTextCacheKey key = {
			lookup.font, lookup.fontData, lookup.layoutIdentity,
			lookup.directProfilePublicationEpoch,
			lookup.metricSignature, lookup.iconSignature, lookup.codePage,
			lookup.width, lookup.height, lookup.lineStart, lookup.lineEnd,
			lookup.lineSeparator, lookup.terminal,
			std::string(lookup.source), std::string(lookup.resolved),
			lookup.sourceHash, lookup.resolvedHash,
			lookupHash
		};
		auto keyOwner =
			std::make_shared<const PreparedTextCacheKey>(
				std::move(key));
		const size_t keyBytes = sizeof(PreparedTextCacheKey)
			+ keyOwner->source.capacity()
			+ keyOwner->resolved.capacity();
		const size_t bytes = sizeof(PreparedTextCacheEntry)
			+ sizeof(PreparedTextCacheValue) + keyBytes
			+ value->text.capacity()
			+ value->lineWidths.capacity() * sizeof(int)
			+ (value->directSidecar
				? sizeof(PreparedDirectTextSidecar)
					+ value->directSidecar->units.capacity()
						* sizeof(DirectTextUnit)
				: 0);
		const size_t cacheOverhead = sizeof(PreparedTextCacheEntry)
			+ keyBytes + 2u * sizeof(PreparedTextCacheKeyPtr)
			+ 4u * sizeof(void*);
		if (bytes > limit)
			return false;
		state.lru.push_front(keyOwner);
		const auto [inserted, success] = state.entries.emplace(keyOwner,
			PreparedTextCacheEntry{ value, bytes, state.lru.begin() });
		if (!success)
		{
			state.lru.pop_front();
			return false;
		}
		inserted->second.cpuMemory.Reset(
			vectorfont::CpuMemoryCategory::PreparedText, cacheOverhead);
		state.bytes += bytes;
		vectorfont::RecordFreeTypePerf(
			vectorfont::FreeTypePerfCounter::PreparedTextAdmission);
		bool retained = true;
		while ((vectorfont::GetCpuMemoryUsage(
			vectorfont::CpuMemoryCategory::PreparedText) > limit
			|| vectorfont::IsCpuMemoryBudgetExceeded())
			&& !state.lru.empty())
		{
			const auto oldest = state.entries.find(state.lru.back());
			if (oldest != state.entries.end())
			{
				if (oldest->first == keyOwner)
					retained = false;
				state.bytes -= oldest->second.bytes;
				state.entries.erase(oldest);
				state.generation.fetch_add(1,
					std::memory_order_acq_rel);
				vectorfont::RecordFreeTypePerf(
					vectorfont::FreeTypePerfCounter::
						PreparedTextEviction);
			}
			state.lru.pop_back();
		}
		const UInt64 generation =
			state.generation.load(std::memory_order_relaxed);
		lock.unlock();
		if (retained)
			PublishPreparedTextTlsEntry(keyOwner, value, generation);
		return retained;
	}

	namespace vectorfont
	{
		void TrimPreparedTextCpuCacheForTotalBudget()
		{
			PreparedTextCacheState& state = GetPreparedTextCacheState();
			std::lock_guard<std::mutex> lock(state.mutex);
			const size_t limit = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::PreparedText, GetPreparedTextCacheLimit());
			while ((GetCpuMemoryUsage(CpuMemoryCategory::PreparedText) > limit
					|| IsCpuMemoryBudgetExceeded())
				&& !state.lru.empty())
			{
				const auto oldest = state.entries.find(state.lru.back());
				if (oldest != state.entries.end())
				{
					state.bytes -= oldest->second.bytes;
					state.entries.erase(oldest);
					state.generation.fetch_add(1,
						std::memory_order_acq_rel);
					RecordFreeTypePerf(
						FreeTypePerfCounter::PreparedTextEviction);
				}
				state.lru.pop_back();
			}
		}
	}

	enum class FreeTypeBreakKind : UInt8
	{
		None,
		Whitespace,
		SoftHyphen
	};

	struct FreeTypeBreakOpportunity
	{
		FreeTypeBreakKind kind = FreeTypeBreakKind::None;
		UInt32 outputPosition = 0;
		size_t directUnitPosition = std::numeric_limits<size_t>::max();
		UInt32 sourceConsumedEnd = 0;
		double prefixWidth = 0.0;
		double consumedWidth = 0.0;

		void Clear()
		{
			kind = FreeTypeBreakKind::None;
			outputPosition = 0;
			directUnitPosition = std::numeric_limits<size_t>::max();
			sourceConsumedEnd = 0;
			prefixWidth = 0.0;
			consumedWidth = 0.0;
		}
	};

	struct PreparedLineRange
	{
		UInt32 begin = 0;
		UInt32 end = 0;
		int width = 0;
		UInt32 consumed = 0;
	};

	static void InsertPreparedBytes(std::vector<char>& output,
		UInt32& outputLength, UInt32 position, const char* bytes, UInt32 count)
	{
		if (!bytes || !count || position > outputLength)
			return;
		EnsureTextScratchSize(output,
			static_cast<size_t>(outputLength) + count + 1);
		memmove(output.data() + position + count, output.data() + position,
			static_cast<size_t>(outputLength - position) + 1);
		memcpy(output.data() + position, bytes, count);
		outputLength += count;
	}

	static double GetFreeTypeHyphenAdvance(FontEx* font)
	{
		return font && font->pFontData
			? GetGlyphRenderAdvance(&font->pFontData->pFontLetters['-']) : 0.0;
	}

	static double GetNextTabAdvance(double lineWidth)
	{
		double remainder = std::fmod(lineWidth, static_cast<double>(kTabWidth));
		if (remainder < 0.0)
			remainder += static_cast<double>(kTabWidth);
		return static_cast<double>(kTabWidth) - remainder;
	}

	static bool IsAsciiWordGlyph(const VectorEncodedGlyph& glyph)
	{
		if (glyph.byteLength != 1 || glyph.encodedCode > 0x7F)
			return false;
		const UInt8 value = static_cast<UInt8>(glyph.encodedCode);
		return (value >= '0' && value <= '9')
			|| (value >= 'A' && value <= 'Z')
			|| (value >= 'a' && value <= 'z');
	}

	static std::shared_ptr<const PreparedDirectTextSidecar>
		BuildPreparedDirectTextSidecar(
			FontEx* font, const Font::TextData& data,
			size_t textLength, std::vector<DirectTextUnit>&& units)
	{
		vectorfont::FreeTypePerfScope perf(
			vectorfont::FreeTypePerfPhase::Sidecar);
		const char* text = data.xNewText.c_str();
		if (!font || !text)
			return {};
		UInt64 layoutIdentity = 0;
		if (!GetFreeTypeLayoutIdentity(font, layoutIdentity))
			return {};
		UInt32 previousEnd = 0;
		for (const DirectTextUnit& unit : units)
		{
			const UInt32 end = unit.byteOffset + unit.byteLength;
			if (!unit.byteLength || unit.byteOffset != previousEnd
				|| end > textLength)
			{
				return {};
			}
			const UInt8 first = static_cast<UInt8>(
				text[unit.byteOffset]);
			if (unit.kind == DirectTextUnitKind::Glyph
				&& (unit.directSlot
						== std::numeric_limits<UInt16>::max()
					|| unit.byteClass >= 2))
			{
				return {};
			}
			if (unit.kind == DirectTextUnitKind::Glyph)
			{
				const UInt16 encoded = unit.byteLength == 2
					? static_cast<UInt16>(
						(static_cast<UInt16>(first) << 8)
						| static_cast<UInt8>(
							text[unit.byteOffset + 1]))
					: first;
				if (encoded != unit.encodedCode)
					return {};
			}
			else if ((unit.kind == DirectTextUnitKind::LineBreak
						&& first
							!= static_cast<UInt8>(data.cLineSep))
				|| (unit.kind == DirectTextUnitKind::Tab
					&& first != '\t')
				|| (unit.kind == DirectTextUnitKind::Icon
					&& first != 1))
			{
				return {};
			}
			previousEnd = end;
		}
		if (previousEnd != textLength)
			return {};
		auto sidecar =
			std::make_shared<PreparedDirectTextSidecar>();
		sidecar->layoutIdentity = layoutIdentity;
		sidecar->textLength = textLength;
		sidecar->units = std::move(units);
		return sidecar;
	}

	static std::shared_ptr<const PreparedDirectTextSidecar>
		BuildRejectedPreparedDirectTextSidecar(
			FontEx* font, const Font::TextData& data)
	{
		if (!font)
			return {};
		UInt64 layoutIdentity = 0;
		if (!GetFreeTypeLayoutIdentity(font, layoutIdentity))
			return {};
		const char* text = data.xNewText.c_str();
		if (!text)
			return {};
		const size_t textLength = strlen(text);
		auto sidecar =
			std::make_shared<PreparedDirectTextSidecar>();
		sidecar->layoutIdentity = layoutIdentity;
		sidecar->textLength = textLength;
		sidecar->rejectBatch = true;
		return sidecar;
	}

	// FreeType no longer performs shaping or cluster substitution. Match the
	// native PrepText contract directly at encoded-unit granularity so wrapping,
	// measurement, line selection, tabs, and final geometry all consume the same
	// FontLetter advances. A DBCS pair is always emitted and moved as one unit.
	static std::shared_ptr<const PreparedDirectTextSidecar>
		PrepDirectFreeTypeText(
		FontEx* font,
		char* source,
		UInt32 sourceLength,
		Font::TextData* data,
		bool isTerminal,
		UInt32 initialConsumed,
		PrepTextScratch& scratch)
	{
		vectorfont::FreeTypePerfScope perf(
			vectorfont::FreeTypePerfPhase::Layout);
		if (!font || !font->pFontData || !source || !data)
			return {};

		data->xLineWidths.RemoveAll();
		std::vector<char>& output = scratch.processed;
		EnsureTextScratchSize(output, static_cast<size_t>(sourceLength) + 8);
		output[0] = 0;

		FontLetter* letters = font->pFontData->pFontLetters;
		const float lineHeight = font->pFontData->fBaseLine
			+ FontManager::GetLinePadding(font->iFontNum);
		const float firstLineHeight = letters[kSpaceChar].fHeight;
		const double hyphenAdvance = GetFreeTypeHyphenAdvance(font);
		const double maxWidth = static_cast<double>(data->iWidth);
		const bool boundedWidth = data->iWidth < kSentinelMax;
		const int requestedLineStart = std::max(0, data->iLineStart);
		const int requestedLineEnd = data->iLineEnd;
		vectorfont::RuntimeFont* directRuntime =
			vectorfont::FindActiveRuntime(font);
		std::shared_ptr<const
			vectorfont::SealedDirectFontProfile> directProfile =
				directRuntime
					? vectorfont::AcquireSealedDirectLayoutProfile(
						*directRuntime)
					: nullptr;
		bool directRecordInvalid = false;
		std::vector<DirectTextUnit> directUnits;
		if (directProfile)
			directUnits.reserve(sourceLength);
		VectorEncodedGlyph directHyphen;
		bool directHyphenResolved = false;
		bool directHyphenAttempted = false;

		UInt32 outputLength = 0;
		UInt32 consumed = initialConsumed;
		UInt32 iconIndex = 0;
		double lineWidth = 0.0;
		bool lastUnitWasAsciiWord = false;
		FreeTypeBreakOpportunity breakOpportunity;
		std::vector<int> lineWidths;
		std::vector<UInt32> lineConsumed;
		lineWidths.reserve(8);
		lineConsumed.reserve(8);

		auto appendByte = [&](char value)
		{
			EnsureTextScratchSize(output,
				static_cast<size_t>(outputLength) + 2);
			output[outputLength++] = value;
			output[outputLength] = 0;
		};
		auto appendBytes = [&](const char* bytes, UInt32 count)
		{
			if (!bytes || !count)
				return;
			EnsureTextScratchSize(output,
				static_cast<size_t>(outputLength) + count + 1);
			memcpy(output.data() + outputLength, bytes, count);
			outputLength += count;
			output[outputLength] = 0;
		};
		auto appendDirectUnit = [&](DirectTextUnitKind kind,
			UInt32 byteOffset, UInt32 byteLength, UInt16 encodedCode,
			double advance, const VectorEncodedGlyph* glyph)
		{
			if (!directProfile || !byteLength
				|| byteLength > std::numeric_limits<UInt8>::max())
			{
				return;
			}
			DirectTextUnit unit;
			unit.byteOffset = byteOffset;
			unit.byteLength = static_cast<UInt8>(byteLength);
			unit.encodedCode = encodedCode;
			unit.kind = kind;
			unit.advance = static_cast<float>(advance);
			if (glyph)
			{
				unit.directSlot = glyph->directSlot;
				unit.byteClass =
					static_cast<UInt8>(glyph->byteClass);
			}
			directUnits.push_back(unit);
		};
		auto resolveDirectHyphen = [&]() -> const VectorEncodedGlyph*
		{
			if (!directProfile)
				return nullptr;
			if (!directHyphenAttempted)
			{
				directHyphenAttempted = true;
				const vectorfont::SealedDirectGlyphLookup lookup =
					vectorfont::DecodeSealedDirectGlyph(
						*directProfile, "-", directHyphen);
				directHyphenResolved = lookup
					== vectorfont::SealedDirectGlyphLookup::Resolved
					&& directHyphen.hasDirectMetrics
					&& directHyphen.byteLength == 1
					&& directHyphen.directSlot
						!= std::numeric_limits<UInt16>::max();
				if (!directHyphenResolved)
				{
					if (lookup
						== vectorfont::SealedDirectGlyphLookup::Invalid)
					{
						vectorfont::InvalidateSealedDirectFontProfile(
							*directRuntime);
						directRecordInvalid = true;
					}
					directProfile.reset();
					directUnits.clear();
				}
			}
			return directHyphenResolved ? &directHyphen : nullptr;
		};
		auto finishLine = [&](double width, UInt32 sourceConsumedEnd)
		{
			lineWidths.push_back(static_cast<int>(std::ceil(std::max(0.0, width))));
			lineConsumed.push_back(sourceConsumedEnd);
		};

		auto emitUnit = [&](const char* bytes, UInt32 byteCount, double unitWidth,
			UInt32 unitSourceEnd, bool breakableWhitespace,
			bool removableSpace, bool asciiWord,
			DirectTextUnitKind unitKind,
			const VectorEncodedGlyph* directGlyph)
		{
			bool wrappedBeforeCurrent = false;
			while (boundedWidth && lineWidth > 0.0
				&& lineWidth + unitWidth > maxWidth)
			{
				if (breakOpportunity.kind == FreeTypeBreakKind::Whitespace
					&& breakOpportunity.outputPosition < outputLength)
				{
					output[breakOpportunity.outputPosition] = data->cLineSep;
					if (directProfile
						&& breakOpportunity.directUnitPosition
							< directUnits.size())
					{
						DirectTextUnit& unit =
							directUnits[
								breakOpportunity.directUnitPosition];
						unit.advance = 0.0f;
						unit.directSlot =
							std::numeric_limits<UInt16>::max();
						unit.encodedCode = static_cast<UInt8>(
							data->cLineSep);
						unit.byteLength = 1;
						unit.byteClass = 0;
						unit.kind = DirectTextUnitKind::LineBreak;
					}
					finishLine(breakOpportunity.prefixWidth,
						breakOpportunity.sourceConsumedEnd);
					lineWidth = std::max(0.0,
						lineWidth - breakOpportunity.consumedWidth);
					breakOpportunity.Clear();
					lastUnitWasAsciiWord = false;
					wrappedBeforeCurrent = true;
					continue;
				}
				if (breakOpportunity.kind == FreeTypeBreakKind::SoftHyphen)
				{
					const char inserted[2] = { '-', data->cLineSep };
					const VectorEncodedGlyph* hyphen =
						resolveDirectHyphen();
					if (directProfile
						&& breakOpportunity.directUnitPosition
							<= directUnits.size()
						&& hyphen)
					{
						for (size_t unitIndex =
								breakOpportunity.directUnitPosition;
							unitIndex < directUnits.size();
							++unitIndex)
						{
							directUnits[unitIndex].byteOffset += 2;
						}
						DirectTextUnit hyphenUnit;
						hyphenUnit.byteOffset =
							breakOpportunity.outputPosition;
						hyphenUnit.advance =
							static_cast<float>(hyphenAdvance);
						hyphenUnit.directSlot =
							hyphen->directSlot;
						hyphenUnit.encodedCode = '-';
						hyphenUnit.byteLength = 1;
						hyphenUnit.byteClass =
							static_cast<UInt8>(
								hyphen->byteClass);
						hyphenUnit.kind =
							DirectTextUnitKind::Glyph;
						DirectTextUnit lineBreakUnit;
						lineBreakUnit.byteOffset =
							breakOpportunity.outputPosition + 1;
						lineBreakUnit.encodedCode =
							static_cast<UInt8>(
								data->cLineSep);
						lineBreakUnit.byteLength = 1;
						lineBreakUnit.kind =
							DirectTextUnitKind::LineBreak;
						const auto insertion =
							directUnits.begin()
								+ breakOpportunity
									.directUnitPosition;
						directUnits.insert(insertion,
							{ hyphenUnit, lineBreakUnit });
					}
					InsertPreparedBytes(output, outputLength,
						breakOpportunity.outputPosition, inserted,
						static_cast<UInt32>(sizeof(inserted)));
					finishLine(breakOpportunity.prefixWidth + hyphenAdvance,
						breakOpportunity.sourceConsumedEnd);
					lineWidth = std::max(0.0,
						lineWidth - breakOpportunity.consumedWidth);
					breakOpportunity.Clear();
					lastUnitWasAsciiWord = false;
					wrappedBeforeCurrent = true;
					continue;
				}

				double completedWidth = lineWidth;
				if (lastUnitWasAsciiWord && asciiWord && hyphenAdvance > 0.0
					&& completedWidth + hyphenAdvance <= maxWidth)
				{
					const UInt32 hyphenOffset = outputLength;
					appendByte('-');
					if (const VectorEncodedGlyph* hyphen =
							resolveDirectHyphen())
					{
						appendDirectUnit(
							DirectTextUnitKind::Glyph,
							hyphenOffset, 1, '-',
							hyphenAdvance, hyphen);
					}
					completedWidth += hyphenAdvance;
				}
				const UInt32 lineBreakOffset = outputLength;
				appendByte(data->cLineSep);
				appendDirectUnit(DirectTextUnitKind::LineBreak,
					lineBreakOffset, 1,
					static_cast<UInt8>(data->cLineSep),
					0.0, nullptr);
				finishLine(completedWidth, consumed);
				lineWidth = 0.0;
				breakOpportunity.Clear();
				lastUnitWasAsciiWord = false;
				wrappedBeforeCurrent = true;
			}

			// A space that itself forced a wrap is the discarded delimiter, not an
			// indentation byte on the new line. Explicit leading spaces remain intact.
			if (removableSpace && wrappedBeforeCurrent && lineWidth == 0.0)
				return;

			const double prefixWidth = lineWidth;
			const UInt32 unitOutputStart = outputLength;
			appendBytes(bytes, byteCount);
			UInt16 encodedCode = byteCount == 2
				? static_cast<UInt16>(
					(static_cast<UInt16>(
						static_cast<UInt8>(bytes[0])) << 8)
					| static_cast<UInt8>(bytes[1]))
				: static_cast<UInt8>(bytes[0]);
			appendDirectUnit(unitKind, unitOutputStart,
				byteCount, encodedCode, unitWidth, directGlyph);
			lineWidth += unitWidth;
			if (breakableWhitespace && prefixWidth > 0.0)
			{
				breakOpportunity.kind = FreeTypeBreakKind::Whitespace;
				breakOpportunity.outputPosition = unitOutputStart;
				breakOpportunity.directUnitPosition =
					directProfile && !directUnits.empty()
						? directUnits.size() - 1
						: std::numeric_limits<size_t>::max();
				breakOpportunity.sourceConsumedEnd = unitSourceEnd;
				breakOpportunity.prefixWidth = prefixWidth;
				breakOpportunity.consumedWidth = lineWidth;
			}
			lastUnitWasAsciiWord = asciiWord;
		};

		for (UInt32 sourceOffset = 0;
			sourceOffset < sourceLength && source[sourceOffset];)
		{
			UInt8 current = static_cast<UInt8>(source[sourceOffset]);
			if (current == static_cast<UInt8>(data->cLineSep))
			{
				++consumed;
				const UInt32 lineBreakOffset = outputLength;
				appendByte(data->cLineSep);
				appendDirectUnit(DirectTextUnitKind::LineBreak,
					lineBreakOffset, 1,
					static_cast<UInt8>(data->cLineSep),
					0.0, nullptr);
				finishLine(lineWidth, consumed);
				lineWidth = 0.0;
				breakOpportunity.Clear();
				lastUnitWasAsciiWord = false;
				++sourceOffset;
				continue;
			}
			if (current == '~')
			{
				++consumed;
				if (lineWidth > 0.0)
				{
					breakOpportunity.kind = FreeTypeBreakKind::SoftHyphen;
					breakOpportunity.outputPosition = outputLength;
					breakOpportunity.directUnitPosition =
						directProfile
							? directUnits.size()
							: std::numeric_limits<size_t>::max();
					breakOpportunity.sourceConsumedEnd = consumed;
					breakOpportunity.prefixWidth = lineWidth;
					breakOpportunity.consumedWidth = lineWidth;
				}
				++sourceOffset;
				continue;
			}
			if (current == '	')
			{
				++consumed;
				const double tabAdvance = GetNextTabAdvance(lineWidth);
				emitUnit(source + sourceOffset, 1, tabAdvance, consumed,
					true, false, false,
					DirectTextUnitKind::Tab, nullptr);
				++sourceOffset;
				continue;
			}
			if (current == 1)
			{
				++consumed;
				FontLetter iconMetrics = letters[1];
				if (font->ButtonIcons.pBuffer && iconIndex < font->ButtonIcons.uiSize)
				{
					iconMetrics.fWidth = font->ButtonIcons.pBuffer[iconIndex].fWidth;
					iconMetrics.fSpacing = font->ButtonIcons.pBuffer[iconIndex].fSpacing;
				}
				++iconIndex;
				const char iconByte = 1;
				emitUnit(&iconByte, 1, GetGlyphRenderAdvance(&iconMetrics),
					consumed, false, false, false,
					DirectTextUnitKind::Icon, nullptr);
				++sourceOffset;
				continue;
			}
			if (current == kDelChar)
			{
				++consumed;
				++sourceOffset;
				continue;
			}
			if (current < 0x20)
			{
				++consumed;
				const UInt32 controlOffset = outputLength;
				appendByte(static_cast<char>(current));
				appendDirectUnit(DirectTextUnitKind::Control,
					controlOffset, 1, current, 0.0, nullptr);
				++sourceOffset;
				continue;
			}

			UInt32 dbcsCode = 0;
			const bool isDbcs = sourceOffset + 1 < sourceLength
				&& source[sourceOffset + 1]
				&& TryDecodeDoubleByte(source + sourceOffset, dbcsCode);
			if (!isDbcs)
			{
				ConvertToAsciiQuotes(&current);
				source[sourceOffset] = static_cast<char>(current);
			}

			VectorEncodedGlyph glyph;
			bool decoded = false;
			if (directProfile)
			{
				const vectorfont::SealedDirectGlyphLookup lookup =
					vectorfont::DecodeSealedDirectGlyph(
						*directProfile,
						source + sourceOffset, glyph);
				decoded = lookup
					== vectorfont::SealedDirectGlyphLookup::Resolved;
				if (!decoded)
				{
					directProfile.reset();
					directUnits.clear();
					if (lookup
						== vectorfont::SealedDirectGlyphLookup::
							Unavailable)
					{
						decoded = DecodeFreeTypeGlyph(font,
							source + sourceOffset, glyph);
					}
					else
					{
						vectorfont::InvalidateSealedDirectFontProfile(
							*directRuntime);
						directRecordInvalid = true;
					}
				}
			}
			else
			{
				decoded = DecodeFreeTypeGlyph(
					font, source + sourceOffset, glyph);
			}
			if (!decoded
				|| !glyph.byteLength)
			{
				const UInt32 fallbackLength = isDbcs ? 2u : 1u;
				consumed += fallbackLength;
				emitUnit(source + sourceOffset, fallbackLength, 0.0,
					consumed, false, false, false,
					DirectTextUnitKind::Control, nullptr);
				sourceOffset += fallbackLength;
				continue;
			}

			const double unitWidth = GetVectorGlyphRenderAdvance(glyph);
			const bool isSpace = glyph.byteLength == 1
				&& glyph.encodedCode == kSpaceChar;
			emitUnit(source + sourceOffset, glyph.byteLength, unitWidth,
				consumed + glyph.byteLength, isSpace, isSpace,
				IsAsciiWordGlyph(glyph),
				DirectTextUnitKind::Glyph, &glyph);
			consumed += glyph.byteLength;
			sourceOffset += glyph.byteLength;
		}

		finishLine(lineWidth, consumed);
		output[outputLength] = 0;

		std::vector<PreparedLineRange> ranges;
		ranges.reserve(lineWidths.size());
		UInt32 lineBegin = 0;
		size_t lineOrdinal = 0;
		for (UInt32 index = 0; index < outputLength; ++index)
		{
			if (output[index] != data->cLineSep)
				continue;
			PreparedLineRange range;
			range.begin = lineBegin;
			range.end = index;
			range.width = lineOrdinal < lineWidths.size()
				? lineWidths[lineOrdinal] : 0;
			range.consumed = lineOrdinal < lineConsumed.size()
				? lineConsumed[lineOrdinal] : consumed;
			ranges.push_back(range);
			lineBegin = index + 1;
			++lineOrdinal;
		}
		PreparedLineRange finalRange;
		finalRange.begin = lineBegin;
		finalRange.end = outputLength;
		finalRange.width = lineOrdinal < lineWidths.size()
			? lineWidths[lineOrdinal] : 0;
		finalRange.consumed = lineOrdinal < lineConsumed.size()
			? lineConsumed[lineOrdinal] : consumed;
		ranges.push_back(finalRange);

		const int totalLines = static_cast<int>(ranges.size());
		int selectedStart = std::clamp(requestedLineStart, 0, totalLines);
		int selectedEnd = requestedLineEnd >= kSentinelMax
			? totalLines : std::clamp(requestedLineEnd, selectedStart, totalLines);
		auto setDirectSpaceUnit = [&]()
		{
			directUnits.clear();
			if (!directProfile)
				return;
			VectorEncodedGlyph space;
			const vectorfont::SealedDirectGlyphLookup lookup =
				vectorfont::DecodeSealedDirectGlyph(
					*directProfile, " ", space);
			if (lookup
					== vectorfont::SealedDirectGlyphLookup::Resolved
				&& space.hasDirectMetrics
				&& space.byteLength == 1
				&& space.directSlot
					!= std::numeric_limits<UInt16>::max())
			{
				appendDirectUnit(DirectTextUnitKind::Glyph,
					0, 1, kSpaceChar,
					GetVectorGlyphRenderAdvance(space), &space);
				return;
			}
			if (lookup
				== vectorfont::SealedDirectGlyphLookup::Invalid)
			{
				vectorfont::InvalidateSealedDirectFontProfile(
					*directRuntime);
				directRecordInvalid = true;
			}
			directProfile.reset();
			directUnits.clear();
		};

		if (selectedStart >= selectedEnd)
		{
			outputLength = 1;
			output[0] = ' ';
			output[1] = 0;
			const int width = static_cast<int>(std::ceil(std::max(0.0f,
				GetGlyphRenderAdvance(&letters[kSpaceChar]))));
			int mutableWidth = width;
			data->xLineWidths.AddTail(mutableWidth);
			data->xNewText.Set(output.data(), 0);
			data->iWidth = width;
			data->iHeight = static_cast<int>(std::max(0.0f, firstLineHeight));
			data->iLineStart = 0;
			data->iLineEnd = 1;
			data->iCharCount = 1;
			setDirectSpaceUnit();
			return directRecordInvalid
				? BuildRejectedPreparedDirectTextSidecar(
					font, *data)
				: directProfile
					? BuildPreparedDirectTextSidecar(
						font, *data, outputLength,
						std::move(directUnits))
					: std::shared_ptr<const
						PreparedDirectTextSidecar>();
		}

		UInt32 selectedLength = 0;
		int maximumLineWidth = 0;
		std::vector<DirectTextUnit> selectedDirectUnits;
		size_t directUnitCursor = 0;
		if (directProfile)
			selectedDirectUnits.reserve(directUnits.size());
		for (int line = selectedStart; line < selectedEnd; ++line)
		{
			const PreparedLineRange& range = ranges[line];
			const UInt32 count = range.end - range.begin;
			const UInt32 selectedLineOffset = selectedLength;
			if (directProfile)
			{
				while (directUnitCursor < directUnits.size()
					&& directUnits[directUnitCursor].byteOffset
							+ directUnits[directUnitCursor].byteLength
						<= range.begin)
				{
					++directUnitCursor;
				}
				size_t scan = directUnitCursor;
				while (scan < directUnits.size()
					&& directUnits[scan].byteOffset < range.end)
				{
					const DirectTextUnit& sourceUnit =
						directUnits[scan];
					if (sourceUnit.byteOffset >= range.begin
						&& sourceUnit.byteOffset
								+ sourceUnit.byteLength
							<= range.end)
					{
						DirectTextUnit selectedUnit = sourceUnit;
						selectedUnit.byteOffset =
							selectedLineOffset
							+ sourceUnit.byteOffset
							- range.begin;
						selectedDirectUnits.push_back(
							selectedUnit);
					}
					++scan;
				}
				directUnitCursor = scan;
			}
			if (count)
			{
				memmove(output.data() + selectedLength,
					output.data() + range.begin, count);
				selectedLength += count;
			}
			int mutableWidth = range.width;
			data->xLineWidths.AddTail(mutableWidth);
			maximumLineWidth = MaxInt(maximumLineWidth, range.width);
			if (line + 1 < selectedEnd)
			{
				if (directProfile)
				{
					DirectTextUnit lineBreak;
					lineBreak.byteOffset = selectedLength;
					lineBreak.encodedCode =
						static_cast<UInt8>(data->cLineSep);
					lineBreak.byteLength = 1;
					lineBreak.kind =
						DirectTextUnitKind::LineBreak;
					selectedDirectUnits.push_back(lineBreak);
				}
				output[selectedLength++] = data->cLineSep;
			}
		}
		if (directProfile)
			directUnits = std::move(selectedDirectUnits);

		bool substitutedEmptyLine = false;
		int selectedLineCount = selectedEnd - selectedStart;

		// Match the original PrepText contract: an empty selected result is
		// represented by one space, so BSStringT retains a valid buffer and
		// downstream CreateText code receives one measurable placeholder glyph.
		if (!selectedLength)
		{
			data->xLineWidths.RemoveAll();

			EnsureTextScratchSize(output, 2);
			output[0] = ' ';
			output[1] = 0;
			selectedLength = 1;

			const int spaceWidth = static_cast<int>(std::ceil(std::max(
				0.0f,
				GetGlyphRenderAdvance(&letters[kSpaceChar]))));

			int mutableWidth = spaceWidth;
			data->xLineWidths.AddTail(mutableWidth);

			maximumLineWidth = spaceWidth;
			selectedLineCount = 1;
			substitutedEmptyLine = true;
			setDirectSpaceUnit();
		}

		outputLength = selectedLength;
		output[outputLength] = 0;

		data->xNewText.Set(output.data(), 0);
		data->iWidth = maximumLineWidth;
		data->iHeight = static_cast<int>(std::max(
			0.0f,
			firstLineHeight
			+ std::max(0, selectedLineCount - 1) * lineHeight));
		data->iLineStart = 0;
		data->iLineEnd = selectedLineCount;
		data->iCharCount = substitutedEmptyLine
			? 1
			: (isTerminal && selectedStart == 0
				? static_cast<int>(ranges[selectedEnd - 1].consumed)
				: static_cast<int>(outputLength));
		return directRecordInvalid
			? BuildRejectedPreparedDirectTextSidecar(font, *data)
			: directProfile
				? BuildPreparedDirectTextSidecar(
					font, *data, outputLength,
					std::move(directUnits))
				: std::shared_ptr<const PreparedDirectTextSidecar>();
	}

	static void PrepTextImpl(FontEx* font, const char* apOrigString,
		Font::TextData* axData, bool isTerminal, bool resolveEscapes = true)
	{
		if (!apOrigString)
			return;

		if (axData->iWidth <= 0)
			axData->iWidth = kSentinelMax;
		if (axData->iHeight <= 0)
			axData->iHeight = kSentinelMax;
		if (axData->iLineEnd <= 0)
			axData->iLineEnd = kSentinelMax;
		const PreparedTextScan originalText = ScanPreparedText(apOrigString);
		const UInt32 originalTextLen = originalText.length;
		const float lineSpacingAdjust = FontManager::GetLinePadding(font->iFontNum);
		UInt64 layoutIdentity = 0;
		const bool cacheable = GetFreeTypeLayoutIdentity(font, layoutIdentity)
			&& (resolveEscapes || !originalText.hasEscape);
		UInt64 directProfilePublicationEpoch = 0;
		if (cacheable && g_bEnableFreeTypeFontStructuralFastPaths)
		{
			if (const vectorfont::RuntimeFont* runtime =
				vectorfont::FindActiveRuntime(font))
			{
				directProfilePublicationEpoch = vectorfont::
					GetRuntimeSealedDirectProfilePublicationEpoch(*runtime);
			}
		}
		PreparedTextCacheLookupKey cacheLookup;
		bool preparedTextStoreCandidate = false;
		if (cacheable)
		{
			cacheLookup = {
				font, font->pFontData, layoutIdentity,
				directProfilePublicationEpoch,
				GetPreparedTextMetricSignature(font, lineSpacingAdjust),
				0, GetFreeTypeTextCodePage(),
				axData->iWidth, axData->iHeight, axData->iLineStart,
				axData->iLineEnd, axData->cLineSep, isTerminal,
				std::string_view(apOrigString, originalTextLen), {},
				originalText.hash, 1469598103934665603ull
			};
			if (!originalText.hasEscape)
			{
				cacheLookup.iconSignature =
					GetPreparedTextIconSignature(font);
				RefreshPreparedTextLookupHash(cacheLookup);
				if (const std::shared_ptr<const PreparedTextCacheValue> cached =
					FindPreparedTextCacheValue(
						cacheLookup, preparedTextStoreCandidate))
				{
					ApplyPreparedTextCacheValue(
						*cached, *axData, font);
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

		thread_local PrepTextScratchPool cachedScratchPool;
		PrepTextScratchPool& scratchPool = cachedScratchPool;
		PrepTextScratchLease scratchLease(scratchPool);
		PrepTextScratch& scratch = scratchLease.Get();
		scratch.Prepare(apOrigString, sourceTextLen);
		char* processedOriginalText = scratch.original.data();
		char* dynamicTextBuffer = scratch.processed.data();

		UInt32 processedTextLen = 0;
		UInt32 textBufferSize = sourceTextLen + 4;
		UInt64 resolvedTextHash = 1469598103934665603ull;

		// ---- Pass 1: Process escape sequences (&variable;) ----
		const bool processedEscapes = resolveEscapes
			&& ProcessEscapeSequences(scratch.original, scratch.processed,
				textBufferSize, processedTextLen, origConsumed, sourceTextLen,
				resolvedTextHash, font, axData);
		processedOriginalText = scratch.original.data();
		dynamicTextBuffer = scratch.processed.data();
		if (cacheable && processedEscapes)
		{
			cacheLookup.iconSignature = GetPreparedTextIconSignature(font);
			cacheLookup.resolved = std::string_view(processedOriginalText, sourceTextLen);
			cacheLookup.resolvedHash = resolvedTextHash;
			RefreshPreparedTextLookupHash(cacheLookup);
			if (const std::shared_ptr<const PreparedTextCacheValue> cached =
				FindPreparedTextCacheValue(
					cacheLookup, preparedTextStoreCandidate))
			{
				ApplyPreparedTextCacheValue(
					*cached, *axData, font);
				return;
			}
		}

		if (IsFreeTypeFontActive(font))
		{
			std::shared_ptr<const PreparedDirectTextSidecar>
				directSidecar = PrepDirectFreeTypeText(
					font, processedOriginalText,
				sourceTextLen, axData, isTerminal, origConsumed, scratch);
			PublishPreparedTextSidecar(
				axData, font, directSidecar);
			if (preparedTextStoreCandidate)
			{
				const bool stableReject = directSidecar
					&& directSidecar->rejectBatch
					&& g_bEnableFreeTypeFontStructuralFastPaths
					&& directProfilePublicationEpoch != 0;
				if (directSidecar
					&& (!directSidecar->rejectBatch || stableReject))
				{
					const bool stored = StorePreparedTextCacheValue(cacheLookup,
						CapturePreparedTextCacheValue(
							*axData, std::move(directSidecar)));
					if (!stored)
					{
						RejectPreparedTextAdmission(
							cacheLookup.precomputedHash);
					}
					else if (stableReject)
					{
						vectorfont::RecordFreeTypePerf(
							vectorfont::FreeTypePerfCounter::
								PreparedTextRejectCacheStored);
					}
				}
				else
				{
					RejectPreparedTextAdmission(
						cacheLookup.precomputedHash);
				}
			}
			return;
		}

		UInt32 buttonIconIndex = 0;
		UInt32 previousUnitOutputStart = 0;
		bool hasPreviousUnitOutput = false;

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
				hasPreviousUnitOutput = false;
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
					FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
					if (glyph)
					{
						pCurrentGlyph = glyph;
						unitWidth = GetGlyphRenderAdvance(pCurrentGlyph);
					}
				}

				LayoutWrapResult wrapResult;
				if (!isSoftMarker)
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
						UInt32 tailStart = hasPreviousUnitOutput
							? previousUnitOutputStart : processedTextLen - 1;
						UInt32 tailBytes = processedTextLen - tailStart;
						if (!hasPreviousUnitOutput && processedTextLen >= 2)
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
				const UInt32 currentUnitOutputStart = processedTextLen;

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
				if (!isSoftMarker)
				{
					previousUnitOutputStart = currentUnitOutputStart;
					hasPreviousUnitOutput = true;
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
		if (preparedTextStoreCandidate)
		{
			if (!StorePreparedTextCacheValue(
				cacheLookup, CapturePreparedTextCacheValue(*axData)))
			{
				RejectPreparedTextAdmission(
					cacheLookup.precomputedHash);
			}
		}
	}

	// ==================== FontEx::PrepTextForTerminal ====================
	void __thiscall FontEx::PrepTextForTerminal(const char* apOrigString, Font::TextData* axData)
	{
		if (!g_bEnableMultibyteFontHook && !IsFreeTypeFontActive(this))
		{
			ThisStdCall(0xA12FB0, this, apOrigString, axData);
			return;
		}
		PrepTextImpl(this, apOrigString, axData, true);
	}

	// ==================== FontEx::PrepText ====================
	void __thiscall FontEx::PrepText(const char* apOrigString, Font::TextData* axData)
	{
		if (!g_bEnableMultibyteFontHook && !IsFreeTypeFontActive(this))
		{
			ThisStdCall(0xA12FB0, this, apOrigString, axData);
			return;
		}
		PrepTextImpl(this, apOrigString, axData, false);
	}

	bool MeasureFreeTypeSingleByteText(
		FontEx* font,
		const char* text,
		float maxWrapWidth,
		UInt32 startCharIndex,
		NiPoint3& dimensions)
	{
		if (!font || !font->pFontData || !text || !IsFreeTypeFontActive(font))
			return false;

		const size_t textLength = std::strlen(text);
		if (startCharIndex >= textLength)
		{
			dimensions.x = 0.0f;
			dimensions.y = font->pFontData->pFontLetters[kSpaceChar].fHeight;
			dimensions.z = 1.0f;
			return true;
		}

		int wrapWidth = std::numeric_limits<int>::max();
		if (std::isfinite(maxWrapWidth)
			&& maxWrapWidth > 0.0f
			&& maxWrapWidth < static_cast<float>(
				std::numeric_limits<int>::max()))
		{
			wrapWidth = std::max(
				1,
				static_cast<int>(std::floor(maxWrapWidth)));
		}

		Font::TextData prepared = {};
		ThisStdCall(0x759330, &prepared, wrapWidth,
			std::numeric_limits<int>::max(), 0,
			std::numeric_limits<int>::max(), '\n');
		PrepTextImpl(font, text + startCharIndex, &prepared, false, false);
		dimensions.x = static_cast<float>(prepared.iWidth);
		dimensions.y = static_cast<float>(prepared.iHeight);
		dimensions.z = static_cast<float>(prepared.iLineEnd);
		ThisStdCall<UInt32>(0x7593E0, reinterpret_cast<char*>(&prepared));
		return true;
	}

} // namespace fonthook
