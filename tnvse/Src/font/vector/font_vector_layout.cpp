#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

namespace fonthook::vectorfont
{
		void TouchLayoutCacheEntry(FreeTypeState& state, LayoutCacheEntry& entry)
		{
			state.layoutLru.splice(state.layoutLru.begin(), state.layoutLru, entry.lru);
			entry.lru = state.layoutLru.begin();
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

		struct LayoutInputGlyph
		{
			VectorEncodedGlyph glyph;
			ResolvedGlyph resolved;
			UInt32 byteOffset = 0;
		};

		struct LayoutInputScratchPool
		{
			struct Slot
			{
				std::vector<LayoutInputGlyph> input;
				CpuMemoryLease cpuMemory;
			};

			std::array<Slot, 4> slots;
			size_t depth = 0;
		};

		class LayoutInputScratchLease
		{
		public:
			explicit LayoutInputScratchLease(LayoutInputScratchPool& pool) : m_pool(pool)
			{
				if (m_pool.depth < m_pool.slots.size())
				{
					m_slot = &m_pool.slots[m_pool.depth];
					m_input = &m_slot->input;
				}
				else
				{
					m_input = &m_fallback;
				}
				++m_pool.depth;
				m_input->clear();
			}

			~LayoutInputScratchLease()
			{
				constexpr size_t kMaximumRetainedLayoutUnits = 8192;
				m_input->clear();
				if (m_input->capacity() > kMaximumRetainedLayoutUnits)
					std::vector<LayoutInputGlyph>().swap(*m_input);
				if (m_slot)
				{
					m_slot->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
						m_input->capacity() * sizeof(LayoutInputGlyph));
				}
				--m_pool.depth;
			}

			std::vector<LayoutInputGlyph>& Get() { return *m_input; }

		private:
			LayoutInputScratchPool& m_pool;
			std::vector<LayoutInputGlyph> m_fallback;
			LayoutInputScratchPool::Slot* m_slot = nullptr;
			std::vector<LayoutInputGlyph>* m_input = nullptr;
		};

		hb_language_t GetLayoutLanguage()
		{
			thread_local UInt32 cachedCodePage = UINT32_MAX;
			thread_local hb_language_t cachedLanguage = HB_LANGUAGE_INVALID;
			const UInt32 codePage = GetFreeTypeTextCodePage();
			if (cachedCodePage == codePage && cachedLanguage != HB_LANGUAGE_INVALID)
				return cachedLanguage;
			const char* language = "en";
			switch (codePage)
			{
			case 936: language = "zh-Hans"; break;
			case 950: language = "zh-Hant"; break;
			case 932: language = "ja"; break;
			case 949: language = "ko"; break;
			default: break;
			}
			cachedCodePage = codePage;
			cachedLanguage = hb_language_from_string(language, -1);
			return cachedLanguage;
		}

		hb_buffer_t* GetThreadHarfBuzzBuffer()
		{
			thread_local hb_buffer_t* buffer = hb_buffer_create();
			if (buffer)
				hb_buffer_clear_contents(buffer);
			return buffer;
		}

		bool DecodeLayoutUnit(RuntimeFont& runtime, const char* text,
			size_t length, size_t offset, LayoutInputGlyph& output)
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

		bool DecodeLayoutInput(RuntimeFont& runtime,
			const char* text, size_t length, std::vector<LayoutInputGlyph>& input)
		{
			input.clear();
			for (size_t offset = 0; offset < length;)
			{
				LayoutInputGlyph item;
				if (!DecodeLayoutUnit(runtime, text, length, offset, item))
					return false;
				offset += item.glyph.byteLength;
				input.push_back(std::move(item));
			}
			return true;
		}

