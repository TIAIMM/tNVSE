#include "font_vector_internal.h"

#include "encoding.h"
#include "load_config.h"

#include <array>
#include <deque>
#include <unordered_set>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kCommonDoubleByteLimit = 7000;
		constexpr UInt32 kMaximumCandidatesPerFrame = 256;
		constexpr UInt32 kMaximumGlyphsPerFrame = 8;

		struct PrewarmJob
		{
			UInt32 fontId = 0;
			UInt64 styleHash = 0;
			UInt32 codePage = 0;
			FontPrewarmMode mode = FontPrewarmMode::None;
			UInt32 singleByte = 0x20;
			UInt32 leadByte = 0x81;
			UInt32 leadByteEnd = 0xFE;
			UInt32 trailByte = 0x40;
			UInt32 trailByteStart = 0x40;
			UInt32 validDoubleByteCount = 0;
			UInt32 rasterizedGlyphCount = 0;
			bool scanningDoubleByte = false;
		};

		std::deque<PrewarmJob> s_jobs;
		std::unordered_set<UInt64> s_scheduledProfiles;

		UInt64 BuildProfileKey(const FontConfig& config)
		{
			return config.styleHash ^ (static_cast<UInt64>(config.fontId) << 32);
		}

		void ConfigureCommonRange(PrewarmJob& job)
		{
			if (job.mode != FontPrewarmMode::Common)
				return;
			switch (job.codePage)
			{
			case 936:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xF7;
				job.trailByte = job.trailByteStart = 0xA1;
				break;
			case 950:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xC6;
				break;
			case 932:
				job.leadByte = 0x81;
				job.leadByteEnd = 0xEA;
				break;
			case 949:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xC8;
				job.trailByte = job.trailByteStart = 0xA1;
				break;
			default:
				job.leadByteEnd = 0;
				break;
			}
		}

		bool IsInRange(UInt32 value, UInt32 first, UInt32 last)
		{
			return value >= first && value <= last;
		}

		bool IsDcfgCodePageUnit(UInt32 codePage, UInt32 lead, UInt32 trail)
		{
			const UInt32 encoded = (lead << 8) | trail;
			switch (codePage)
			{
			case 936: // DCFGCF GB2312(true): complete GBK profile.
				if (IsInRange(lead, 0x81, 0xA0)
					&& IsInRange(trail, 0x40, 0xFE) && trail != 0x7F)
					return true;
				if (IsInRange(encoded, 0xA6A1, 0xA6B8)
					|| IsInRange(encoded, 0xA6C1, 0xA6D8)
					|| IsInRange(encoded, 0xA6E0, 0xA6EB)
					|| IsInRange(encoded, 0xA6EE, 0xA6F2)
					|| IsInRange(encoded, 0xA6F4, 0xA6F5)
					|| IsInRange(encoded, 0xA7A1, 0xA7C1)
					|| IsInRange(encoded, 0xA7D1, 0xA7F1)
					|| IsInRange(encoded, 0xA840, 0xA895)
					|| IsInRange(encoded, 0xA8A1, 0xA8BB)
					|| IsInRange(encoded, 0xA8BD, 0xA8BE)
					|| encoded == 0xA8C1
					|| IsInRange(encoded, 0xA8C5, 0xA8E9)
					|| IsInRange(encoded, 0xA940, 0xA957)
					|| IsInRange(encoded, 0xA959, 0xA95A)
					|| encoded == 0xA95C
					|| IsInRange(encoded, 0xA960, 0xA988)
					|| encoded == 0xA996)
					return trail != 0x7F;
				if (IsInRange(lead, 0xAA, 0xFC)
					&& IsInRange(trail, 0x40, 0xA0) && trail != 0x7F)
					return true;
				if (IsInRange(encoded, 0xFE40, 0xFE4F))
					return true;
				if (lead == 0xA1 && IsInRange(trail, 0xA1, 0xFE))
					return true;
				if (lead == 0xA3 && IsInRange(trail, 0xA1, 0xFE))
					return true;
				if (lead == 0xA4 && IsInRange(trail, 0xA1, 0xF3))
					return true;
				if (lead == 0xA5 && IsInRange(trail, 0xA1, 0xF6))
					return true;
				return IsInRange(lead, 0xB0, 0xF7)
					&& IsInRange(trail, 0xA1, 0xFE);

			case 950: // DCFGCF Big5().
				if (IsInRange(lead, 0xA1, 0xA2))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xFE);
				if (lead == 0xA3)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xBF) || trail == 0xE1;
				if (IsInRange(lead, 0xA4, 0xC5)
					|| IsInRange(lead, 0xC9, 0xF9))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xFE);
				return lead == 0xC6 && IsInRange(trail, 0x40, 0x7E);

			case 932: // DCFGCF SJIS().
				if (lead == 0x81)
					return IsInRange(encoded, 0x8140, 0x81AC)
						|| IsInRange(encoded, 0x81B8, 0x81BF)
						|| IsInRange(encoded, 0x81C8, 0x81CE)
						|| IsInRange(encoded, 0x81DA, 0x81E8)
						|| IsInRange(encoded, 0x81F0, 0x81F7)
						|| encoded == 0x81FC;
				if (lead == 0x82)
					return IsInRange(encoded, 0x824F, 0x8258)
						|| IsInRange(encoded, 0x8260, 0x8279)
						|| IsInRange(encoded, 0x8281, 0x829A)
						|| IsInRange(encoded, 0x829F, 0x82F1);
				if (lead == 0x83)
					return IsInRange(encoded, 0x8340, 0x8396)
						|| IsInRange(encoded, 0x839F, 0x83B6)
						|| IsInRange(encoded, 0x83BF, 0x83D6);
				if (lead == 0x84)
					return IsInRange(encoded, 0x8440, 0x8460)
						|| IsInRange(encoded, 0x8470, 0x8491)
						|| IsInRange(encoded, 0x849F, 0x84BE);
				if (lead == 0x87)
					return IsInRange(encoded, 0x8740, 0x875D)
						|| IsInRange(encoded, 0x875F, 0x8775)
						|| encoded == 0x877E
						|| IsInRange(encoded, 0x8780, 0x879C);
				if (lead == 0x88)
					return IsInRange(encoded, 0x889F, 0x88FC);
				if (IsInRange(lead, 0x89, 0x9F)
					|| IsInRange(lead, 0xE0, 0xE9)
					|| IsInRange(lead, 0xFA, 0xFB))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xFC);
				if (lead == 0xEA)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xA4);
				if (lead == 0xED)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xEC)
						|| IsInRange(trail, 0xEF, 0xFC);
				if (lead == 0xEE)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xFC);
				return lead == 0xFC && IsInRange(trail, 0x40, 0x4B);

			case 949: // DCFGCF Korea().
				if (IsInRange(lead, 0x81, 0xC5))
					return IsInRange(trail, 0x41, 0x5A)
						|| IsInRange(trail, 0x60, 0x7A)
						|| IsInRange(trail, 0x81, 0xFE);
				if (lead == 0xC6)
					return IsInRange(trail, 0x41, 0x52)
						|| IsInRange(trail, 0xA1, 0xFE);
				return IsInRange(lead, 0xC7, 0xC8)
					&& IsInRange(trail, 0xA1, 0xFE);
			default:
				return false;
			}
		}

		bool NextEncodedUnit(PrewarmJob& job, std::array<char, 2>& bytes,
			size_t& length)
		{
			while (!job.scanningDoubleByte)
			{
				if (job.singleByte <= 0xFF)
				{
					bytes[0] = static_cast<char>(job.singleByte++);
					length = 1;
					return true;
				}
				job.scanningDoubleByte = true;
			}

			while (job.leadByte <= job.leadByteEnd)
			{
				while (job.trailByte <= 0xFE)
				{
					bytes[0] = static_cast<char>(job.leadByte);
					bytes[1] = static_cast<char>(job.trailByte++);
					UInt32 encoded = 0;
					const bool selected = job.mode == FontPrewarmMode::CodePage
						? IsDcfgCodePageUnit(job.codePage, job.leadByte,
							static_cast<UInt8>(bytes[1]))
						: TryDecodeDoubleByte(bytes.data(), encoded);
					if (selected)
					{
						length = 2;
						return true;
					}
				}
				++job.leadByte;
				job.trailByte = job.trailByteStart;
			}
			return false;
		}

		void AddBitmap(std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			std::unordered_set<UInt64>& unique,
			const std::shared_ptr<const GlyphBitmap>& bitmap)
		{
			if (!bitmap || bitmap->width <= 0 || bitmap->height <= 0
				|| bitmap->alpha.empty() || !unique.insert(bitmap->cacheId).second)
			{
				return;
			}
			bitmaps.push_back(bitmap);
		}

		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u mode=%s glyphs=%u doubleByte=%u status=%s",
				job.fontId,
				job.mode == FontPrewarmMode::CodePage ? "codepage" : "common",
				job.rasterizedGlyphCount, job.validDoubleByteCount, status);
		}
	}

	void QueueFontPrewarm(UInt32 fontId)
	{
		const FontConfig* config = FindConfig(fontId);
		if (!config || config->prewarm == FontPrewarmMode::None)
			return;
		const UInt64 key = BuildProfileKey(*config);
		if (!s_scheduledProfiles.insert(key).second)
			return;
		PrewarmJob job;
		job.fontId = fontId;
		job.styleHash = config->styleHash;
		job.codePage = g_usingWinEncoding;
		job.mode = config->prewarm;
		if (!job.codePage)
			job.leadByteEnd = 0;
		ConfigureCommonRange(job);
		s_jobs.push_back(job);
		gLog.FormattedMessage(
			"tnvse_freetype_font: queued prewarm font=%u mode=%s codePage=%u",
			fontId, config->prewarm == FontPrewarmMode::CodePage
				? "codepage" : "common", g_usingWinEncoding);
	}

	void PumpFontPrewarm()
	{
		if (s_jobs.empty() || !g_bEnableFreeTypeFontRendering)
			return;

		PrewarmJob job = s_jobs.front();
		s_jobs.pop_front();
		const FontConfig* config = FindConfig(job.fontId);
		RuntimeFont* runtime = FindRuntimeFont(job.fontId);
		if (!config || !runtime || config->styleHash != job.styleHash
			|| job.codePage != g_usingWinEncoding)
		{
			FinishJob(job, "cancelled");
			return;
		}

		std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps;
		bitmaps.reserve(kMaximumGlyphsPerFrame * 3);
		std::unordered_set<UInt64> unique;
		UInt32 candidates = 0;
		UInt32 glyphs = 0;
		bool exhausted = false;
		while (candidates < kMaximumCandidatesPerFrame
			&& glyphs < kMaximumGlyphsPerFrame)
		{
			std::array<char, 2> bytes = {};
			size_t length = 0;
			if (!NextEncodedUnit(job, bytes, length))
			{
				exhausted = true;
				break;
			}
			++candidates;
			VectorEncodedGlyph glyph;
			if (!ResolvePrewarmGlyph(*runtime, bytes.data(), length, glyph))
				continue;
			if (length == 2)
			{
				if (job.mode == FontPrewarmMode::Common
					&& job.validDoubleByteCount >= kCommonDoubleByteLimit)
				{
					exhausted = true;
					break;
				}
				++job.validDoubleByteCount;
			}

			AddBitmap(bitmaps, unique, GetGlyphBitmap(
				*runtime, glyph, GlyphMaskType::Fill, 1.0f));
			if (config->glow.enabled)
			{
				AddBitmap(bitmaps, unique, GetGlyphBitmap(
					*runtime, glyph, GlyphMaskType::Glow, 1.0f));
			}
			if (config->outline.enabled)
			{
				AddBitmap(bitmaps, unique, GetGlyphBitmap(
					*runtime, glyph, GlyphMaskType::Outline, 1.0f));
			}
			++glyphs;
			++job.rasterizedGlyphCount;
		}

		if (!bitmaps.empty() && !PrewarmGlyphAtlas(*runtime, bitmaps, 1.0f))
		{
			FinishJob(job, "atlas-full");
			return;
		}
		if (exhausted)
		{
			FinishJob(job, "complete");
			return;
		}
		s_jobs.push_back(job);
	}
}

namespace fonthook
{
	void PumpFreeTypeFontPrewarm()
	{
		vectorfont::PumpFontPrewarm();
	}
}
