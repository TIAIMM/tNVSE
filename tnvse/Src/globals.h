#pragma once
#include "MemoryManager.hpp"
#include "NiPoint3.hpp"
#include "ui_decode.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class NiPixelFormat;

namespace fonthook
{
	using ExtraGlyphMap = std::unordered_map<UInt32, FontLetter>;

	struct DirectExtraGlyphTable
	{
		static constexpr UInt32 kFirstLeadByte = 0x81;
		static constexpr UInt32 kLastLeadByte = 0xFE;
		static constexpr UInt32 kFirstTrailByte = 0x40;
		static constexpr UInt32 kLastTrailByte = 0xFE;
		static constexpr UInt32 kGlyphsPerRow =
			kLastTrailByte - kFirstTrailByte + 1;
		static constexpr UInt32 kIndexCount =
			(kLastLeadByte - kFirstLeadByte + 1) * kGlyphsPerRow;
		static constexpr UInt16 kMissingMetric =
			std::numeric_limits<UInt16>::max();

		std::unique_ptr<UInt16[]> metricIndices;
		std::vector<FontLetter> metrics;

		bool Initialize(size_t metricCapacity)
		{
			try
			{
				metricIndices = std::make_unique<UInt16[]>(kIndexCount);
				std::fill_n(metricIndices.get(), kIndexCount, kMissingMetric);
				metrics.reserve(std::min<size_t>(metricCapacity, kIndexCount));
				return true;
			}
			catch (const std::bad_alloc&)
			{
				metricIndices.reset();
				metrics.clear();
				return false;
			}
		}

		static bool ResolveIndex(UInt32 code, UInt32& index)
		{
			if (code & 0xFFFF0000u)
				return false;
			const UInt32 lead = (code >> 8) & 0xFFu;
			const UInt32 trail = code & 0xFFu;
			if (lead < kFirstLeadByte || lead > kLastLeadByte
				|| trail < kFirstTrailByte || trail > kLastTrailByte)
			{
				return false;
			}
			index = (lead - kFirstLeadByte) * kGlyphsPerRow
				+ (trail - kFirstTrailByte);
			return true;
		}

		bool Insert(UInt32 code, const FontLetter& metric)
		{
			UInt32 index = 0;
			if (!metricIndices || !ResolveIndex(code, index)
				|| metrics.size() >= kMissingMetric)
			{
				return false;
			}
			if (metricIndices[index] != kMissingMetric)
				return true;
			metricIndices[index] = static_cast<UInt16>(metrics.size());
			metrics.push_back(metric);
			return true;
		}

		FontLetter* find(UInt32 code)
		{
			UInt32 index = 0;
			if (!metricIndices || !ResolveIndex(code, index))
				return nullptr;
			const UInt16 metricIndex = metricIndices[index];
			return metricIndex != kMissingMetric && metricIndex < metrics.size()
				? &metrics[metricIndex] : nullptr;
		}

		size_t GetAllocatedBytes() const
		{
			return sizeof(*this)
				+ 2u * sizeof(void*)
				+ (metricIndices ? kIndexCount * sizeof(UInt16) : 0)
				+ metrics.capacity() * sizeof(FontLetter);
		}
	};

	// Serialized .fnt files store one dense FontLetter row for every lead byte
	// from 0x81 through 0xFE. Keep that immutable fallback representation dense;
	// incomplete FreeType manifests continue to use the sparse generated cache.
	struct SerializedExtraGlyphTable
	{
		static constexpr UInt32 kFirstLeadByte = 0x81;
		static constexpr UInt32 kLastLeadByte = 0xFE;
		static constexpr UInt32 kFirstTrailByte = 0x40;
		static constexpr UInt32 kLastTrailByte = 0xFE;
		static constexpr UInt32 kGlyphsPerRow =
			kLastTrailByte - kFirstTrailByte + 1;
		static constexpr UInt32 kGlyphCount =
			(kLastLeadByte - kFirstLeadByte + 1) * kGlyphsPerRow;

		std::unique_ptr<FontLetter[]> glyphs;
		UInt32 validGlyphs = 0;

		bool empty() const
		{
			return !glyphs || !validGlyphs;
		}

		void clear()
		{
			glyphs.reset();
			validGlyphs = 0;
		}

		FontLetter* find(UInt32 code) const
		{
			if (!glyphs || (code & 0xFFFF0000u))
				return nullptr;
			const UInt32 lead = (code >> 8) & 0xFFu;
			const UInt32 trail = code & 0xFFu;
			if (lead < kFirstLeadByte || lead > kLastLeadByte
				|| trail < kFirstTrailByte || trail > kLastTrailByte)
			{
				return nullptr;
			}
			const UInt32 index = (lead - kFirstLeadByte) * kGlyphsPerRow
				+ (trail - kFirstTrailByte);
			return index < validGlyphs ? &glyphs[index] : nullptr;
		}
	};

	static_assert(DirectExtraGlyphTable::kIndexCount
		== SerializedExtraGlyphTable::kGlyphCount);

	struct ExtraGlyphStore
	{
		SerializedExtraGlyphTable serialized;
		std::shared_ptr<DirectExtraGlyphTable> generatedCodePage;
		ExtraGlyphMap generated;

		bool empty() const
		{
			return serialized.empty() && !generatedCodePage && generated.empty();
		}
	};

	// ---- Font letter caches (defined in font_hook.cpp) ----
	extern std::unordered_map<std::string, ExtraGlyphStore> gExtraFontLetters;
	extern std::unordered_map<UInt32, ExtraGlyphStore> gNumberedExtraLetters;

	// ---- Quest text double-byte character state machine (defined in font_hook.cpp) ----
	extern UInt8 pFirstChar;
	extern bool bIsQuestTextMSBDBCharacter;
	extern bool bIsQuestTextLSBDBCharacter;
	extern bool bMeasureQuestTextMSBAsEmpty;
	extern char szDBChar[3];

	// ---- Memory / rendering singletons (defined in native_calls.cpp) ----
	extern MemoryManager* MemoryManager_s_Instance;
	extern const NiPoint3& NiPoint3_ZERO;
	extern const NiPixelFormat& NiPixelFormat_RGBA32;

} // namespace fonthook
