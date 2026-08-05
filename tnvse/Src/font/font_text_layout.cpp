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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
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

	static void ProcessEscapeSequences(
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

	static UInt32 GetPreparedTextLength(const char* text)
	{
		if (!text)
			return 0;
		UInt32 length = 0;
		while (length != std::numeric_limits<UInt32>::max()
			&& text[length])
			++length;
		return length;
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
		const UInt32 originalTextLen = GetPreparedTextLength(apOrigString);
		const float lineSpacingAdjust = FontManager::GetLinePadding(font->iFontNum);

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
		// ---- Pass 1: Process escape sequences (&variable;) ----
		if (resolveEscapes)
		{
			ProcessEscapeSequences(scratch.original, scratch.processed,
				textBufferSize, processedTextLen, origConsumed, sourceTextLen,
				font, axData);
		}
		processedOriginalText = scratch.original.data();
		dynamicTextBuffer = scratch.processed.data();

		if (IsFreeTypeFontActive(font))
		{
			std::shared_ptr<const PreparedDirectTextSidecar>
				directSidecar = PrepDirectFreeTypeText(
					font, processedOriginalText,
				sourceTextLen, axData, isTerminal, origConsumed, scratch);
			PublishPreparedTextSidecar(
				axData, font, directSidecar);
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
