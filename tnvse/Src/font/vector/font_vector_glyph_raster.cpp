#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "font_vector_msdfgen.h"
#include "globals.h"

#include <atomic>
#include <thread>

namespace fonthook::vectorfont
{
		void TouchBitmapCacheEntry(FreeTypeState& state, BitmapCacheEntry& entry,
			const BitmapCacheKey& key)
		{
			state.bitmapLru.splice(state.bitmapLru.begin(), state.bitmapLru, entry.lru);
			entry.lru = state.bitmapLru.begin();
		}

		void TrimBitmapCache(FreeTypeState& state)
		{
			const size_t limit = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::GlyphBitmap, GetBitmapCacheLimit());
			while ((GetCpuMemoryUsage(CpuMemoryCategory::GlyphBitmap) > limit
					|| IsCpuMemoryBudgetExceeded())
				&& !state.bitmapLru.empty())
			{
				const BitmapCacheKey key = state.bitmapLru.back();
				auto it = state.bitmapCache.find(key);
				if (it != state.bitmapCache.end())
				{
					state.bitmapCacheBytes -= it->second.bytes;
					state.bitmapCache.erase(it);
				}
				state.bitmapLru.pop_back();
			}
		}

		void RefreshGlyphBitmapCpuMemory(GlyphBitmap& bitmap)
		{
			bitmap.cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
				sizeof(GlyphBitmap) + bitmap.alpha.capacity());
		}

		constexpr size_t GetBitmapCacheEntryCpuBytes()
		{
			return sizeof(BitmapCacheEntry) + 2u * sizeof(BitmapCacheKey)
				+ 4u * sizeof(void*);
		}

	void SetBitmapCacheReducedAfterPrewarm(bool reduced)
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		if (state.bitmapCacheReducedAfterPrewarm == reduced)
			return;

		const size_t previousLimit = GetBitmapCacheLimit();
		const size_t previousBytes = state.bitmapCacheBytes;
		const size_t previousEntries = state.bitmapCache.size();
		state.bitmapCacheReducedAfterPrewarm = reduced;
		TrimBitmapCache(state);
		ReportCpuMemoryBudget(reduced ? "post-prewarm" : "prewarm", true);
		gLog.FormattedMessage(
			"tnvse_freetype_font: bitmap cache phase=%s limitMiB=%.2f previousLimitMiB=%.2f bytesMiB=%.2f previousBytesMiB=%.2f entries=%u evicted=%u",
			reduced ? "post-prewarm" : "prewarm",
			GetBitmapCacheLimit() / (1024.0 * 1024.0),
			previousLimit / (1024.0 * 1024.0),
			state.bitmapCacheBytes / (1024.0 * 1024.0),
			previousBytes / (1024.0 * 1024.0),
			static_cast<UInt32>(state.bitmapCache.size()),
			static_cast<UInt32>(previousEntries - state.bitmapCache.size()));
	}

		bool CopyGrayBitmap(const FT_Bitmap& source, GlyphBitmap& target)
		{
			constexpr int kBitmapGuardPixels = 1;
			const int sourceWidth = static_cast<int>(source.width);
			const int sourceHeight = static_cast<int>(source.rows);
			if (sourceWidth <= 0 || sourceHeight <= 0)
			{
				target.width = 0;
				target.height = 0;
				return true;
			}
			if (!source.buffer)
				return false;
			// FreeType returns the tight bitmap bounds. Keep a transparent texel around
			// every mask so point/linear sampling and fractional UI transforms cannot
			// clip the first or last coverage row or read a neighbouring atlas region.
			target.width = sourceWidth + kBitmapGuardPixels * 2;
			target.height = sourceHeight + kBitmapGuardPixels * 2;
			target.left -= kBitmapGuardPixels;
			target.top += kBitmapGuardPixels;
			target.alpha.assign(static_cast<size_t>(target.width) * target.height, 0);
			const int pitch = source.pitch;
			for (int y = 0; y < sourceHeight; ++y)
			{
				const int sourceY = pitch >= 0 ? y : sourceHeight - 1 - y;
				const UInt8* row = source.buffer + static_cast<ptrdiff_t>(sourceY) * std::abs(pitch);
				UInt8* output = target.alpha.data()
					+ static_cast<size_t>(y + kBitmapGuardPixels) * target.width
					+ kBitmapGuardPixels;
				if (source.pixel_mode == FT_PIXEL_MODE_GRAY)
				{
					if (source.num_grays == 256)
						std::copy(row, row + sourceWidth, output);
					else
					{
						const UInt32 denominator = std::max<UInt32>(1, source.num_grays - 1);
						for (int x = 0; x < sourceWidth; ++x)
							output[x] = static_cast<UInt8>(std::min<UInt32>(255,
								row[x] * 255u / denominator));
					}
				}
				else if (source.pixel_mode == FT_PIXEL_MODE_MONO)
				{
					for (int x = 0; x < sourceWidth; ++x)
						output[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
				}
				else
				{
					return false;
				}
			}
			return true;
		}

		bool BuildMsdfgenTrueSdf(
			FT_GlyphSlot slot,
			UInt8 spread,
			GlyphBitmap& target)
		{
			if (!slot || slot->format != FT_GLYPH_FORMAT_OUTLINE
				|| spread < 2 || spread > 32)
				return false;

			MsdfgenSdfBitmap generated;
			if (!GenerateMsdfgenTrueSdf(slot->outline, spread, generated))
				return false;
			target.width = generated.width;
			target.height = generated.height;
			target.left = generated.left;
			target.top = generated.top;
			target.alpha = std::move(generated.pixels);
			return true;
		}

		std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(FreeTypeState& state,
			RuntimeFont& runtime,
			const ResolvedGlyph& resolved,
			GlyphMaskType maskType,
			float rasterScale, const BitmapCacheKey& key)
		{
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			bitmap->maskType = maskType;
			bitmap->sdfSpread = key.sdfSpread;
			bitmap->strokeWidth26Dot6 = key.strokeWidth26Dot6;
			RuntimeRole& role = *resolved.role;
			if (!ConfigureRuntimeFace(*resolved.runtimeFace, *role.style, rasterScale, true))
				return nullptr;

			const FT_Int32 loadFlags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL
				| FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
			if (FT_Load_Glyph(resolved.runtimeFace->face, resolved.glyphIndex, loadFlags))
				return nullptr;
			FT_GlyphSlot slot = resolved.runtimeFace->face->glyph;
			if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
			{
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}
			if (role.style->embolden > 0.0f && slot->outline.n_points)
			{
				const FT_Pos strength = key.embolden26Dot6;
				FT_Outline_EmboldenXY(&slot->outline, strength, strength);
			}

			if (maskType == GlyphMaskType::Fill)
			{
				if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
					return nullptr;
				bitmap->left = slot->bitmap_left;
				bitmap->top = slot->bitmap_top;
				if (!CopyGrayBitmap(slot->bitmap, *bitmap))
					return nullptr;
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			if (maskType == GlyphMaskType::DistanceField)
			{
				if (key.sdfSpread < 2 || key.sdfSpread > 32)
					return nullptr;
				if (!BuildMsdfgenTrueSdf(slot, key.sdfSpread, *bitmap))
					return nullptr;
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			const EffectStyle& effect = maskType == GlyphMaskType::Glow
				? runtime.config->glow : runtime.config->outline;
			if (!effect.enabled || effect.width <= 0.0f || !slot->outline.n_points)
			{
				RefreshGlyphBitmapCpuMemory(*bitmap);
				return bitmap;
			}

			FT_Glyph strokedGlyph = nullptr;
			FT_Stroker stroker = nullptr;
			if (FT_Get_Glyph(slot, &strokedGlyph)
				|| FT_Stroker_New(state.library, &stroker))
			{
				if (strokedGlyph)
					FT_Done_Glyph(strokedGlyph);
				return nullptr;
			}
			FT_Stroker_Set(stroker, key.strokeWidth26Dot6,
				FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
			const FT_Error strokeError = FT_Glyph_StrokeBorder(
				&strokedGlyph, stroker, false, true);
			FT_Stroker_Done(stroker);
			if (strokeError || !strokedGlyph)
			{
				if (strokedGlyph)
					FT_Done_Glyph(strokedGlyph);
				return nullptr;
			}

			const FT_Error bitmapError = FT_Glyph_To_Bitmap(
				&strokedGlyph, FT_RENDER_MODE_NORMAL, nullptr, true);
			if (bitmapError || !strokedGlyph || strokedGlyph->format != FT_GLYPH_FORMAT_BITMAP)
			{
				if (strokedGlyph)
					FT_Done_Glyph(strokedGlyph);
				return nullptr;
			}
			const FT_BitmapGlyph bitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(strokedGlyph);
			bitmap->left = bitmapGlyph->left;
			bitmap->top = bitmapGlyph->top;
			const bool copied = CopyGrayBitmap(bitmapGlyph->bitmap, *bitmap);
			FT_Done_Glyph(strokedGlyph);
			if (!copied)
				return nullptr;
			RefreshGlyphBitmapCpuMemory(*bitmap);
			return bitmap;
		}

	static float SanitizeBitmapRasterScale(float rasterScale)
	{
		return std::isfinite(rasterScale) && rasterScale >= 0.1f
			&& rasterScale <= 10.0f ? rasterScale : 1.0f;
	}

	static bool ResolveBitmapCacheKey(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMaskType maskType, float safeScale,
		UInt32 sdfSpread, ResolvedGlyph& resolved, BitmapCacheKey& key)
	{
		if (!ResolveVectorGlyph(runtime, glyph, resolved) || !resolved.role
			|| !resolved.role->style || !resolved.runtimeFace
			|| !resolved.runtimeFace->face || !resolved.runtimeFace->file)
		{
			return false;
		}
		const ByteStyle& style = *resolved.role->style;
		const int effectiveWidth = std::clamp(static_cast<int>(std::lround(
			style.pixelSize * style.scaleX * safeScale)), 1, 65535);
		const int effectiveHeight = std::clamp(static_cast<int>(std::lround(
			style.pixelSize * style.scaleY * safeScale)), 1, 65535);
		const EffectStyle* effect = maskType == GlyphMaskType::Glow
			? &runtime.config->glow
			: maskType == GlyphMaskType::Outline ? &runtime.config->outline : nullptr;
		const SInt32 strokeWidth = effect && effect->enabled
			? static_cast<SInt32>(std::lround(effect->width * safeScale * 64.0f)) : 0;
		const SInt32 embolden = static_cast<SInt32>(std::lround(
			style.embolden * safeScale * 64.0f));
		const UInt8 resolvedSdfSpread = maskType == GlyphMaskType::DistanceField
			&& sdfSpread >= 2 && sdfSpread <= 32
			? static_cast<UInt8>(sdfSpread) : 0;
		if (maskType == GlyphMaskType::DistanceField && !resolvedSdfSpread)
			return false;
		const float slant = std::tan(style.slantDegrees
			* 3.14159265358979323846f / 180.0f);
		key = {
			resolved.runtimeFace->file->contentHash,
			static_cast<SInt32>(resolved.runtimeFace->face->face_index),
			resolved.glyphIndex,
			GetFreeTypeTextCodePage(),
			static_cast<UInt16>(effectiveWidth),
			static_cast<UInt16>(effectiveHeight),
			embolden,
			strokeWidth,
			static_cast<SInt32>(std::lround(slant * kFixedScale)),
			resolvedSdfSpread,
			static_cast<UInt8>(maskType)
		};
		return true;
	}

	void ResolveGlyphBitmapCacheIds(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<UInt64>& cacheIds)
	{
		cacheIds.assign(requests.size(), 0);
		if (requests.empty())
			return;
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		for (size_t index = 0; index < requests.size(); ++index)
		{
			const GlyphBitmapRequest& request = requests[index];
			if (!request.glyph)
				continue;
			ResolvedGlyph resolved;
			BitmapCacheKey key;
			if (ResolveBitmapCacheKey(runtime, *request.glyph, request.maskType,
				safeScale, request.sdfSpread, resolved, key))
			{
				cacheIds[index] = HashBitmapKey(key);
			}
		}
	}

	static std::shared_ptr<const GlyphBitmap> FindCachedGlyphBitmapLocked(
		FreeTypeState& state, RuntimeFont& runtime, const ResolvedGlyph& resolved,
		const BitmapCacheKey& key, PersistentBitmapProfile*& persistentProfile)
	{
		persistentProfile = nullptr;
		auto existing = state.bitmapCache.find(key);
		if (existing != state.bitmapCache.end())
		{
			TouchBitmapCacheEntry(state, existing->second, key);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapMemoryHit);
			if (existing->second.sourceFontId != runtime.config->fontId)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapCrossFontHit);
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedCrossFontBitmapShare)
				{
					state.loggedCrossFontBitmapShare = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first cross-font bitmap cache hit sourceFont=%u targetFont=%u path=%ls face=%d glyph=%u size=%ux%u mask=%u",
						existing->second.sourceFontId, runtime.config->fontId,
						resolved.runtimeFace->file->path.c_str(), key.fontFaceIndex,
						key.glyphIndex, key.effectiveWidth, key.effectiveHeight,
						key.maskType);
				}
			}
			return existing->second.bitmap;
		}

		const PersistentBitmapProfileKey persistentKey =
			MakePersistentBitmapProfileKey(key,
				resolved.runtimeFace->file->contentHash);
		persistentProfile =
			GetPersistentBitmapProfile(persistentKey,
				resolved.runtimeFace->file->path, runtime.config->fontId,
				static_cast<UInt32>(std::max<FT_Long>(1,
					resolved.runtimeFace->face->num_glyphs)));
		if (persistentProfile)
		{
			std::shared_ptr<GlyphBitmap> diskBitmap =
				LoadPersistentGlyphBitmap(*persistentProfile, key);
			if (diskBitmap)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskHit);
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskReadBytes,
					diskBitmap->alpha.size());
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedPersistentBitmapHit)
				{
					state.loggedPersistentBitmapHit = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first persistent bitmap cache hit path=%ls font=%u glyph=%u size=%ux%u mask=%u bytes=%u records=%u",
						persistentProfile->path.c_str(), runtime.config->fontId,
						key.glyphIndex, key.effectiveWidth, key.effectiveHeight,
						key.maskType, static_cast<UInt32>(diskBitmap->alpha.size()),
						persistentProfile->recordCount);
				}
				const size_t bytes = sizeof(GlyphBitmap) + diskBitmap->alpha.capacity();
				state.bitmapLru.push_front(key);
				const auto [inserted, success] = state.bitmapCache.emplace(key,
					BitmapCacheEntry{
					diskBitmap, bytes, state.bitmapLru.begin(), runtime.config->fontId });
				if (!success)
				{
					state.bitmapLru.pop_front();
					return inserted->second.bitmap;
				}
				inserted->second.cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
					GetBitmapCacheEntryCpuBytes());
				state.bitmapCacheBytes += bytes;
				TrimBitmapCache(state);
				return diskBitmap;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskMiss);
		}
		return nullptr;
	}

	static void InsertGlyphBitmapCacheLocked(FreeTypeState& state,
		RuntimeFont& runtime, const BitmapCacheKey& key,
		const std::shared_ptr<GlyphBitmap>& bitmap)
	{
		if (!bitmap)
			return;
		const size_t bytes = sizeof(GlyphBitmap) + bitmap->alpha.capacity();
		state.bitmapLru.push_front(key);
		const auto [inserted, success] = state.bitmapCache.emplace(key,
			BitmapCacheEntry{ bitmap, bytes, state.bitmapLru.begin(),
				runtime.config->fontId });
		if (!success)
		{
			state.bitmapLru.pop_front();
			return;
		}
		inserted->second.cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
			GetBitmapCacheEntryCpuBytes());
		state.bitmapCacheBytes += bytes;
	}

	void TrimFreeTypeCpuCachesForTotalBudget()
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		TrimBitmapCache(state);
	}

	static std::shared_ptr<const GlyphBitmap> GetGlyphBitmapLocked(
		FreeTypeState& state, RuntimeFont& runtime,
		const ResolvedGlyph& resolved, GlyphMaskType maskType, float safeScale,
		const BitmapCacheKey& key)
	{
		PersistentBitmapProfile* persistentProfile = nullptr;
		if (std::shared_ptr<const GlyphBitmap> cached = FindCachedGlyphBitmapLocked(
			state, runtime, resolved, key, persistentProfile))
		{
			return cached;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized);
		std::shared_ptr<GlyphBitmap> bitmap =
			BuildGlyphBitmap(state, runtime, resolved, maskType, safeScale, key);
		if (!bitmap)
			return nullptr;
		if (persistentProfile
			&& StorePersistentGlyphBitmap(*persistentProfile, key, *bitmap))
		{
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWrite);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWriteBytes,
				bitmap->alpha.size());
		}
		InsertGlyphBitmapCacheLocked(state, runtime, key, bitmap);
		TrimBitmapCache(state);
		return bitmap;
	}

	struct BitmapBatchDedupeSlot
	{
		BitmapCacheKey key;
		size_t resultIndex = 0;
		UInt32 generation = 0;
	};

	struct BitmapBatchDedupeScratch
	{
		std::vector<BitmapBatchDedupeSlot> slots;
		UInt32 generation = 0;

		void Prepare(size_t requestCount)
		{
			size_t required = 8;
			const size_t target = std::max<size_t>(8, requestCount * 2);
			while (required < target)
				required <<= 1;
			if (slots.size() < required)
				slots.resize(required);
			if (++generation == 0)
			{
				for (BitmapBatchDedupeSlot& slot : slots)
					slot.generation = 0;
				generation = 1;
			}
		}
	};

	void GetGlyphBitmaps(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& results)
	{
		results.assign(requests.size(), nullptr);
		if (requests.empty())
			return;

		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		thread_local BitmapBatchDedupeScratch scratch;
		scratch.Prepare(requests.size());
		const size_t slotMask = scratch.slots.size() - 1;
		UInt64 duplicateCount = 0;
		for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
		{
			const GlyphBitmapRequest& request = requests[requestIndex];
			if (!request.glyph)
				continue;
			ResolvedGlyph resolved;
			BitmapCacheKey key;
			if (!ResolveBitmapCacheKey(runtime, *request.glyph, request.maskType,
				safeScale, request.sdfSpread, resolved, key))
			{
				continue;
			}

			size_t slotIndex = BitmapCacheKeyHash{}(key) & slotMask;
			for (;;)
			{
				BitmapBatchDedupeSlot& slot = scratch.slots[slotIndex];
				if (slot.generation != scratch.generation)
				{
					slot.generation = scratch.generation;
					slot.key = key;
					slot.resultIndex = requestIndex;
					results[requestIndex] = GetGlyphBitmapLocked(state, runtime,
						resolved, request.maskType, safeScale, key);
					break;
				}
				if (slot.key == key)
				{
					++duplicateCount;
					results[requestIndex] = results[slot.resultIndex];
					break;
				}
				slotIndex = (slotIndex + 1) & slotMask;
			}
		}
		RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchRequest,
			static_cast<UInt64>(requests.size()));
		if (duplicateCount)
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchDedupe, duplicateCount);
	}

	struct PrewarmBitmapWorkItem
	{
		size_t requestIndex = 0;
		ResolvedGlyph resolved;
		BitmapCacheKey key;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		PersistentBitmapProfile* persistentProfile = nullptr;
		std::shared_ptr<GlyphBitmap> bitmap;
	};

	struct PrewarmWorkerFace
	{
		std::shared_ptr<MappedFontFile> file;
		SInt32 faceIndex = 0;
		RuntimeFace runtimeFace;
	};

	static RuntimeFace* GetPrewarmWorkerFace(FT_Library library,
		std::vector<PrewarmWorkerFace>& faces,
		const std::shared_ptr<MappedFontFile>& file, SInt32 faceIndex)
	{
		for (PrewarmWorkerFace& face : faces)
		{
			if (face.file == file && face.faceIndex == faceIndex)
				return &face.runtimeFace;
		}
		if (!library || !file || !file->data || file->size <= 0)
			return nullptr;
		PrewarmWorkerFace workerFace;
		workerFace.file = file;
		workerFace.faceIndex = faceIndex;
		workerFace.runtimeFace.file = file;
		if (FT_New_Memory_Face(library, file->data, file->size, faceIndex,
			&workerFace.runtimeFace.face))
		{
			return nullptr;
		}
		faces.push_back(std::move(workerFace));
		return &faces.back().runtimeFace;
	}

	static UInt32 ResolvePrewarmWorkerCount(size_t workCount)
	{
		if (workCount < 64)
			return 1;
		UInt32 processors = std::thread::hardware_concurrency();
		if (!processors)
		{
			SYSTEM_INFO info = {};
			GetSystemInfo(&info);
			processors = std::max<DWORD>(1, info.dwNumberOfProcessors);
		}
		// Leave one logical processor for the progress window and system services.
		// The x86 process also caps worker-local FreeType heaps at a predictable level.
		const UInt32 workers = processors > 2 ? processors - 1 : processors;
		return static_cast<UInt32>(std::min<size_t>(workCount,
			std::clamp<UInt32>(workers, 1, 12)));
	}

	void GetPrewarmGlyphBitmaps(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& results)
	{
		if (requests.size() < 64)
		{
			GetGlyphBitmaps(runtime, requests, rasterScale, results);
			return;
		}
		results.assign(requests.size(), nullptr);
		const float safeScale = SanitizeBitmapRasterScale(rasterScale);
		FreeTypeState& state = State();
		std::vector<PrewarmBitmapWorkItem> workItems;
		workItems.reserve(requests.size());
		std::vector<std::pair<size_t, size_t>> duplicates;
		duplicates.reserve(requests.size() / 8);
		UInt64 duplicateCount = 0;
		{
			std::lock_guard<std::recursive_mutex> lock(state.mutex);
			thread_local BitmapBatchDedupeScratch scratch;
			scratch.Prepare(requests.size());
			const size_t slotMask = scratch.slots.size() - 1;
			for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
			{
				const GlyphBitmapRequest& request = requests[requestIndex];
				if (!request.glyph)
					continue;
				ResolvedGlyph resolved;
				BitmapCacheKey key;
				if (!ResolveBitmapCacheKey(runtime, *request.glyph,
					request.maskType, safeScale, request.sdfSpread, resolved, key))
				{
					continue;
				}
				size_t slotIndex = BitmapCacheKeyHash{}(key) & slotMask;
				for (;;)
				{
					BitmapBatchDedupeSlot& slot = scratch.slots[slotIndex];
					if (slot.generation != scratch.generation)
					{
						slot.generation = scratch.generation;
						slot.key = key;
						slot.resultIndex = requestIndex;
						PersistentBitmapProfile* persistentProfile = nullptr;
						if (std::shared_ptr<const GlyphBitmap> cached =
							FindCachedGlyphBitmapLocked(state, runtime, resolved,
								key, persistentProfile))
						{
							results[requestIndex] = std::move(cached);
						}
						else
						{
							PrewarmBitmapWorkItem item;
							item.requestIndex = requestIndex;
							item.resolved = resolved;
							item.key = key;
							item.maskType = request.maskType;
							item.persistentProfile = persistentProfile;
							workItems.push_back(std::move(item));
						}
						break;
					}
					if (slot.key == key)
					{
						++duplicateCount;
						duplicates.push_back({ requestIndex, slot.resultIndex });
						break;
					}
					slotIndex = (slotIndex + 1) & slotMask;
				}
			}
		}

		if (!workItems.empty())
		{
			const UInt32 workerCount = ResolvePrewarmWorkerCount(workItems.size());
			static bool loggedWorkers = false;
			if (!loggedWorkers)
			{
				loggedWorkers = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: parallel prewarm raster workers=%u batchMisses=%u",
					workerCount, static_cast<UInt32>(workItems.size()));
			}
			std::atomic<size_t> nextWork{ 0 };
			auto worker = [&]()
			{
				FreeTypeState workerState;
				if (FT_Init_FreeType(&workerState.library))
					return;
				{
					std::vector<PrewarmWorkerFace> faces;
					faces.reserve(8);
					for (;;)
					{
						const size_t index = nextWork.fetch_add(1,
							std::memory_order_relaxed);
						if (index >= workItems.size())
							break;
						PrewarmBitmapWorkItem& item = workItems[index];
						RuntimeFace* face = GetPrewarmWorkerFace(workerState.library,
							faces, item.resolved.runtimeFace->file,
							item.key.fontFaceIndex);
						if (!face)
							continue;
						ResolvedGlyph workerResolved = item.resolved;
						workerResolved.runtimeFace = face;
						item.bitmap = BuildGlyphBitmap(workerState, runtime,
							workerResolved, item.maskType, safeScale, item.key);
					}
				}
				FT_Done_FreeType(workerState.library);
				workerState.library = nullptr;
			};
			std::vector<std::thread> workers;
			workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
			for (UInt32 index = 1; index < workerCount; ++index)
				workers.emplace_back(worker);
			worker();
			for (std::thread& thread : workers)
				thread.join();

			std::lock_guard<std::recursive_mutex> lock(state.mutex);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized,
				static_cast<UInt64>(workItems.size()));
			// Worker initialization or a cloned face can fail independently. Preserve
			// the runtime path's behavior by retrying only those entries on the main face.
			for (PrewarmBitmapWorkItem& item : workItems)
			{
				if (!item.bitmap)
				{
					item.bitmap = BuildGlyphBitmap(state, runtime, item.resolved,
						item.maskType, safeScale, item.key);
				}
			}

			std::unordered_map<PersistentBitmapProfile*,
				std::vector<PersistentBitmapStoreRequest>> stores;
			for (PrewarmBitmapWorkItem& item : workItems)
			{
				if (item.bitmap && item.persistentProfile)
					stores[item.persistentProfile].push_back(
						{ &item.key, item.bitmap.get() });
			}
			for (auto& pair : stores)
			{
				UInt64 storedBytes = 0;
				const UInt32 stored = StorePersistentGlyphBitmaps(
					*pair.first, pair.second, storedBytes);
				if (stored)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWrite, stored);
					RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWriteBytes,
						storedBytes);
				}
			}
			for (PrewarmBitmapWorkItem& item : workItems)
			{
				if (!item.bitmap)
					continue;
				auto existing = state.bitmapCache.find(item.key);
				if (existing != state.bitmapCache.end())
				{
					TouchBitmapCacheEntry(state, existing->second, item.key);
					results[item.requestIndex] = existing->second.bitmap;
					continue;
				}
				InsertGlyphBitmapCacheLocked(state, runtime, item.key, item.bitmap);
				results[item.requestIndex] = item.bitmap;
			}
			TrimBitmapCache(state);
		}

		for (const auto& duplicate : duplicates)
			results[duplicate.first] = results[duplicate.second];
		RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchRequest,
			static_cast<UInt64>(requests.size()));
		if (duplicateCount)
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapBatchDedupe, duplicateCount);
	}
}
