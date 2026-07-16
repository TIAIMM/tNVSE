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
			while (state.layoutCacheBytes > GetLayoutCacheLimit() && !state.layoutLru.empty())
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
			std::array<std::vector<LayoutInputGlyph>, 4> slots;
			size_t depth = 0;
		};

		class LayoutInputScratchLease
		{
		public:
			explicit LayoutInputScratchLease(LayoutInputScratchPool& pool) : m_pool(pool)
			{
				m_input = m_pool.depth < m_pool.slots.size()
					? &m_pool.slots[m_pool.depth] : &m_fallback;
				++m_pool.depth;
				m_input->clear();
			}

			~LayoutInputScratchLease()
			{
				constexpr size_t kMaximumRetainedLayoutUnits = 8192;
				m_input->clear();
				if (m_input->capacity() > kMaximumRetainedLayoutUnits)
					std::vector<LayoutInputGlyph>().swap(*m_input);
				--m_pool.depth;
			}

			std::vector<LayoutInputGlyph>& Get() { return *m_input; }

		private:
			LayoutInputScratchPool& m_pool;
			std::vector<LayoutInputGlyph> m_fallback;
			std::vector<LayoutInputGlyph>* m_input = nullptr;
		};

		hb_language_t GetLayoutLanguage()
		{
			thread_local UInt32 cachedEncoding = UINT32_MAX;
			thread_local hb_language_t cachedLanguage = HB_LANGUAGE_INVALID;
			if (cachedEncoding == g_uiEncoding && cachedLanguage != HB_LANGUAGE_INVALID)
				return cachedLanguage;
			const char* language = "en";
			switch (g_uiEncoding)
			{
			case 1: language = "zh-Hans"; break;
			case 2: language = "zh-Hant"; break;
			case 3: language = "ja"; break;
			case 4: language = "ko"; break;
			default: break;
			}
			cachedEncoding = g_uiEncoding;
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

		bool DecodeLayoutInput(RuntimeFont& runtime,
			const char* text, size_t length, std::vector<LayoutInputGlyph>& input)
		{
			input.clear();
			for (size_t offset = 0; offset < length;)
			{
				const char* encodedText = text + offset;
				char normalizedSingleByte[2] = { text[offset], 0 };
				UInt32 dbcsCode = 0;
				if (!TryDecodeDoubleByte(text + offset, dbcsCode))
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
				input.push_back({ glyph, resolved, static_cast<UInt32>(offset) });
				offset += glyph.byteLength;
			}
			return true;
		}

		void AppendPreciseLayout(RuntimeFont& runtime,
			const std::vector<LayoutInputGlyph>& input, size_t begin, size_t end,
			FreeTypeLayoutRun::GlyphStorage& glyphs, FreeTypeLayoutRun& layout)
		{
			if (begin >= end)
				return;
			RuntimeRole& role = *input[begin].resolved.role;
			RuntimeFace& face = *input[begin].resolved.runtimeFace;
			ConfigureRuntimeFace(face, *role.style, 1.0f, false);
			const bool fixedCell = role.style->fixedWidth > 0.0f;
			const bool haveKerning = FT_HAS_KERNING(face.face) != 0;
			const float fixedAdvance = fixedCell
				? GetFixedCellAdvance(*role.style) : 0.0f;
			FT_UInt previousGlyph = 0;
			for (size_t index = begin; index < end; ++index)
			{
				const LayoutInputGlyph& item = input[index];
				float kerning = 0.0f;
				if (!fixedCell && previousGlyph && item.resolved.glyphIndex
					&& haveKerning)
				{
					FT_Vector delta = {};
					if (!FT_Get_Kerning(face.face, previousGlyph,
						item.resolved.glyphIndex, FT_KERNING_DEFAULT, &delta))
					{
						kerning = static_cast<float>(delta.x) / 64.0f;
					}
				}
				LoadGlyph(role, face, item.resolved.glyphIndex);
				const float baseAdvance = static_cast<float>(face.face->glyph->advance.x) / 64.0f;
				FreeTypeLayoutGlyph positioned;
				positioned.glyph = item.glyph;
				positioned.cluster = item.byteOffset;
				positioned.xOffset = fixedCell
					? GetFixedCellGlyphOffset(*role.style, face.face->glyph) : kerning;
				positioned.xAdvance = fixedCell
					? fixedAdvance
					: baseAdvance + role.style->tracking + kerning;
				layout.advance += positioned.xAdvance;
				positioned.glyph.metrics = nullptr;
				glyphs.push_back(std::move(positioned));
				previousGlyph = item.resolved.glyphIndex;
			}
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

		struct CollisionSpan
		{
			float top = 0.0f;
			float bottom = 0.0f;
			float left = 0.0f;
			float right = 0.0f;
		};

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
			const float originY = positioned.yOffset + GetGlyphBaselineOffset(
				runtime, positioned.glyph.byteClass);
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
			std::vector<CollisionSpan> previousSpans;
			std::vector<CollisionSpan> currentSpans;
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
		}

		bool BuildLayoutRun(FreeTypeState& state, RuntimeFont& runtime, const char* text,
			size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
		{
			layout = {};
			auto glyphs = std::make_shared<FreeTypeLayoutRun::GlyphStorage>();
			layout.glyphs = glyphs;
			thread_local LayoutInputScratchPool inputScratchPool;
			LayoutInputScratchLease inputLease(inputScratchPool);
			std::vector<LayoutInputGlyph>& input = inputLease.Get();
			input.reserve(std::min<size_t>(length, 65536));
			if (!DecodeLayoutInput(runtime, text, length, input))
				return false;
			glyphs->reserve(input.size());
			const bool shape = allowShaping && runtime.config->shaping;
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
			return true;
		}

	bool LayoutRuntimeRun(RuntimeFont& runtime, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const LayoutCacheLookupKey lookup = {
			runtime.config->layoutHash,
			g_usingWinEncoding,
			allowShaping,
			std::string_view(text, length)
		};
		auto existing = state.layoutCache.find(lookup);
		if (existing != state.layoutCache.end())
		{
			TouchLayoutCacheEntry(state, existing->second);
			layout = existing->second.layout;
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
				g_usingWinEncoding,
				allowShaping,
				std::string(text, length)
			};
			FreeTypeLayoutRun cachedLayout = layout;
			const size_t bytes = sizeof(LayoutCacheEntry) + key.text.size()
				+ layout.glyphs->size() * sizeof(FreeTypeLayoutGlyph);
			state.layoutLru.push_front(key);
			state.layoutCache.emplace(std::move(key),
				LayoutCacheEntry{ std::move(cachedLayout), bytes, state.layoutLru.begin() });
			state.layoutCacheBytes += bytes;
			TrimLayoutCache(state);
		}
		return true;
	}
}
