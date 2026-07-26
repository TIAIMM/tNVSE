#include "font_atlas_stream.h"

#include "font_atlas_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		struct StreamingPage
		{
			std::vector<AtlasSnapshotPlacement> placements;
			std::vector<UInt8> pixels;
			UInt32 cursorX = kDistanceFieldAtlasPadding;
			UInt32 cursorY = kDistanceFieldAtlasPadding;
			UInt32 shelfHeight = 0;
			UInt32 usedWidth = 0;
			UInt32 usedHeight = 0;
		};

		struct StreamingPageFile
		{
			std::wstring temporaryPath;
			std::wstring finalPath;
			AtlasSnapshotHeader header;
			UInt32 placementCount = 0;
			UInt64 pixelBytes = 0;
		};

		struct StreamingRole
		{
			VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
			StreamingPage current;
			std::vector<StreamingPageFile> pages;
			std::unordered_set<UInt64> cacheIds;
			UInt64 totalPlacements = 0;
			UInt64 totalPixelBytes = 0;
		};

		struct StreamingPrewarmState
		{
			UInt32 fontId = 0;
			UInt32 scaleMilli = 0;
			UInt32 codePage = 0;
			UInt64 layoutHash = 0;
			UInt64 maskGenerationHash = 0;
			UInt64 shaderEffectHash = 0;
			AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
			bool enabled = false;
			bool failed = false;
			std::array<StreamingRole, 2> roles;
		};

		std::mutex s_streamMutex;
		std::unordered_map<RuntimeFont*, std::unique_ptr<StreamingPrewarmState>> s_streams;

		UInt64 HashBytes(const void* data, size_t size,
			UInt64 hash = 1469598103934665603ull)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		UInt32 NextPowerOfTwo(UInt32 value)
		{
			UInt32 result = 64;
			while (result < value && result < kAtlasHardLimit)
				result <<= 1;
			return result;
		}

		bool WriteExact(HANDLE file, const void* data, size_t size)
		{
			const UInt8* current = static_cast<const UInt8*>(data);
			while (size)
			{
				const DWORD requested = static_cast<DWORD>(std::min<size_t>(
					size, std::numeric_limits<DWORD>::max()));
				DWORD written = 0;
				if (!WriteFile(file, current, requested, &written, nullptr)
					|| written != requested)
				{
					return false;
				}
				current += written;
				size -= written;
			}
			return true;
		}

		bool BuildBaseKey(const FontConfig& config,
			VectorFontByteClass byteClass, float rasterScale, AtlasCacheKey& key)
		{
			return ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key);
		}

		UInt64 BuildSnapshotHash(const AtlasCacheKey& key, UInt64 maskContentHash,
			const FontConfig& config)
		{
			UInt64 hash = HashBytes(&kAtlasSnapshotVersion,
				sizeof(kAtlasSnapshotVersion));
			hash = HashBytes(&maskContentHash, sizeof(maskContentHash), hash);
			hash = HashBytes(&key.atlasContentHash,
				sizeof(key.atlasContentHash), hash);
			hash = HashBytes(&key.scaleMilli, sizeof(key.scaleMilli), hash);
			hash = HashBytes(&key.pixelMode, sizeof(key.pixelMode), hash);
			hash = HashBytes(&key.renderMode, sizeof(key.renderMode), hash);
			hash = HashBytes(&key.padding, sizeof(key.padding), hash);
			hash = HashBytes(&key.levelZeroOnly,
				sizeof(key.levelZeroOnly), hash);
			hash = HashBytes(&key.byteClass, sizeof(key.byteClass), hash);
			const UInt32 codePage = GetFreeTypeTextCodePage();
			hash = HashBytes(&codePage, sizeof(codePage), hash);
			hash = HashBytes(&kCompleteCodePagePrewarmIdentity,
				sizeof(kCompleteCodePagePrewarmIdentity), hash);
			hash = HashBytes(&kMaximumAtlasMipLevels,
				sizeof(kMaximumAtlasMipLevels), hash);
			hash = HashBytes(&A8ShapeColorContract::kTileUniformColorAbi,
				sizeof(A8ShapeColorContract::kTileUniformColorAbi), hash);
			if (key.renderMode == AtlasRenderMode::ShaderEffects)
			{
				const DistanceFieldMethod method =
					GetConfiguredDistanceFieldMethod();
				const UInt32 revision = DistanceFieldGeneratorRevision(method);
				hash = HashBytes(&method, sizeof(method), hash);
				hash = HashBytes(&revision, sizeof(revision), hash);
			}
			// A snapshot is usable only together with a complete glyph manifest.
			// Couple their ABIs so a manifest format change cannot leave an
			// apparently valid atlas that is restored and then discarded.
			hash = HashBytes(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion), hash);
			hash = HashBytes(
				&kPersistentGlyphManifestCacheIdentityVersion,
				sizeof(kPersistentGlyphManifestCacheIdentityVersion), hash);
			return hash;
		}

		std::wstring BuildSnapshotPath(RuntimeFont& runtime,
			VectorFontByteClass byteClass, float rasterScale, UInt16 pageIndex,
			bool markUsed, UInt64* outSnapshotHash = nullptr,
			UInt64* outMaskContentHash = nullptr)
		{
			std::wstring directory;
			if (!GetFreeTypeFontCacheDirectory(directory))
				return {};
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasCacheKey key;
			if (!BuildBaseKey(config, byteClass, rasterScale, key))
				return {};
			key.pageIndex = pageIndex;
			RuntimeFont* atlasRuntime = GetPrewarmAtlasRuntime(runtime, key);
			if (!atlasRuntime)
				return {};
			const UInt64 maskContentHash = GetRuntimeMaskContentHash(
				*atlasRuntime, byteClass);
			const UInt64 snapshotHash = BuildSnapshotHash(key,
				maskContentHash, GetRuntimeConfig(*atlasRuntime));
			if (outSnapshotHash)
				*outSnapshotHash = snapshotHash;
			if (outMaskContentHash)
				*outMaskContentHash = maskContentHash;
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX_p%u.tnvfatlas",
				static_cast<unsigned long long>(snapshotHash), pageIndex);
			const std::wstring path = directory + L"\\" + fileName;
			if (markUsed)
				MarkFreeTypeFontCacheFileUsed(path);
			return path;
		}

		UInt64 ComputePageContentHash(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const std::vector<UInt8>& pixels)
		{
			struct PageIdentity
			{
				UInt32 width;
				UInt32 height;
				UInt32 padding;
				UInt32 mipLevels;
				UInt8 pixelMode;
				UInt8 renderMode;
				UInt8 storageMode;
				UInt8 levelZeroOnly;
			};
			const PageIdentity identity = { header.width, header.height, header.padding,
				header.mipLevels, header.pixelMode, header.renderMode,
				header.storageMode,
				static_cast<UInt8>(header.mipLevels == 1 ? 1 : 0) };
			UInt64 hash = HashBytes(&identity, sizeof(identity));
			struct Slice
			{
				AtlasRect rect;
				size_t offset;
				size_t bytes;
			};
			std::vector<Slice> slices;
			slices.reserve(placements.size());
			size_t offset = 0;
			for (const AtlasSnapshotPlacement& placement : placements)
			{
				const size_t bytes = static_cast<size_t>(placement.rect.width)
					* placement.rect.height * AtlasBytesPerPixel(
						static_cast<AtlasPixelMode>(header.pixelMode));
				if (offset > pixels.size() || bytes > pixels.size() - offset)
					return 0;
				slices.push_back({ placement.rect, offset, bytes });
				offset += bytes;
			}
			if (offset != pixels.size())
				return 0;
			std::sort(slices.begin(), slices.end(), [](const Slice& left,
				const Slice& right)
			{
				if (left.rect.y != right.rect.y) return left.rect.y < right.rect.y;
				if (left.rect.x != right.rect.x) return left.rect.x < right.rect.x;
				if (left.rect.height != right.rect.height)
					return left.rect.height < right.rect.height;
				return left.rect.width < right.rect.width;
			});
			for (const Slice& slice : slices)
			{
				hash = HashBytes(&slice.rect, sizeof(slice.rect), hash);
				hash = HashBytes(pixels.data() + slice.offset, slice.bytes, hash);
			}
			return hash;
		}

		void ResetPage(StreamingPage& page)
		{
			// Reuse one fixed-capacity page buffer across the whole font. This avoids
			// vector doubling peaks and repeated large heap allocations while keeping
			// the live stream bounded to one A8 (4 MiB) or MTSDF (16 MiB) page per
			// byte role.
			page.placements.clear();
			page.pixels.clear();
			page.cursorX = kDistanceFieldAtlasPadding;
			page.cursorY = kDistanceFieldAtlasPadding;
			page.shelfHeight = 0;
			page.usedWidth = 0;
			page.usedHeight = 0;
		}

		bool WriteCurrentPage(RuntimeFont& runtime, StreamingRole& role,
			float rasterScale)
		{
			StreamingPage& page = role.current;
			if (page.placements.empty())
				return true;
			if (role.pages.size() >= kMaximumAtlasSnapshotPages)
				return false;

			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasCacheKey key;
			if (!BuildBaseKey(config, role.byteClass, rasterScale, key))
				return false;
			key.pageIndex = static_cast<UInt16>(role.pages.size());
			UInt64 snapshotHash = 0;
			UInt64 maskContentHash = 0;
			const std::wstring finalPath = BuildSnapshotPath(runtime, role.byteClass,
				rasterScale, key.pageIndex, true, &snapshotHash, &maskContentHash);
			if (finalPath.empty())
				return false;

			AtlasSnapshotHeader header = {};
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kAtlasSnapshotVersion;
			header.headerSize = sizeof(header);
			header.snapshotHash = snapshotHash;
			header.maskContentHash = maskContentHash;
			header.atlasContentHash = key.atlasContentHash;
			// Streaming pages are an intermediate transaction generation. The
			// finalizer globally repacks them before the manifest is committed.
			header.flags = 0;
			header.scaleMilli = key.scaleMilli;
			header.width = NextPowerOfTwo(std::max<UInt32>(64, page.usedWidth));
			header.height = NextPowerOfTwo(std::max<UInt32>(64, page.usedHeight));
			header.cursorX = key.padding;
			header.cursorY = header.height;
			header.shelfHeight = 0;
			header.padding = key.padding;
			header.mipLevels = 1;
			header.pixelMode = static_cast<UInt8>(key.pixelMode);
			header.renderMode = static_cast<UInt8>(key.renderMode);
			header.byteClass = static_cast<UInt8>(key.byteClass);
			header.pageIndex = key.pageIndex;
			header.pageCount = 1;
			header.placementCount = static_cast<UInt32>(page.placements.size());
			header.pixelBytes = page.pixels.size();
			for (AtlasSnapshotPlacement& placement : page.placements)
			{
				if (!CacheAtlasSnapshotGlyphPlacement(
					placement, header.width, header.height, header.pageIndex))
					return false;
			}
			header.storageMode = static_cast<UInt8>(
				AtlasSnapshotStorage::PlacedLevelZeroRects);
			header.storedPixelBytes = page.pixels.size();
			UInt64 payloadHash = HashBytes(page.placements.data(),
				page.placements.size() * sizeof(page.placements[0]));
			header.payloadChecksum = HashBytes(page.pixels.data(),
				page.pixels.size(), payloadHash);
			header.pageContentHash = ComputePageContentHash(
				header, page.placements, page.pixels);
			if (!header.pageContentHash)
				return false;
			header.checksum = HashBytes(&header,
				offsetof(AtlasSnapshotHeader, checksum));

			const std::wstring temporaryPath = finalPath + L".stream.tmp";
			HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			const bool written = WriteExact(file, &header, sizeof(header))
				&& WriteExact(file, page.placements.data(),
					page.placements.size() * sizeof(page.placements[0]))
				&& WriteExact(file, page.pixels.data(), page.pixels.size());
			CloseHandle(file);
			if (!written)
			{
				DeleteFileW(temporaryPath.c_str());
				return false;
			}

			role.pages.push_back({ temporaryPath, finalPath, header,
				header.placementCount, header.storedPixelBytes });
			role.totalPlacements += header.placementCount;
			role.totalPixelBytes += header.storedPixelBytes;
			ResetPage(page);
			return true;
		}

		bool AppendBitmap(RuntimeFont& runtime, const StreamingPrewarmState& state,
			StreamingRole& role, const std::shared_ptr<const GlyphBitmap>& bitmap,
			float rasterScale)
		{
			AtlasCacheKey key;
			if (!BuildBaseKey(GetRuntimeConfig(runtime), role.byteClass,
				rasterScale, key))
			{
				return false;
			}
			const bool shaderEffects =
				state.renderMode == AtlasRenderMode::ShaderEffects;
			if (!bitmap || !bitmap->cacheId || bitmap->width <= 0 || bitmap->height <= 0
				|| (shaderEffects
					? bitmap->maskType != GlyphMaskType::DistanceField
					: bitmap->maskType == GlyphMaskType::DistanceField))
				return true;
			if (shaderEffects
				&& bitmap->distanceFieldMethod != GetConfiguredDistanceFieldMethod())
				return false;
			const size_t requiredBytes = ExpectedGlyphBitmapBytes(*bitmap);
			if (bitmap->alpha.size() < requiredBytes)
				return false;
			if (role.cacheIds.find(bitmap->cacheId) != role.cacheIds.end())
				return true;
			const size_t maximumPageBytes =
				static_cast<size_t>(kMaximumMtsdfPrewarmAtlasSize)
				* kMaximumMtsdfPrewarmAtlasSize
				* AtlasBytesPerPixel(key.pixelMode);
			if (role.current.pixels.capacity() < maximumPageBytes)
				role.current.pixels.reserve(maximumPageBytes);

			const UInt32 maximum = std::min(
				std::min(GetMaximumAtlasSize(), kAtlasHardLimit),
				kMaximumMtsdfPrewarmAtlasSize);
			const UInt32 width = static_cast<UInt32>(bitmap->width);
			const UInt32 height = static_cast<UInt32>(bitmap->height);
			if (maximum < 64 || width + kDistanceFieldAtlasPadding * 2 > maximum
				|| height + kDistanceFieldAtlasPadding * 2 > maximum)
				return false;

			for (UInt32 attempt = 0; attempt < 2; ++attempt)
			{
				StreamingPage& page = role.current;
				UInt32 x = page.cursorX;
				UInt32 y = page.cursorY;
				UInt32 shelfHeight = page.shelfHeight;
				if (x + width + kDistanceFieldAtlasPadding > maximum)
				{
					x = kDistanceFieldAtlasPadding;
					y += shelfHeight;
					shelfHeight = 0;
				}
				if (y + height + kDistanceFieldAtlasPadding > maximum)
				{
					if (page.placements.empty()
						|| !WriteCurrentPage(runtime, role, rasterScale))
						return false;
					continue;
				}

				AtlasSnapshotPlacement placement = {};
				placement.cacheId = bitmap->cacheId;
				placement.rect = { x, y, width, height };
				placement.left = bitmap->left;
				placement.top = bitmap->top;
				placement.effectiveWidth = bitmap->effectiveWidth;
				placement.effectiveHeight = bitmap->effectiveHeight;
				placement.strokeWidth26Dot6 = bitmap->strokeWidth26Dot6;
				placement.atlasRgb = bitmap->atlasRgb;
				placement.bakedRgba = bitmap->bakedRgba;
				placement.maskType = static_cast<UInt8>(bitmap->maskType);
				placement.sdfSpread = bitmap->sdfSpread;
				placement.colorBaked = bitmap->colorBaked ? 1 : 0;
				placement.bakedLayer = bitmap->bakedLayer;
				page.placements.push_back(placement);
				page.pixels.insert(page.pixels.end(), bitmap->alpha.begin(),
					bitmap->alpha.begin() + requiredBytes);
				page.cursorX = x + width + kDistanceFieldAtlasPadding * 2;
				page.cursorY = y;
				page.shelfHeight = std::max(shelfHeight,
					height + kDistanceFieldAtlasPadding * 2);
				page.usedWidth = std::max(page.usedWidth,
					x + width + kDistanceFieldAtlasPadding);
				page.usedHeight = std::max(page.usedHeight,
					y + height + kDistanceFieldAtlasPadding);
				role.cacheIds.insert(bitmap->cacheId);
				return true;
			}
			return false;
		}

		bool StreamIdentityMatches(const StreamingPrewarmState& state,
			const FontConfig& config, UInt32 scaleMilli)
		{
			return state.fontId == config.fontId && state.scaleMilli == scaleMilli
				&& state.codePage == GetFreeTypeTextCodePage()
				&& state.layoutHash == config.layoutHash
				&& state.maskGenerationHash == config.maskGenerationHash
				&& state.shaderEffectHash == config.shaderEffectHash;
		}

		void DeleteStateFiles(StreamingPrewarmState& state, bool includePublished)
		{
			for (StreamingRole& role : state.roles)
			{
				for (StreamingPageFile& page : role.pages)
				{
					if (!page.temporaryPath.empty())
						DeleteFileW(page.temporaryPath.c_str());
					if (includePublished && !page.finalPath.empty())
						DeleteFileW(page.finalPath.c_str());
				}
			}
		}

		StreamingPrewarmState* GetOrCreateState(RuntimeFont& runtime,
			float rasterScale)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			const UInt32 scaleMilli = static_cast<UInt32>(std::lround(
				rasterScale * 1000.0f));
			auto found = s_streams.find(&runtime);
			if (found != s_streams.end()
				&& !StreamIdentityMatches(*found->second, config, scaleMilli))
			{
				DeleteStateFiles(*found->second, false);
				s_streams.erase(found);
				found = s_streams.end();
			}
			if (found != s_streams.end())
				return found->second.get();

			auto state = std::make_unique<StreamingPrewarmState>();
			state->fontId = config.fontId;
			state->scaleMilli = scaleMilli;
			state->codePage = GetFreeTypeTextCodePage();
			state->layoutHash = config.layoutHash;
			state->maskGenerationHash = config.maskGenerationHash;
			state->shaderEffectHash = config.shaderEffectHash;
			AtlasCacheKey key;
			state->enabled = BuildBaseKey(config,
				VectorFontByteClass::SingleByte, rasterScale, key);
			if (state->enabled)
				state->renderMode = key.renderMode;
			state->roles[0].byteClass = VectorFontByteClass::SingleByte;
			state->roles[1].byteClass = VectorFontByteClass::DoubleByte;
			StreamingPrewarmState* result = state.get();
			s_streams.emplace(&runtime, std::move(state));
			return result;
		}

		bool PatchAndPublishRole(RuntimeFont& runtime, StreamingRole& role,
			float rasterScale)
		{
			if (role.pages.empty() || role.pages.size() > kMaximumAtlasSnapshotPages)
				return false;
			const UInt16 pageCount = static_cast<UInt16>(role.pages.size());
			for (UInt16 pageIndex = 0; pageIndex < pageCount; ++pageIndex)
			{
				StreamingPageFile& page = role.pages[pageIndex];
				page.header.pageIndex = pageIndex;
				page.header.pageCount = pageCount;
				page.header.checksum = HashBytes(&page.header,
					offsetof(AtlasSnapshotHeader, checksum));
				HANDLE file = CreateFileW(page.temporaryPath.c_str(), GENERIC_WRITE,
					0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					return false;
				LARGE_INTEGER beginning = {};
				const bool patched = SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)
					&& WriteExact(file, &page.header, sizeof(page.header));
				CloseHandle(file);
				if (!patched || !MoveFileExW(page.temporaryPath.c_str(),
					page.finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
				{
					return false;
				}
				page.temporaryPath.clear();
			}

			for (UInt16 pageIndex = pageCount;
				pageIndex < kMaximumAtlasSnapshotPages; ++pageIndex)
			{
				const std::wstring stale = BuildSnapshotPath(runtime, role.byteClass,
					rasterScale, pageIndex, false);
				if (!stale.empty())
				{
					DeleteFileW(stale.c_str());
					const std::wstring temporary = stale + L".stream.tmp";
					DeleteFileW(temporary.c_str());
				}
			}
			return true;
		}
	}

	bool AppendStreamingPrewarmAtlas(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& results,
		float rasterScale)
	{
		std::lock_guard<std::mutex> lock(s_streamMutex);
		StreamingPrewarmState* state = GetOrCreateState(runtime, rasterScale);
		if (!state || state->failed)
			return false;
		if (!state->enabled)
			return true;
		if (results.size() < requests.size())
		{
			state->failed = true;
			return false;
		}

		try
		{
			std::array<std::vector<std::shared_ptr<const GlyphBitmap>>, 2> grouped;
			for (size_t index = 0; index < requests.size(); ++index)
			{
				const GlyphBitmapRequest& request = requests[index];
				const auto& bitmap = results[index];
				const bool shaderEffects =
					state->renderMode == AtlasRenderMode::ShaderEffects;
				if (!request.glyph
					|| (shaderEffects
						? request.maskType != GlyphMaskType::DistanceField
						: request.maskType == GlyphMaskType::DistanceField)
					|| !bitmap || bitmap->alpha.empty())
					continue;
				grouped[static_cast<size_t>(request.glyph->byteClass)].push_back(bitmap);
			}
			for (size_t roleIndex = 0; roleIndex < grouped.size(); ++roleIndex)
			{
				auto& bitmaps = grouped[roleIndex];
				std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& left,
					const auto& right)
				{
					if (left->height != right->height) return left->height > right->height;
					if (left->width != right->width) return left->width > right->width;
					return left->cacheId < right->cacheId;
				});
				for (const auto& bitmap : bitmaps)
				{
					if (!AppendBitmap(runtime, *state,
						state->roles[roleIndex], bitmap, rasterScale))
					{
						state->failed = true;
						return false;
					}
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			state->failed = true;
			size_t retainedBytes = 0;
			size_t completedPages = 0;
			for (const StreamingRole& role : state->roles)
			{
				retainedBytes += role.current.pixels.size();
				completedPages += role.pages.size();
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: streamed prewarm allocation failed font=%u scale=%.3f pageLimit=%u completedPages=%llu retainedMiB=%.2f",
				state->fontId, rasterScale, kMaximumMtsdfPrewarmAtlasSize,
				static_cast<unsigned long long>(completedPages),
				retainedBytes / (1024.0 * 1024.0));
			return false;
		}
		return true;
	}

	bool FinalizeStreamingPrewarmAtlas(RuntimeFont& runtime, float rasterScale)
	{
		const ULONGLONG finalizeStarted = GetTickCount64();
		std::unique_ptr<StreamingPrewarmState> completed;
		{
			std::lock_guard<std::mutex> lock(s_streamMutex);
			StreamingPrewarmState* state = GetOrCreateState(runtime, rasterScale);
			if (!state || state->failed)
				return false;
			if (!state->enabled)
			{
				s_streams.erase(&runtime);
				return true;
			}
			const size_t roleCount = IsDbcsCodePage(state->codePage) ? 2 : 1;
			for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
			{
				StreamingRole& role = state->roles[roleIndex];
				if (IsPrewarmAtlasAlias(GetRuntimeConfig(runtime),
					role.byteClass))
				{
					continue;
				}
				if (!WriteCurrentPage(runtime, role, rasterScale)
					|| !PatchAndPublishRole(runtime, role, rasterScale))
				{
					state->failed = true;
					return false;
				}
			}
			completed = std::move(s_streams[&runtime]);
			s_streams.erase(&runtime);
		}
		for (StreamingRole& role : completed->roles)
		{
			// The reusable raster buffers are no longer needed. Release them before
			// snapshot validation and D3D9 allocation begin.
			std::vector<AtlasSnapshotPlacement>().swap(role.current.placements);
			std::vector<UInt8>().swap(role.current.pixels);
		}

		// Publishing a streamed role replaces the content-addressed snapshot files.
		// An equivalent font ID can already have the same atlas profile resident, but
		// its CompactAtlasSnapshot still describes the files from before this commit.
		// Invalidate the reuse marker before staging so the snapshot loader must
		// read the just-published generation instead of accepting stale backing
		// metadata. The resources themselves are replaced atomically by that loader.
		{
			AtlasState& atlasState = State();
			std::lock_guard<std::mutex> lock(atlasState.atlasMutex);
			const FontConfig& config = GetRuntimeConfig(runtime);
			const size_t roleCount = IsDbcsCodePage(completed->codePage) ? 2 : 1;
			UInt32 invalidatedProfiles = 0;
			for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
			{
				const VectorFontByteClass byteClass =
					static_cast<VectorFontByteClass>(roleIndex);
				if (IsPrewarmAtlasAlias(config, byteClass))
					continue;
				AtlasCacheKey key;
				if (!BuildBaseKey(config, byteClass, rasterScale, key))
					continue;
				invalidatedProfiles += static_cast<UInt32>(
					atlasState.completeAtlasProfiles.erase(MakeAtlasProfileKey(key)));
			}
			if (invalidatedProfiles)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed atlas backing replaced font=%u invalidatedResidentProfiles=%u",
					config.fontId, invalidatedProfiles);
			}
		}

		const ULONGLONG publishedAt = GetTickCount64();
		// Preserve the global skyline repack, but stage only source headers and
		// placements in the atlas index. SaveGlyphAtlasSnapshot then reads one
		// bounded source page at a time from disk and writes the globally repacked
		// destination pages. No intermediate streamed generation is uploaded.
		const bool staged = StageGlyphAtlasSnapshotMetadata(runtime, rasterScale);
		const ULONGLONG stagedAt = GetTickCount64();
		const bool repacked = staged
			&& SaveGlyphAtlasSnapshot(runtime, rasterScale);
		const ULONGLONG repackedAt = GetTickCount64();
		bool restored = false;
		if (repacked)
		{
			// The manifest is the transaction commit marker. Do not make the
			// generation restorable until both byte roles have been repacked.
			MarkGlyphManifestComplete(runtime);
			restored = RebuildGlyphAtlasFromSnapshot(runtime, rasterScale)
				&& HasGloballyRepackedGlyphAtlasSnapshot(runtime, rasterScale);
		}
		UInt64 pages = 0;
		UInt64 placements = 0;
		UInt64 bytes = 0;
		for (const StreamingRole& role : completed->roles)
		{
			pages += role.pages.size();
			placements += role.totalPlacements;
			bytes += role.totalPixelBytes;
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: streamed prewarm finalized font=%u scale=%.3f pages=%llu placements=%llu rawBytes=%llu publishMs=%llu metadataStageMs=%llu repackMs=%llu restoreMs=%llu stage=%s repack=%s restore=%s",
			completed->fontId, rasterScale,
			static_cast<unsigned long long>(pages),
			static_cast<unsigned long long>(placements),
			static_cast<unsigned long long>(bytes),
			static_cast<unsigned long long>(publishedAt - finalizeStarted),
			static_cast<unsigned long long>(stagedAt - publishedAt),
			static_cast<unsigned long long>(repackedAt - stagedAt),
			static_cast<unsigned long long>(GetTickCount64() - repackedAt),
			staged ? "complete" : "failed",
			repacked ? "complete" : "failed",
			restored ? (g_bEnableFreeTypeDefaultPoolAtlas
				? "repacked-default-pool" : "repacked-managed") : "failed");
		return restored;
	}

	void CancelStreamingPrewarmAtlas(RuntimeFont& runtime)
	{
		std::lock_guard<std::mutex> lock(s_streamMutex);
		const auto found = s_streams.find(&runtime);
		if (found == s_streams.end())
			return;
		DeleteStateFiles(*found->second, false);
		s_streams.erase(found);
	}
}
