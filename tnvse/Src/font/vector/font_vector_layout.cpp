#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace fonthook::vectorfont
{
	namespace
	{
		struct LegacyLayoutUnit
		{
			VectorEncodedGlyph glyph;
			ResolvedGlyph resolved;
			UInt32 byteOffset = 0;
		};

		void TouchLayoutCacheEntry(FreeTypeState& state, LayoutCacheEntry& entry)
		{
			state.layoutLru.splice(state.layoutLru.begin(), state.layoutLru, entry.lru);
			entry.lru = state.layoutLru.begin();
		}

		bool DecodeLegacyLayoutUnit(RuntimeFont& runtime, const char* text,
			size_t length, size_t offset, LegacyLayoutUnit& output)
		{
			if (!text || offset >= length)
				return false;

			const char* encodedText = text + offset;
			char normalizedSingleByte[2] = { text[offset], 0 };
			UInt32 dbcsCode = 0;
			if (!TryDecodeFreeTypeDoubleByte(text + offset, dbcsCode))
			{
				UInt8 normalized = static_cast<UInt8>(normalizedSingleByte[0]);
				ConvertToAsciiQuotes(&normalized);
				normalizedSingleByte[0] = static_cast<char>(normalized);
				encodedText = normalizedSingleByte;
			}

			VectorEncodedGlyph glyph;
			if (!DecodeEncodedGlyphIdentity(runtime, encodedText, glyph)
				|| !glyph.byteLength || offset + glyph.byteLength > length)
			{
				return false;
			}

			ResolvedGlyph resolved;
			if (!ResolveVectorGlyph(runtime, glyph, resolved))
				return false;
			ApplyResolvedIdentity(glyph, resolved);
			output = { glyph, resolved, static_cast<UInt32>(offset) };
			return true;
		}

		bool AppendLegacyLayoutGlyph(const LegacyLayoutUnit& item,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			if (!item.resolved.role || !item.resolved.runtimeFace
				|| !item.resolved.role->style)
			{
				return false;
			}

			RuntimeRole& role = *item.resolved.role;
			RuntimeFace& face = *item.resolved.runtimeFace;
			if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
				return false;

			DirectLayoutGlyphMetric fallbackMetric;
			DirectLayoutGlyphMetric* metric = face.directLayoutMetrics.Find(
				item.resolved.glyphIndex);
			if (metric && metric->valid)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutMetricHit);
			}
			else
			{
				RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutMetricMiss);
				if (LoadGlyph(role, face, item.resolved.glyphIndex))
				{
					metric = face.directLayoutMetrics.Find(item.resolved.glyphIndex);
					if (!metric || !metric->valid)
					{
						fallbackMetric.advance = static_cast<float>(
							face.face->glyph->advance.x) / 64.0f;
						fallbackMetric.fixedOffset = GetFixedCellGlyphOffset(
							*role.style, face.face->glyph);
						fallbackMetric.valid = true;
						metric = &fallbackMetric;
					}
				}
				else
				{
					metric = nullptr;
				}
			}

			const bool fixedCell = role.style->fixedWidth > 0.0f;
			FreeTypeLayoutGlyph positioned;
			positioned.glyph = item.glyph;
			positioned.cluster = item.byteOffset;
			positioned.xOffset = fixedCell && metric && metric->valid
				? metric->fixedOffset : 0.0f;
			positioned.xAdvance = fixedCell
				? GetFixedCellAdvance(*role.style)
				: (metric && metric->valid ? metric->advance : 0.0f)
					+ role.style->tracking;
			positioned.yOffset = 0.0f;
			positioned.glyph.metrics = nullptr;
			layout.advance += positioned.xAdvance;
			glyphs.push_back(std::move(positioned));
			return true;
		}

		bool BuildLegacyLayoutRun(RuntimeFont& runtime, const char* text,
			size_t length, FreeTypeLayoutRun& layout)
		{
			layout = {};
			auto glyphs = std::make_shared<FreeTypeLayoutRun::GlyphStorage>();
			layout.glyphs = glyphs;
			glyphs->reserve(std::min<size_t>(length, 65536));

			for (size_t offset = 0; offset < length;)
			{
				LegacyLayoutUnit item;
				if (!DecodeLegacyLayoutUnit(runtime, text, length, offset, item))
					return false;
				if (!AppendLegacyLayoutGlyph(item, *glyphs, layout))
					return false;
				offset += item.glyph.byteLength;
			}

			layout.shaped = false;
			glyphs->cpuMemory.Reset(CpuMemoryCategory::LayoutRun,
				sizeof(FreeTypeLayoutRun::GlyphStorage)
					+ glyphs->capacity() * sizeof(FreeTypeLayoutGlyph)
					+ 2u * sizeof(void*));
			RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutRun);
			return true;
		}

		FinalLayoutHotCacheEntry* GetFinalLayoutHotCacheEntry(
			FreeTypeThreadState& thread, const LayoutCacheLookupKey& lookup,
			size_t hash)
		{
			FinalLayoutHotCacheEntry& entry =
				thread.finalLayouts[hash % thread.finalLayouts.size()];
			if (!entry.valid || entry.hash != hash)
				return nullptr;
			const LayoutCacheKeyEqual equal;
			return equal(entry.key, lookup) ? &entry : nullptr;
		}

		void StoreFinalLayoutHotCacheEntry(FreeTypeThreadState& thread,
			const LayoutCacheLookupKey& lookup, size_t hash,
			const FreeTypeLayoutRun& layout)
		{
			constexpr size_t kMaximumFinalLayoutHotTextBytes = 65536;
			if (!layout.glyphs || lookup.text.size() > kMaximumFinalLayoutHotTextBytes)
				return;
			FinalLayoutHotCacheEntry& entry =
				thread.finalLayouts[hash % thread.finalLayouts.size()];
			entry.hash = hash;
			entry.key = {
				lookup.layoutHash,
				lookup.codePage,
				false,
				std::string(lookup.text)
			};
			entry.layout = layout;
			entry.valid = true;
		}
	}

	void TrimLayoutCache(FreeTypeState& state)
	{
		const size_t limit = GetCpuMemoryCategoryHeadroom(
			CpuMemoryCategory::LayoutRun, GetLayoutCacheLimit());
		while ((GetCpuMemoryUsage(CpuMemoryCategory::LayoutRun) > limit
				|| IsCpuMemoryBudgetExceeded())
			&& !state.layoutLru.empty())
		{
			const LayoutCacheKey& key = state.layoutLru.back();
			auto it = state.layoutCache.find(key);
			if (it != state.layoutCache.end())
			{
				state.layoutCacheBytes -= it->second.bytes;
				state.layoutCache.erase(it);
			}
			state.layoutLru.pop_back();
		}
	}

	bool LayoutRuntimeRun(RuntimeFont& runtime, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout,
		bool finalRun)
	{
		(void)allowShaping;
		layout = {};
		if (!runtime.config || !text || !length)
			return false;

		const UInt32 codePage = GetFreeTypeTextCodePage();
		const LayoutCacheLookupKey lookup = {
			runtime.config->layoutHash,
			codePage,
			false,
			std::string_view(text, length)
		};
		const size_t lookupHash = LayoutCacheKeyHash{}(lookup);
		FreeTypeThreadState& thread = ThreadState();
		if (finalRun)
		{
			if (FinalLayoutHotCacheEntry* hot = GetFinalLayoutHotCacheEntry(
				thread, lookup, lookupHash))
			{
				layout = hot->layout;
				RecordFreeTypePerf(FreeTypePerfCounter::LayoutHit);
				return true;
			}
		}

		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		auto existing = state.layoutCache.find(lookup);
		if (existing != state.layoutCache.end())
		{
			TouchLayoutCacheEntry(state, existing->second);
			layout = existing->second.layout;
			if (finalRun)
				StoreFinalLayoutHotCacheEntry(thread, lookup, lookupHash, layout);
			RecordFreeTypePerf(FreeTypePerfCounter::LayoutHit);
			return true;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::LayoutMiss);
		if (!BuildLegacyLayoutRun(runtime, text, length, layout))
			return false;

		if (length <= 65536)
		{
			LayoutCacheKey key = {
				runtime.config->layoutHash,
				codePage,
				false,
				std::string(text, length)
			};
			FreeTypeLayoutRun cachedLayout = layout;
			const size_t bytes = sizeof(LayoutCacheEntry) + key.text.capacity()
				+ layout.glyphs->capacity() * sizeof(FreeTypeLayoutGlyph);
			const size_t cacheOverhead = sizeof(LayoutCacheEntry)
				+ 2u * sizeof(LayoutCacheKey) + 2u * key.text.capacity()
				+ 4u * sizeof(void*);
			state.layoutLru.push_front(key);
			const auto [inserted, success] = state.layoutCache.emplace(std::move(key),
				LayoutCacheEntry{ std::move(cachedLayout), bytes, state.layoutLru.begin() });
			if (!success)
			{
				state.layoutLru.pop_front();
			}
			else
			{
				inserted->second.cpuMemory.Reset(CpuMemoryCategory::LayoutRun,
					cacheOverhead);
				state.layoutCacheBytes += bytes;
				TrimLayoutCache(state);
			}
		}

		if (finalRun)
			StoreFinalLayoutHotCacheEntry(thread, lookup, lookupHash, layout);
		return true;
	}
}