		void AppendDirectLayoutGlyph(RuntimeRole& role, RuntimeFace& face,
			const VectorEncodedGlyph& glyph, FT_UInt glyphIndex, UInt32 byteOffset,
			FT_UInt previousGlyph, FreeTypeLayoutRun::GlyphStorage& glyphs,
			FreeTypeLayoutRun& layout)
		{
			const bool fixedCell = role.style->fixedWidth > 0.0f;
			float kerning = 0.0f;
			if (!fixedCell && previousGlyph && glyphIndex && FT_HAS_KERNING(face.face))
			{
				const UInt64 kerningKey = (static_cast<UInt64>(previousGlyph) << 32)
					| glyphIndex;
				const UInt64 mixedKey = kerningKey
					^ ((kerningKey >> 33) * 0x9E3779B185EBCA87ull);
				DirectLayoutKerningEntry& cached = face.directKerning[
					static_cast<size_t>(mixedKey & (face.directKerning.size() - 1))];
				if (cached.key == kerningKey)
				{
					kerning = cached.value;
					RecordFreeTypePerf(FreeTypePerfCounter::DirectKerningHit);
				}
				else
				{
					FT_Vector delta = {};
					if (!FT_Get_Kerning(face.face, previousGlyph, glyphIndex,
						FT_KERNING_DEFAULT, &delta))
					{
						kerning = static_cast<float>(delta.x) / 64.0f;
					}
					cached = { kerningKey, kerning };
					RecordFreeTypePerf(FreeTypePerfCounter::DirectKerningMiss);
				}
			}

			DirectLayoutGlyphMetric fallbackMetric;
			DirectLayoutGlyphMetric* metric = face.directLayoutMetrics.Find(glyphIndex);
			if (metric && metric->valid)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutMetricHit);
			}
			else
			{
				RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutMetricMiss);
				if (LoadGlyph(role, face, glyphIndex))
				{
					metric = face.directLayoutMetrics.Find(glyphIndex);
					if (!metric || !metric->valid)
					{
						// Sparse pages intentionally cover the 16-bit OpenType glyph space.
						// Preserve correct layout for a wider glyph index or an allocation
						// failure by using the live FreeType slot for this one layout unit.
						fallbackMetric.advance = static_cast<float>(
							face.face->glyph->advance.x) / 64.0f;
						fallbackMetric.fixedOffset = GetFixedCellGlyphOffset(
							*role.style, face.face->glyph);
						fallbackMetric.valid = true;
						metric = &fallbackMetric;
					}
				}
				else
					metric = nullptr;
			}

			FreeTypeLayoutGlyph positioned;
			positioned.glyph = glyph;
			positioned.cluster = byteOffset;
			positioned.xOffset = fixedCell
				? (metric && metric->valid ? metric->fixedOffset : 0.0f)
				: kerning;
			positioned.xAdvance = fixedCell
				? GetFixedCellAdvance(*role.style)
				: (metric && metric->valid ? metric->advance : 0.0f)
					+ role.style->tracking + kerning;
			layout.advance += positioned.xAdvance;
			positioned.glyph.metrics = nullptr;
			glyphs.push_back(std::move(positioned));
		}

		void AppendPreciseLayout(RuntimeFont& runtime,
			const std::vector<LayoutInputGlyph>& input, size_t begin, size_t end,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			if (begin >= end)
				return;
			RuntimeRole& role = *input[begin].resolved.role;
			RuntimeFace& face = *input[begin].resolved.runtimeFace;
			if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
				return;
			FT_UInt previousGlyph = 0;
			for (size_t index = begin; index < end; ++index)
			{
				const LayoutInputGlyph& item = input[index];
				AppendDirectLayoutGlyph(role, face, item.glyph,
					item.resolved.glyphIndex, item.byteOffset, previousGlyph,
					glyphs, layout);
				previousGlyph = item.resolved.glyphIndex;
			}
		}

		bool AppendDirectLayout(RuntimeFont& runtime, const char* text, size_t length,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			RuntimeRole* previousRole = nullptr;
			RuntimeFace* previousFace = nullptr;
			VectorFontByteClass previousByteClass = VectorFontByteClass::SingleByte;
			FT_UInt previousGlyph = 0;
			for (size_t offset = 0; offset < length;)
			{
				LayoutInputGlyph item;
				if (!DecodeLayoutUnit(runtime, text, length, offset, item))
					return false;
				RuntimeRole& role = *item.resolved.role;
				RuntimeFace& face = *item.resolved.runtimeFace;
				if (previousRole != &role || previousFace != &face
					|| previousByteClass != item.glyph.byteClass)
				{
					if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
						return false;
					previousGlyph = 0;
				}
				AppendDirectLayoutGlyph(role, face, item.glyph,
					item.resolved.glyphIndex, item.byteOffset, previousGlyph,
					glyphs, layout);
				previousRole = &role;
				previousFace = &face;
				previousByteClass = item.glyph.byteClass;
				previousGlyph = item.resolved.glyphIndex;
				offset += item.glyph.byteLength;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::DirectLayoutRun);
			return true;
		}

		bool AppendHarfBuzzLayout(FreeTypeState& state, RuntimeFont& runtime,
			const std::vector<LayoutInputGlyph>& input, size_t begin, size_t end,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			if (begin >= end)
				return true;
			RuntimeRole& role = *input[begin].resolved.role;
			RuntimeFace& face = *input[begin].resolved.runtimeFace;
			if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
				return false;

			if (!face.hbFont)
			{
				face.hbFont = hb_ft_font_create_referenced(face.face);
				if (face.hbFont)
					hb_ft_font_set_load_flags(face.hbFont, kGlyphLoadFlags);
			}
			hb_font_t* hbFont = face.hbFont;
			hb_buffer_t* buffer = GetThreadHarfBuzzBuffer();
			if (!hbFont || !buffer)
				return false;
			for (size_t index = begin; index < end; ++index)
			{
				hb_buffer_add(buffer, input[index].resolved.renderedCodePoint,
					input[index].byteOffset);
			}
			hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
			hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
			hb_buffer_set_language(buffer, GetLayoutLanguage());
			hb_buffer_guess_segment_properties(buffer);

			RecordFreeTypePerf(FreeTypePerfCounter::HarfBuzzShape);
			hb_shape(hbFont, buffer,
				runtime.hbFeatures.empty() ? nullptr : runtime.hbFeatures.data(),
				static_cast<unsigned int>(runtime.hbFeatures.size()));
			unsigned int glyphCount = 0;
			const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &glyphCount);
			const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);
			if (!glyphCount || !infos || !positions)
				return false;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const VectorFontByteClass byteClass = input[begin].glyph.byteClass;
				const UInt64 logKey = (static_cast<UInt64>(runtime.config->fontId) << 8)
					| static_cast<UInt8>(byteClass);
				if (state.loggedHarfBuzzVerticalRoles.insert(logKey).second)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: HarfBuzz vertical metrics font=%u role=%s glyph=%u cluster=%u yAdvance=%.2f yOffset=%.2f resolvedBaselineOffset=%.2f",
						runtime.config->fontId,
						byteClass == VectorFontByteClass::DoubleByte ? "doubleByte" : "singleByte",
						infos[0].codepoint, infos[0].cluster,
						static_cast<float>(positions[0].y_advance) / 64.0f,
						static_cast<float>(positions[0].y_offset) / 64.0f,
						role.resolvedBaselineOffset);
				}
			}

			// MONOTONE_CHARACTERS plus the forced LTR direction guarantees that
			// output clusters never move backwards. Advance the source cursor once
			// across the run instead of rescanning every input glyph for every shaped
			// glyph (quadratic for long runs).
			size_t sourceIndex = begin;
			for (unsigned int glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex)
			{
				const UInt32 cluster = infos[glyphIndex].cluster;
				while (sourceIndex + 1 < end
					&& input[sourceIndex + 1].byteOffset <= cluster)
				{
					++sourceIndex;
				}
				FreeTypeLayoutGlyph positioned;
				positioned.glyph = input[sourceIndex].glyph;
				positioned.glyph.glyphIndex = infos[glyphIndex].codepoint;
				positioned.glyph.faceIndex = static_cast<UInt16>(input[begin].resolved.faceIndex);
				positioned.glyph.hasGlyphIdentity = true;
				positioned.cluster = cluster;
				positioned.xAdvance = static_cast<float>(positions[glyphIndex].x_advance) / 64.0f;
				positioned.xOffset = static_cast<float>(positions[glyphIndex].x_offset) / 64.0f;
				positioned.yOffset = static_cast<float>(positions[glyphIndex].y_offset) / 64.0f;
				const bool clusterEnd = glyphIndex + 1 == glyphCount
					|| infos[glyphIndex + 1].cluster != cluster;
				if (clusterEnd)
					positioned.xAdvance += role.style->tracking;
				layout.advance += positioned.xAdvance;
				positioned.glyph.metrics = nullptr;
				glyphs.push_back(std::move(positioned));
			}
			layout.shaped = true;
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
				lookup.allowShaping,
				std::string(lookup.text)
			};
			entry.layout = layout;
			entry.valid = true;
		}

		struct CollisionSpan
		{
			float top = 0.0f;
			float bottom = 0.0f;
			float left = 0.0f;
			float right = 0.0f;
		};

		struct CollisionScratch
		{
			std::vector<CollisionSpan> previous;
			std::vector<CollisionSpan> current;
			CpuMemoryLease cpuMemory;
		};

		thread_local CollisionScratch s_collisionScratch;

		void AppendCollisionSpans(RuntimeFont& runtime,
			const FreeTypeLayoutGlyph& positioned, float glyphPen,
			std::vector<CollisionSpan>& spans)
		{
			GlyphCollisionProfile profile;
			if (!LoadGlyphCollisionProfile(runtime, positioned.glyph, profile)
				|| !profile.bandMask || profile.top <= profile.bottom)
			{
				return;
			}
			const float bandHeight = (profile.top - profile.bottom)
				/ static_cast<float>(kGlyphCollisionBandCount);
			const float originX = glyphPen + positioned.xOffset;
			const float originY = positioned.yOffset
				+ GetGlyphBaselineOffset(runtime, positioned.glyph);
			for (size_t band = 0; band < kGlyphCollisionBandCount; ++band)
			{
				if (!(profile.bandMask & static_cast<UInt16>(1u << band)))
					continue;
				CollisionSpan span;
				span.top = originY + profile.top
					- static_cast<float>(band) * bandHeight;
				span.bottom = span.top - bandHeight;
				span.left = originX + profile.left[band];
				span.right = originX + profile.right[band];
				if (span.right > span.left)
					spans.push_back(span);
			}
		}

		void ApplyGlyphCollisionProtection(RuntimeFont& runtime,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			if (!g_bEnableFreeTypeGlyphCollisionProtection || glyphs.size() < 2)
				return;
			constexpr float kCollisionClearance = 1.0f / 64.0f;
			constexpr float kMaximumCollisionCorrection = 1.0f;
			std::vector<CollisionSpan>& previousSpans = s_collisionScratch.previous;
			std::vector<CollisionSpan>& currentSpans = s_collisionScratch.current;
			previousSpans.clear();
			currentSpans.clear();
			previousSpans.reserve(kGlyphCollisionBandCount * 2);
			currentSpans.reserve(kGlyphCollisionBandCount * 2);
			float pen = 0.0f;
			size_t previousEnd = 0;
			for (size_t begin = 0; begin < glyphs.size();)
			{
				size_t end = begin + 1;
				while (end < glyphs.size()
					&& glyphs[end].cluster == glyphs[begin].cluster)
				{
					++end;
				}
				currentSpans.clear();
				for (size_t index = begin; index < end; ++index)
				{
					AppendCollisionSpans(runtime, glyphs[index], pen, currentSpans);
					pen += glyphs[index].xAdvance;
				}

				float correction = 0.0f;
				if (previousEnd && !previousSpans.empty() && !currentSpans.empty())
				{
					const FreeTypeLayoutGlyph& terminal = glyphs[previousEnd - 1];
					const RuntimeRole& role = runtime.roles[static_cast<size_t>(
						terminal.glyph.byteClass)];
					if (role.style && role.style->tracking <= 0.0f)
					{
						for (const CollisionSpan& previous : previousSpans)
						{
							for (const CollisionSpan& current : currentSpans)
							{
								if (std::min(previous.top, current.top)
									<= std::max(previous.bottom, current.bottom))
								{
									continue;
								}
								correction = std::max(correction,
									previous.right - current.left + kCollisionClearance);
							}
						}
					}
				}
				correction = std::clamp(correction, 0.0f,
					kMaximumCollisionCorrection);
				if (correction > 0.0f)
				{
					glyphs[previousEnd - 1].xAdvance += correction;
					layout.advance += correction;
					pen += correction;
					for (CollisionSpan& span : currentSpans)
					{
						span.left += correction;
						span.right += correction;
					}
				}
				previousSpans.swap(currentSpans);
				previousEnd = end;
				begin = end;
			}
			previousSpans.clear();
			currentSpans.clear();
			constexpr size_t kMaximumRetainedCollisionSpans = 256;
			if (previousSpans.capacity() > kMaximumRetainedCollisionSpans)
				std::vector<CollisionSpan>().swap(previousSpans);
			if (currentSpans.capacity() > kMaximumRetainedCollisionSpans)
				std::vector<CollisionSpan>().swap(currentSpans);
			s_collisionScratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				(previousSpans.capacity() + currentSpans.capacity())
					* sizeof(CollisionSpan));
		}

		bool BuildLayoutRun(FreeTypeState& state, RuntimeFont& runtime, const char* text,
			size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
		{
			layout = {};
			auto glyphs = std::make_shared<FreeTypeLayoutRun::GlyphStorage>();
			layout.glyphs = glyphs;
			const bool shape = allowShaping && runtime.config->shaping;
			if (!shape)
			{
				glyphs->reserve(std::min<size_t>(length, 65536));
				if (!AppendDirectLayout(runtime, text, length, *glyphs, layout))
					return false;
				ApplyGlyphCollisionProtection(runtime, *glyphs, layout);
				glyphs->cpuMemory.Reset(CpuMemoryCategory::LayoutRun,
					sizeof(FreeTypeLayoutRun::GlyphStorage)
						+ glyphs->capacity() * sizeof(FreeTypeLayoutGlyph)
						+ 2u * sizeof(void*));
				return true;
			}

			thread_local LayoutInputScratchPool inputScratchPool;
			LayoutInputScratchLease inputLease(inputScratchPool);
			std::vector<LayoutInputGlyph>& input = inputLease.Get();
			input.reserve(std::min<size_t>(length, 65536));
			if (!DecodeLayoutInput(runtime, text, length, input))
				return false;
			glyphs->reserve(input.size());
			for (size_t begin = 0; begin < input.size();)
			{
				size_t end = begin + 1;
				while (end < input.size()
					&& input[end].glyph.byteClass == input[begin].glyph.byteClass
					&& input[end].resolved.runtimeFace == input[begin].resolved.runtimeFace)
				{
					++end;
				}
				RuntimeRole& groupRole = *input[begin].resolved.role;
				bool canShapeGroup = shape && groupRole.style->fixedWidth <= 0.0f;
				for (size_t index = begin; canShapeGroup && index < end; ++index)
				{
					canShapeGroup = input[index].resolved.glyphIndex != 0
						&& input[index].resolved.renderedCodePoint == input[index].glyph.codePoint;
				}
				if (canShapeGroup)
				{
					const size_t glyphStart = glyphs->size();
					const float advanceStart = layout.advance;
					if (!AppendHarfBuzzLayout(state, runtime, input, begin, end, *glyphs, layout))
					{
						if (g_bEnableFreeTypeFontRenderingLog && state.shapingFallbackLogCount < 32)
						{
							++state.shapingFallbackLogCount;
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: HarfBuzz run failed font=%u units=%u; using precise FreeType kerning",
								runtime.config->fontId, static_cast<UInt32>(end - begin));
						}
						glyphs->resize(glyphStart);
						layout.advance = advanceStart;
						AppendPreciseLayout(runtime, input, begin, end, *glyphs, layout);
					}
				}
				else
				{
					AppendPreciseLayout(runtime, input, begin, end, *glyphs, layout);
				}
				begin = end;
			}
			ApplyGlyphCollisionProtection(runtime, *glyphs, layout);
			glyphs->cpuMemory.Reset(CpuMemoryCategory::LayoutRun,
				sizeof(FreeTypeLayoutRun::GlyphStorage)
					+ glyphs->capacity() * sizeof(FreeTypeLayoutGlyph)
					+ 2u * sizeof(void*));
			return true;
		}

	bool LayoutRuntimeRun(RuntimeFont& runtime, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout,
		bool finalRun)
	{
		const UInt32 codePage = GetFreeTypeTextCodePage();
		const LayoutCacheLookupKey lookup = {
			runtime.config->layoutHash,
			codePage,
			allowShaping,
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
		if (!BuildLayoutRun(state, runtime, text, length, allowShaping, layout))
			return false;
		if (length <= 65536)
		{
			LayoutCacheKey key = {
				runtime.config->layoutHash,
				codePage,
				allowShaping,
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
