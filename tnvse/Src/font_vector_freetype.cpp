#include "font_vector_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include <hb-ft.h>
#include <hb.h>

#include <tesselator.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <winioctl.h>

namespace fonthook::vectorfont
{
	bool DecodeEncodedGlyphIdentity(RuntimeFont& runtime, const char* text,
		VectorEncodedGlyph& glyph);

	namespace
	{
		constexpr size_t kMeshCacheLimit = 8u * 1024u * 1024u;
		constexpr FT_Int32 kGlyphLoadFlags =
			FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
		constexpr float kFixedScale = 65536.0f;

		size_t GetBitmapCacheLimit()
		{
			return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u / 4u;
		}

		size_t GetLayoutCacheLimit()
		{
			return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u / 8u;
		}

		struct MappedFontFile
		{
			std::wstring path;
			HANDLE file = INVALID_HANDLE_VALUE;
			HANDLE mapping = nullptr;
			const FT_Byte* data = nullptr;
			FT_Long size = 0;
			UInt64 contentHash = 0;

			~MappedFontFile()
			{
				if (data)
					UnmapViewOfFile(data);
				if (mapping)
					CloseHandle(mapping);
				if (file != INVALID_HANDLE_VALUE)
					CloseHandle(file);
			}
		};

		struct RuntimeFace
		{
			std::shared_ptr<MappedFontFile> file;
			FT_Face face = nullptr;
			hb_font_t* hbFont = nullptr;
			bool configured = false;
			bool configuredRaster = false;
			FT_UInt configuredWidth = 0;
			FT_UInt configuredHeight = 0;

			RuntimeFace() = default;
			RuntimeFace(const RuntimeFace&) = delete;
			RuntimeFace& operator=(const RuntimeFace&) = delete;
			RuntimeFace(RuntimeFace&& other) noexcept
				: file(std::move(other.file)), face(other.face), hbFont(other.hbFont),
				configured(other.configured), configuredRaster(other.configuredRaster),
				configuredWidth(other.configuredWidth), configuredHeight(other.configuredHeight)
			{
				other.face = nullptr;
				other.hbFont = nullptr;
			}
			RuntimeFace& operator=(RuntimeFace&& other) noexcept
			{
				if (this != &other)
				{
					if (hbFont)
						hb_font_destroy(hbFont);
					if (face)
						FT_Done_Face(face);
					file = std::move(other.file);
					face = other.face;
					hbFont = other.hbFont;
					configured = other.configured;
					configuredRaster = other.configuredRaster;
					configuredWidth = other.configuredWidth;
					configuredHeight = other.configuredHeight;
					other.face = nullptr;
					other.hbFont = nullptr;
				}
				return *this;
			}
			~RuntimeFace()
			{
				if (hbFont)
					hb_font_destroy(hbFont);
				if (face)
					FT_Done_Face(face);
			}
		};

#pragma pack(push, 1)
		struct PersistentFontHashRecord
		{
			UInt8 magic[8] = {};
			UInt32 version = 1;
			UInt32 recordSize = 0;
			UInt64 normalizedPathHash = 0;
			UInt64 fileSize = 0;
			UInt32 volumeSerial = 0;
			UInt32 fileIndexHigh = 0;
			UInt32 fileIndexLow = 0;
			UInt32 lastWriteHigh = 0;
			UInt32 lastWriteLow = 0;
			UInt64 contentHash = 0;
			UInt64 checksum = 0;
		};
#pragma pack(pop)

		struct CachedGlyphIdentity
		{
			UInt16 faceIndex = 0;
			FT_UInt glyphIndex = 0;
			UInt32 renderedCodePoint = 0;
		};

		struct RuntimeRole
		{
			const ByteStyle* style = nullptr;
			std::vector<RuntimeFace> faces;
			std::unordered_map<UInt32, CachedGlyphIdentity> glyphIdentities;
			float resolvedBaselineOffset = 0.0f;
			float visualCenterCorrection = 0.0f;
			float ascender = 0.0f;
			float descender = 0.0f;
		};

		struct VerticalEffectExtents
		{
			float top = 0.0f;
			float bottom = 0.0f;
		};

		struct ResolvedGlyph
		{
			RuntimeRole* role = nullptr;
			RuntimeFace* runtimeFace = nullptr;
			UInt32 faceIndex = 0;
			FT_UInt glyphIndex = 0;
			UInt32 renderedCodePoint = 0;
		};

		struct MeshCacheKey
		{
			UInt64 generationHash = 0;
			UInt32 fontId = 0;
			UInt32 glyphIndex = 0;
			UInt16 faceIndex = 0;
			UInt8 byteClass = 0;
			UInt8 meshType = 0;

			bool operator==(const MeshCacheKey& other) const
			{
				return generationHash == other.generationHash
					&& fontId == other.fontId
					&& glyphIndex == other.glyphIndex
					&& faceIndex == other.faceIndex
					&& byteClass == other.byteClass
					&& meshType == other.meshType;
			}
		};

		struct MeshCacheKeyHash
		{
			size_t operator()(const MeshCacheKey& key) const
			{
				size_t result = static_cast<size_t>(
					key.generationHash ^ (key.generationHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.faceIndex) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.byteClass) << 8;
				result ^= key.meshType;
				return result;
			}
		};

		struct MeshCacheEntry
		{
			std::shared_ptr<GlyphMesh> mesh;
			size_t bytes = 0;
			std::list<MeshCacheKey>::iterator lru;
		};

		struct BitmapCacheKey
		{
			UInt64 fontContentHash = 0;
			SInt32 fontFaceIndex = 0;
			UInt32 glyphIndex = 0;
			UInt16 effectiveWidth = 0;
			UInt16 effectiveHeight = 0;
			SInt32 embolden26Dot6 = 0;
			SInt32 strokeWidth26Dot6 = 0;
			SInt32 slant16Dot16 = 0;
			UInt8 sdfSpread = 0;
			UInt8 maskType = 0;

			bool operator==(const BitmapCacheKey& other) const
			{
				return fontContentHash == other.fontContentHash
					&& fontFaceIndex == other.fontFaceIndex
					&& glyphIndex == other.glyphIndex
					&& effectiveWidth == other.effectiveWidth
					&& effectiveHeight == other.effectiveHeight
					&& embolden26Dot6 == other.embolden26Dot6
					&& strokeWidth26Dot6 == other.strokeWidth26Dot6
					&& slant16Dot16 == other.slant16Dot16
					&& sdfSpread == other.sdfSpread
					&& maskType == other.maskType;
			}
		};

		struct BitmapCacheKeyHash
		{
			size_t operator()(const BitmapCacheKey& key) const
			{
				size_t result = static_cast<size_t>(
					key.fontContentHash ^ (key.fontContentHash >> 32));
				result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.effectiveWidth) << 16;
				result ^= static_cast<size_t>(key.effectiveHeight);
				result ^= static_cast<size_t>(key.embolden26Dot6) * 0x27D4EB2Du;
				result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.slant16Dot16) * 0x165667B1u;
				result ^= static_cast<size_t>(key.sdfSpread) * 0x165667B1u;
				result ^= key.maskType;
				return result;
			}
		};

		struct BitmapCacheEntry
		{
			std::shared_ptr<GlyphBitmap> bitmap;
			size_t bytes = 0;
			std::list<BitmapCacheKey>::iterator lru;
			UInt32 sourceFontId = 0;
		};

		// Version 7 validates overlap-SDF output and falls back to coverage BSDF
		// when FreeType cannot resolve a glyph's contour geometry. Older masks are
		// left untouched and are retired through normal cleanup.
		constexpr UInt32 kPersistentBitmapVersion = 7;
		constexpr UInt32 kPersistentBitmapRecordMagic = 0x4B534D47u; // GMSK
		constexpr UInt64 kMaximumPersistentProfileBytes = 512ull * 1024ull * 1024ull;
		constexpr UInt32 kMaximumPersistentBitmapBytes = 16u * 1024u * 1024u;

		struct PersistentBitmapProfileKey
		{
			UInt64 fontContentHash = 0;
			SInt32 fontFaceIndex = 0;
			UInt16 effectiveWidth = 0;
			UInt16 effectiveHeight = 0;
			SInt32 embolden26Dot6 = 0;
			SInt32 strokeWidth26Dot6 = 0;
			SInt32 slant16Dot16 = 0;
			UInt8 sdfSpread = 0;
			UInt8 maskType = 0;

			bool operator==(const PersistentBitmapProfileKey& other) const
			{
				return fontContentHash == other.fontContentHash
					&& fontFaceIndex == other.fontFaceIndex
					&& effectiveWidth == other.effectiveWidth
					&& effectiveHeight == other.effectiveHeight
					&& embolden26Dot6 == other.embolden26Dot6
					&& strokeWidth26Dot6 == other.strokeWidth26Dot6
					&& slant16Dot16 == other.slant16Dot16
					&& sdfSpread == other.sdfSpread
					&& maskType == other.maskType;
			}
		};

		struct PersistentBitmapProfileKeyHash
		{
			size_t operator()(const PersistentBitmapProfileKey& key) const
			{
				size_t result = static_cast<size_t>(
					key.fontContentHash ^ (key.fontContentHash >> 32));
				result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.effectiveWidth) << 16;
				result ^= static_cast<size_t>(key.effectiveHeight);
				result ^= static_cast<size_t>(key.embolden26Dot6) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.slant16Dot16) * 0x27D4EB2Du;
				result ^= static_cast<size_t>(key.sdfSpread) << 8;
				result ^= key.maskType;
				return result;
			}
		};

#pragma pack(push, 1)
		struct PersistentBitmapFileHeader
		{
			UInt8 magic[8] = {};
			UInt32 version = 0;
			UInt32 headerSize = 0;
			UInt64 profileHash = 0;
			UInt64 fontContentHash = 0;
			SInt32 fontFaceIndex = 0;
			UInt16 effectiveWidth = 0;
			UInt16 effectiveHeight = 0;
			SInt32 embolden26Dot6 = 0;
			SInt32 strokeWidth26Dot6 = 0;
			SInt32 slant16Dot16 = 0;
			UInt8 sdfSpread = 0;
			UInt8 maskType = 0;
			UInt16 reserved = 0;
			UInt32 glyphCapacity = 0;
			UInt32 indexEntrySize = 0;
			UInt64 dataOffset = 0;
			UInt64 checksum = 0;
		};

		struct PersistentBitmapIndexEntry
		{
			UInt64 offset = 0;
			UInt32 size = 0;
			UInt32 reserved = 0;
		};

		struct PersistentBitmapRecordHeader
		{
			UInt32 magic = 0;
			UInt32 headerSize = 0;
			UInt32 glyphIndex = 0;
			SInt32 width = 0;
			SInt32 height = 0;
			SInt32 left = 0;
			SInt32 top = 0;
			UInt32 alphaSize = 0;
			UInt64 checksum = 0;
		};
#pragma pack(pop)

		struct PersistentBitmapProfile
		{
			PersistentBitmapProfileKey key;
			UInt64 profileHash = 0;
			UInt32 fontId = 0;
			std::wstring fontFileName;
			std::wstring path;
			HANDLE file = INVALID_HANDLE_VALUE;
			HANDLE mapping = nullptr;
			const UInt8* mappedData = nullptr;
			UInt64 mappedSize = 0;
			UInt64 validSize = 0;
			UInt32 glyphCapacity = 0;
			UInt32 recordCount = 0;
			bool writable = false;
			bool initialized = false;
			~PersistentBitmapProfile()
			{
				if (mappedData)
					UnmapViewOfFile(mappedData);
				if (mapping)
					CloseHandle(mapping);
				if (file != INVALID_HANDLE_VALUE)
					CloseHandle(file);
			}
		};

		// Version 5 stores effect-independent body metrics in a sparse, compressed,
		// content-addressed file so identical configurations share it across font IDs.
		constexpr UInt32 kPersistentGlyphManifestVersion = 5;
		constexpr UInt32 kPersistentGlyphManifestEntries = 65536;

#pragma pack(push, 1)
		struct PersistentGlyphManifestHeader
		{
			UInt8 magic[8] = {};
			UInt32 version = 0;
			UInt32 headerSize = 0;
			UInt64 manifestHash = 0;
			UInt64 layoutContentHash = 0;
			UInt64 layoutHash = 0;
			UInt32 reservedFontId = 0;
			UInt32 codePage = 0;
			UInt32 entryCount = 0;
			UInt32 entrySize = 0;
			UInt8 completeMode = 0;
			UInt8 reserved[7] = {};
			UInt64 checksum = 0;
		};

		struct PersistentGlyphManifestEntry
		{
			UInt8 valid = 0;
			UInt8 byteClass = 0;
			UInt16 faceIndex = 0;
			UInt32 glyphIndex = 0;
			UInt32 codePoint = 0;
			UInt32 renderedCodePoint = 0;
			SInt32 textureIndex = 0;
			float width = 0.0f;
			float leadingEdge = 0.0f;
			float height = 0.0f;
			float topEdge = 0.0f;
			float spacing = 0.0f;
			UInt64 checksum = 0;
		};
#pragma pack(pop)

		struct PersistentGlyphManifest
		{
			UInt64 manifestHash = 0;
			UInt64 layoutContentHash = 0;
			std::wstring path;
			HANDLE file = INVALID_HANDLE_VALUE;
			HANDLE mapping = nullptr;
			UInt8* mappedData = nullptr;
			bool writable = false;

			~PersistentGlyphManifest()
			{
				if (mappedData)
					UnmapViewOfFile(mappedData);
				if (mapping)
					CloseHandle(mapping);
				if (file != INVALID_HANDLE_VALUE)
					CloseHandle(file);
			}
		};

		struct LayoutCacheKey
		{
			UInt64 layoutHash = 0;
			UInt32 fontId = 0;
			UInt32 codePage = 0;
			bool allowShaping = false;
			std::string text;

			bool operator==(const LayoutCacheKey& other) const
			{
				return layoutHash == other.layoutHash && fontId == other.fontId
					&& codePage == other.codePage && allowShaping == other.allowShaping
					&& text == other.text;
			}
		};

		struct LayoutCacheLookupKey
		{
			UInt64 layoutHash = 0;
			UInt32 fontId = 0;
			UInt32 codePage = 0;
			bool allowShaping = false;
			std::string_view text;
		};

		struct LayoutCacheKeyHash
		{
			using is_transparent = void;

			size_t operator()(const LayoutCacheKey& key) const
			{
				return Hash(key.layoutHash, key.fontId, key.codePage,
					key.allowShaping, key.text);
			}

			size_t operator()(const LayoutCacheLookupKey& key) const
			{
				return Hash(key.layoutHash, key.fontId, key.codePage,
					key.allowShaping, key.text);
			}

		private:
			static size_t Hash(UInt64 layoutHash, UInt32 fontId, UInt32 codePage,
				bool allowShaping, std::string_view text)
			{
				size_t result = static_cast<size_t>(layoutHash ^ (layoutHash >> 32));
				result ^= static_cast<size_t>(fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(codePage) * 0x85EBCA77u;
				result ^= static_cast<size_t>(allowShaping) << 7;
				for (char value : text)
					result = (result ^ static_cast<UInt8>(value))
						* static_cast<size_t>(16777619u);
				return result;
			}
		};

		struct LayoutCacheKeyEqual
		{
			using is_transparent = void;

			bool operator()(const LayoutCacheKey& lhs,
				const LayoutCacheKey& rhs) const
			{
				return lhs == rhs;
			}

			bool operator()(const LayoutCacheKey& lhs,
				const LayoutCacheLookupKey& rhs) const
			{
				return lhs.layoutHash == rhs.layoutHash && lhs.fontId == rhs.fontId
					&& lhs.codePage == rhs.codePage
					&& lhs.allowShaping == rhs.allowShaping && lhs.text == rhs.text;
			}

			bool operator()(const LayoutCacheLookupKey& lhs,
				const LayoutCacheKey& rhs) const
			{
				return (*this)(rhs, lhs);
			}
		};

		struct LayoutCacheEntry
		{
			FreeTypeLayoutRun layout;
			size_t bytes = 0;
			std::list<LayoutCacheKey>::iterator lru;
		};

		struct KerningCacheKey
		{
			UInt64 layoutHash = 0;
			UInt32 fontId = 0;
			UInt32 codePage = 0;
			UInt16 leftCode = 0;
			UInt16 rightCode = 0;
			UInt8 leftLength = 0;
			UInt8 rightLength = 0;

			bool operator==(const KerningCacheKey& other) const
			{
				return layoutHash == other.layoutHash && fontId == other.fontId
					&& codePage == other.codePage && leftCode == other.leftCode
					&& rightCode == other.rightCode && leftLength == other.leftLength
					&& rightLength == other.rightLength;
			}
		};

		struct KerningCacheKeyHash
		{
			size_t operator()(const KerningCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.layoutHash ^ (key.layoutHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.codePage) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.leftCode) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.rightCode) * 0x27D4EB2Du;
				result ^= static_cast<size_t>(key.leftLength) << 8;
				result ^= key.rightLength;
				return result;
			}
		};

		struct ActiveFontState
		{
			struct OriginalVerticalMetrics
			{
				float baseLine = 0.0f;
				float fontHeight = 0.0f;
				float maxDrop = 0.0f;
				float spaceHeight = 0.0f;
				float spaceTopEdge = 0.0f;
				bool valid = false;
			};

			const FontData* data = nullptr;
			UInt32 fontId = 0;
			OriginalVerticalMetrics originalMetrics;
			bool originalMetricsCaptured = false;
			bool originalFallbackLogged = false;
		};

		FT_Library s_library = nullptr;
		std::unordered_map<std::wstring, std::weak_ptr<MappedFontFile>> s_mappedFiles;
		std::unordered_map<UInt32, std::unique_ptr<RuntimeFont>> s_runtimeFonts;
		std::unordered_map<const Font*, ActiveFontState> s_activeFonts;
		struct ActiveRuntimeCache
		{
			const Font* font = nullptr;
			const void* data = nullptr;
			UInt32 fontId = 0;
			RuntimeFont* runtime = nullptr;
		};
		thread_local ActiveRuntimeCache s_activeRuntimeCache;
		std::unordered_map<MeshCacheKey, MeshCacheEntry, MeshCacheKeyHash> s_meshCache;
		std::list<MeshCacheKey> s_meshLru;
		std::unordered_map<BitmapCacheKey, BitmapCacheEntry, BitmapCacheKeyHash> s_bitmapCache;
		std::list<BitmapCacheKey> s_bitmapLru;
		std::unordered_map<PersistentBitmapProfileKey,
			std::unique_ptr<PersistentBitmapProfile>,
			PersistentBitmapProfileKeyHash> s_persistentBitmapProfiles;
		std::unordered_set<std::wstring> s_usedPersistentCachePaths;
		std::unordered_map<LayoutCacheKey, LayoutCacheEntry, LayoutCacheKeyHash,
			LayoutCacheKeyEqual> s_layoutCache;
		std::list<LayoutCacheKey> s_layoutLru;
		std::unordered_map<KerningCacheKey, float, KerningCacheKeyHash> s_kerningCache;
		std::deque<KerningCacheKey> s_kerningCacheOrder;
		std::array<UInt32, 256> s_singleByteCodePoints = {};
		std::array<UInt32, 65536> s_doubleByteCodePoints = {};
		UInt32 s_codePointCacheCodePage = UINT32_MAX;
		std::unordered_set<UInt32> s_loggedUnconfiguredFontIds;
		std::unordered_set<UInt64> s_loggedVerticalMetricRoles;
		std::unordered_set<UInt64> s_loggedHarfBuzzVerticalRoles;
		UInt32 s_shapingFallbackLogCount = 0;
		UInt32 s_overlapSdfFallbackLogCount = 0;
		bool s_loggedCrossFontBitmapShare = false;
		bool s_loggedPersistentBitmapDirectory = false;
		bool s_loggedPersistentBitmapHit = false;
		bool s_persistentBitmapUnavailable = false;
		UInt32 s_persistentBitmapFailureLogCount = 0;
		size_t s_meshCacheBytes = 0;
		size_t s_bitmapCacheBytes = 0;
		size_t s_layoutCacheBytes = 0;
		std::recursive_mutex s_mutex;

		std::wstring NormalizePathKey(std::wstring path)
		{
			std::replace(path.begin(), path.end(), L'/', L'\\');
			std::transform(path.begin(), path.end(), path.begin(), towlower);
			return path;
		}

		UInt64 HashBytes64(const void* data, size_t size,
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

		bool ResolvePersistentFontContentHash(MappedFontFile& mapped,
			const std::wstring& normalizedPath);

		std::shared_ptr<MappedFontFile> MapFontFile(const std::wstring& path)
		{
			const std::wstring key = NormalizePathKey(path);
			auto existing = s_mappedFiles.find(key);
			if (existing != s_mappedFiles.end())
			{
				if (std::shared_ptr<MappedFontFile> mapped = existing->second.lock())
					return mapped;
			}

			auto mapped = std::make_shared<MappedFontFile>();
			mapped->path = path;
			mapped->file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (mapped->file == INVALID_HANDLE_VALUE)
			{
				gLog.FormattedMessage("tnvse_freetype_font: CreateFileW failed path=%ls win32=%lu",
					path.c_str(), GetLastError());
				return nullptr;
			}

			LARGE_INTEGER size = {};
			if (!GetFileSizeEx(mapped->file, &size)
				|| size.QuadPart <= 0
				|| size.QuadPart > std::numeric_limits<FT_Long>::max())
			{
				gLog.FormattedMessage("tnvse_freetype_font: invalid font file size path=%ls size=%lld win32=%lu",
					path.c_str(), size.QuadPart, GetLastError());
				return nullptr;
			}
			mapped->size = static_cast<FT_Long>(size.QuadPart);
			mapped->mapping = CreateFileMappingW(mapped->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapped->mapping)
			{
				gLog.FormattedMessage("tnvse_freetype_font: CreateFileMappingW failed path=%ls win32=%lu",
					path.c_str(), GetLastError());
				return nullptr;
			}
			mapped->data = static_cast<const FT_Byte*>(MapViewOfFile(mapped->mapping, FILE_MAP_READ, 0, 0, 0));
			if (!mapped->data)
			{
				gLog.FormattedMessage("tnvse_freetype_font: MapViewOfFile failed path=%ls win32=%lu",
					path.c_str(), GetLastError());
				return nullptr;
			}
			if (!ResolvePersistentFontContentHash(*mapped, key))
				mapped->contentHash = HashBytes64(mapped->data,
					static_cast<size_t>(mapped->size));

			s_mappedFiles[key] = mapped;
			return mapped;
		}

		bool InitializeLibrary()
		{
			if (s_library)
				return true;
			if (FT_Init_FreeType(&s_library))
			{
				gLog.FormattedMessage("tnvse_freetype_font: FT_Init_FreeType failed");
				return false;
			}
			return true;
		}

		bool ConfigureFace(FT_Face face, const ByteStyle& style,
			float rasterScale, bool raster)
		{
			if (!face)
				return false;
			const float safeScale = std::isfinite(rasterScale)
				&& rasterScale >= 0.1f && rasterScale <= 10.0f ? rasterScale : 1.0f;
			FT_Error error = 0;
			FT_Matrix matrix = {};
			const float slant = std::tan(style.slantDegrees
				* 3.14159265358979323846f / 180.0f);
			if (raster)
			{
				const FT_UInt width = static_cast<FT_UInt>(std::max(1.0f,
					std::round(style.pixelSize * style.scaleX * safeScale)));
				const FT_UInt height = static_cast<FT_UInt>(std::max(1.0f,
					std::round(style.pixelSize * style.scaleY * safeScale)));
				error = FT_Set_Pixel_Sizes(face, width, height);
				matrix.xx = static_cast<FT_Fixed>(kFixedScale);
				matrix.xy = static_cast<FT_Fixed>(std::lround(slant * kFixedScale));
				matrix.yy = static_cast<FT_Fixed>(kFixedScale);
			}
			else
			{
				error = FT_Set_Char_Size(face, 0,
					static_cast<FT_F26Dot6>(std::lround(style.pixelSize * 64.0f)), 72, 72);
				matrix.xx = static_cast<FT_Fixed>(std::lround(style.scaleX * kFixedScale));
				matrix.xy = static_cast<FT_Fixed>(std::lround(slant * style.scaleY * kFixedScale));
				matrix.yy = static_cast<FT_Fixed>(std::lround(style.scaleY * kFixedScale));
			}
			matrix.yx = 0;
			FT_Set_Transform(face, &matrix, nullptr);
			return error == 0;
		}

		bool ConfigureRuntimeFace(RuntimeFace& runtimeFace, const ByteStyle& style,
			float rasterScale, bool raster)
		{
			const float safeScale = std::isfinite(rasterScale)
				&& rasterScale >= 0.1f && rasterScale <= 10.0f ? rasterScale : 1.0f;
			const FT_UInt width = raster ? static_cast<FT_UInt>(std::max(1.0f,
				std::round(style.pixelSize * style.scaleX * safeScale))) : 0;
			const FT_UInt height = raster ? static_cast<FT_UInt>(std::max(1.0f,
				std::round(style.pixelSize * style.scaleY * safeScale)))
				: static_cast<FT_UInt>(std::max(1.0f, std::round(style.pixelSize * 64.0f)));
			if (runtimeFace.configured && runtimeFace.configuredRaster == raster
				&& runtimeFace.configuredWidth == width && runtimeFace.configuredHeight == height)
			{
				return true;
			}
			if (!ConfigureFace(runtimeFace.face, style, safeScale, raster))
				return false;
			runtimeFace.configured = true;
			runtimeFace.configuredRaster = raster;
			runtimeFace.configuredWidth = width;
			runtimeFace.configuredHeight = height;
			if (runtimeFace.hbFont)
				hb_ft_font_changed(runtimeFace.hbFont);
			return true;
		}

		bool CreateRuntimeFace(const FaceConfig& config, const ByteStyle& style, RuntimeFace& result)
		{
			result.file = MapFontFile(config.path);
			if (!result.file)
				return false;
			FT_Error error = FT_New_Memory_Face(s_library, result.file->data,
				result.file->size, config.faceIndex, &result.face);
			if (error)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: FT_New_Memory_Face failed path=%ls index=%ld error=0x%02X",
					config.path.c_str(), config.faceIndex, static_cast<UInt32>(error));
				return false;
			}
			error = FT_Select_Charmap(result.face, FT_ENCODING_UNICODE);
			if (error)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: FT_Select_Charmap failed path=%ls index=%ld error=0x%02X",
					config.path.c_str(), config.faceIndex, static_cast<UInt32>(error));
				return false;
			}
			if (!ConfigureRuntimeFace(result, style, 1.0f, false))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: base face sizing failed path=%ls index=%ld size=%.2f",
					config.path.c_str(), config.faceIndex, style.pixelSize);
				return false;
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: loaded face path=%ls index=%ld family=%s style=%s faces=%ld glyphs=%ld",
					config.path.c_str(), config.faceIndex,
					result.face->family_name ? result.face->family_name : "",
					result.face->style_name ? result.face->style_name : "",
					result.face->num_faces, result.face->num_glyphs);
			}
			return true;
		}

		bool LoadGlyph(RuntimeRole& role, RuntimeFace& face, FT_UInt glyphIndex)
		{
			if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
				return false;
			if (FT_Load_Glyph(face.face, glyphIndex, kGlyphLoadFlags))
				return false;
			if (face.face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && role.style->embolden > 0.0f)
			{
				const FT_Pos strength = static_cast<FT_Pos>(std::lround(role.style->embolden * 64.0f));
				FT_Outline_EmboldenXY(&face.face->glyph->outline, strength, strength);
			}
			return true;
		}

		bool ResolveExactGlyph(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& result)
		{
			for (UInt32 i = 0; i < role.faces.size(); ++i)
			{
				RuntimeFace& face = role.faces[i];
				const FT_UInt glyphIndex = FT_Get_Char_Index(face.face, codePoint);
				if (!glyphIndex)
					continue;
				result = { &role, &face, i, glyphIndex, codePoint };
				return true;
			}
			return false;
		}

		bool ResolveGlyph(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& result)
		{
			auto cached = role.glyphIdentities.find(codePoint);
			if (cached != role.glyphIdentities.end() && cached->second.faceIndex < role.faces.size())
			{
				const CachedGlyphIdentity& identity = cached->second;
				result = { &role, &role.faces[identity.faceIndex], identity.faceIndex,
					identity.glyphIndex, identity.renderedCodePoint };
				return true;
			}

			if (!ResolveExactGlyph(role, codePoint, result)
				&& (codePoint == 0xFFFD || !ResolveExactGlyph(role, 0xFFFD, result))
				&& (codePoint == '?' || !ResolveExactGlyph(role, '?', result)))
			{
				if (role.faces.empty())
					return false;
				result = { &role, &role.faces.front(), 0, 0, 0 };
			}
			role.glyphIdentities.emplace(codePoint, CachedGlyphIdentity{
				static_cast<UInt16>(result.faceIndex), result.glyphIndex, result.renderedCodePoint });
			return true;
		}

		bool DecodeCodePoint(const char* bytes, int length, UInt32& codePoint)
		{
			if (!bytes || length <= 0)
				return false;
			if (!g_usingWinEncoding)
			{
				codePoint = static_cast<UInt8>(bytes[0]);
				return length == 1;
			}

			if (s_codePointCacheCodePage != g_usingWinEncoding)
			{
				s_singleByteCodePoints.fill(UINT32_MAX);
				s_doubleByteCodePoints.fill(UINT32_MAX);
				s_codePointCacheCodePage = g_usingWinEncoding;
			}
			const UInt32 encoded = length == 1
				? static_cast<UInt8>(bytes[0])
				: (static_cast<UInt32>(static_cast<UInt8>(bytes[0])) << 8)
					| static_cast<UInt8>(bytes[1]);
			UInt32& cached = length == 1
				? s_singleByteCodePoints[encoded] : s_doubleByteCodePoints[encoded];
			if (cached != UINT32_MAX)
			{
				if (cached == UINT32_MAX - 1)
					return false;
				codePoint = cached;
				return true;
			}

			wchar_t wide[2] = {};
			int count = MultiByteToWideChar(g_usingWinEncoding, MB_ERR_INVALID_CHARS,
				bytes, length, wide, static_cast<int>(std::size(wide)));
			if (!count && GetLastError() == ERROR_INVALID_FLAGS)
			{
				count = MultiByteToWideChar(g_usingWinEncoding, 0,
					bytes, length, wide, static_cast<int>(std::size(wide)));
			}
			if (count == 1)
			{
				codePoint = cached = static_cast<UInt16>(wide[0]);
				return true;
			}
			if (count == 2 && wide[0] >= 0xD800 && wide[0] <= 0xDBFF
				&& wide[1] >= 0xDC00 && wide[1] <= 0xDFFF)
			{
				codePoint = cached = 0x10000 + ((wide[0] - 0xD800) << 10) + (wide[1] - 0xDC00);
				return true;
			}
			cached = UINT32_MAX - 1;
			return false;
		}

		bool GetGlyphBox(RuntimeRole& role, UInt32 codePoint, FT_BBox& box, float& advance)
		{
			ResolvedGlyph glyph;
			if (!ResolveGlyph(role, codePoint, glyph))
				return false;
			if (!LoadGlyph(role, *glyph.runtimeFace, glyph.glyphIndex))
				return false;
			FT_GlyphSlot slot = glyph.runtimeFace->face->glyph;
			if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
				FT_Outline_Get_CBox(&slot->outline, &box);
			else
				box = {};
			advance = static_cast<float>(slot->advance.x) / 64.0f;
			return true;
		}

		float GetFixedCellAdvance(const ByteStyle& style)
		{
			return std::max(0.0f, style.fixedWidth + style.tracking);
		}

		float GetFixedCellGlyphOffset(const ByteStyle& style, const FT_GlyphSlot slot)
		{
			if (style.fixedWidth <= 0.0f || !slot || slot->format != FT_GLYPH_FORMAT_OUTLINE)
				return 0.0f;
			FT_BBox box = {};
			FT_Outline_Get_CBox(&slot->outline, &box);
			const float xMin = std::floor(static_cast<float>(box.xMin) / 64.0f);
			const float xMax = std::ceil(static_cast<float>(box.xMax) / 64.0f);
			const float bodyWidth = std::max(0.0f, xMax - xMin);
			const float centeredLeading = (style.fixedWidth - bodyWidth) * 0.5f;
			return centeredLeading - xMin;
		}

		void ApplyResolvedIdentity(VectorEncodedGlyph& glyph, const ResolvedGlyph& resolved)
		{
			glyph.faceIndex = static_cast<UInt16>(resolved.faceIndex);
			glyph.glyphIndex = resolved.glyphIndex;
			glyph.hasGlyphIdentity = true;
		}

		VerticalEffectExtents GetVerticalEffectExtents(const FontConfig& config)
		{
			const float stroke = std::max(
				config.glow.enabled ? std::max(0.0f, config.glow.width) : 0.0f,
				config.outline.enabled ? std::max(0.0f, config.outline.width) : 0.0f);
			const float shadowTop = config.shadow.enabled
				? std::max(0.0f, -config.shadow.y) : 0.0f;
			const float shadowBottom = config.shadow.enabled
				? std::max(0.0f, config.shadow.y) : 0.0f;
			return { std::max(stroke, shadowTop), std::max(stroke, shadowBottom) };
		}

		void RemoveEffectExtentsFromMetrics(const FontConfig& config,
			UInt32 codePoint, FontLetter& metrics)
		{
			if (codePoint == 0x20)
				return;
			const VerticalEffectExtents effects = GetVerticalEffectExtents(config);
			metrics.fTopEdge -= effects.top;
			metrics.fHeight = std::max(0.0f,
				metrics.fHeight - effects.top - effects.bottom);
		}

		void ApplyEffectExtentsToMetrics(const FontConfig& config,
			UInt32 codePoint, FontLetter& metrics)
		{
			if (codePoint == 0x20)
				return;
			const VerticalEffectExtents effects = GetVerticalEffectExtents(config);
			metrics.fTopEdge += effects.top;
			metrics.fHeight += effects.top + effects.bottom;
		}

		FontLetter BuildFontLetter(RuntimeRole& role, const FontConfig& config,
			VectorFontByteClass byteClass, UInt32 codePoint)
		{
			FontLetter result = {};
			result.iTextureIndex = 0;
			FT_BBox box = {};
			float advance = 0.0f;
			if (!GetGlyphBox(role, codePoint, box, advance))
				return result;

			const float xMin = std::floor(static_cast<float>(box.xMin) / 64.0f);
			const float xMax = std::ceil(static_cast<float>(box.xMax) / 64.0f);
			const float bodyBottom = std::floor(static_cast<float>(box.yMin) / 64.0f)
				+ role.resolvedBaselineOffset;
			const float bodyTop = std::ceil(static_cast<float>(box.yMax) / 64.0f)
				+ role.resolvedBaselineOffset;
			const VerticalEffectExtents effects = GetVerticalEffectExtents(config);
			const float glyphBottom = bodyBottom - effects.bottom;
			const float glyphTop = bodyTop + effects.top;
			result.fWidth = std::max(0.0f, xMax - xMin);
			result.fLeadingEdge = role.style->fixedWidth > 0.0f
				? (role.style->fixedWidth - result.fWidth) * 0.5f : xMin;
			result.fHeight = std::max(0.0f, glyphTop - glyphBottom);
			result.fTopEdge = glyphTop;
			const float requestedAdvance = role.style->fixedWidth > 0.0f
				? GetFixedCellAdvance(*role.style)
				: std::max(0.0f, advance + role.style->tracking);
			const float totalAdvance = requestedAdvance;
			if (codePoint == 0x20)
			{
				result.fLeadingEdge = 0.0f;
				result.fWidth = 0.0f;
				result.fSpacing = totalAdvance;
				result.fHeight = 0.0f;
				result.fTopEdge = 0.0f;
				return result;
			}
			if (result.fWidth <= 0.0f && totalAdvance > 0.0f)
			{
				result.fLeadingEdge = 0.0f;
				result.fWidth = totalAdvance;
				result.fSpacing = 0.0f;
			}
			else
			{
				result.fSpacing = totalAdvance - result.fLeadingEdge - result.fWidth;
			}
			if (g_bEnableFreeTypeFontRenderingLog && codePoint >= 0x21)
			{
				const UInt64 logKey = (static_cast<UInt64>(config.fontId) << 8)
					| static_cast<UInt8>(byteClass);
				if (s_loggedVerticalMetricRoles.insert(logKey).second)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: glyph metrics font=%u role=%s codepoint=U+%04X bodyTop=%.2f bodyBottom=%.2f effectTop=%.2f effectBottom=%.2f topEdge=%.2f height=%.2f drop=%.2f configuredBaselineOffset=%.2f visualCorrection=%.2f resolvedBaselineOffset=%.2f",
						config.fontId,
						byteClass == VectorFontByteClass::DoubleByte ? "doubleByte" : "singleByte",
						codePoint, bodyTop, bodyBottom, effects.top, effects.bottom,
						result.fTopEdge, result.fHeight,
						result.fHeight - result.fTopEdge, role.style->baselineOffset,
						role.visualCenterCorrection, role.resolvedBaselineOffset);
				}
			}
			return result;
		}

		bool MeasureVisualCenter(RuntimeRole& role, const UInt32* codePoints, size_t count, float& center)
		{
			float total = 0.0f;
			UInt32 measured = 0;
			for (size_t i = 0; i < count; ++i)
			{
				ResolvedGlyph glyph;
				if (!ResolveExactGlyph(role, codePoints[i], glyph)
					|| glyph.runtimeFace->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
				{
					continue;
				}
				FT_BBox box = {};
				FT_Outline_Get_CBox(&glyph.runtimeFace->face->glyph->outline, &box);
				total += static_cast<float>(box.yMin + box.yMax) / 128.0f;
				++measured;
			}
			if (!measured)
				return false;
			center = total / static_cast<float>(measured);
			return true;
		}

		struct OutlineCollector
		{
			std::vector<std::vector<MeshPoint>> contours;
			float tolerance = 0.35f;

			MeshPoint current() const
			{
				return contours.empty() || contours.back().empty() ? MeshPoint{} : contours.back().back();
			}
		};

		MeshPoint ToPoint(const FT_Vector& point)
		{
			return { static_cast<float>(point.x) / 64.0f, static_cast<float>(point.y) / 64.0f };
		}

		float PointLineDistanceSquared(const MeshPoint& point, const MeshPoint& start, const MeshPoint& end)
		{
			const float dx = end.x - start.x;
			const float dy = end.y - start.y;
			const float lengthSquared = dx * dx + dy * dy;
			if (lengthSquared <= 1.0e-8f)
			{
				const float px = point.x - start.x;
				const float py = point.y - start.y;
				return px * px + py * py;
			}
			const float cross = (point.x - start.x) * dy - (point.y - start.y) * dx;
			return cross * cross / lengthSquared;
		}

		void FlattenQuadratic(OutlineCollector& collector, const MeshPoint& p0,
			const MeshPoint& p1, const MeshPoint& p2, UInt32 depth)
		{
			if (depth >= 16 || PointLineDistanceSquared(p1, p0, p2) <= collector.tolerance * collector.tolerance)
			{
				collector.contours.back().push_back(p2);
				return;
			}
			const MeshPoint p01 = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
			const MeshPoint p12 = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
			const MeshPoint p012 = { (p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f };
			FlattenQuadratic(collector, p0, p01, p012, depth + 1);
			FlattenQuadratic(collector, p012, p12, p2, depth + 1);
		}

		void FlattenCubic(OutlineCollector& collector, const MeshPoint& p0,
			const MeshPoint& p1, const MeshPoint& p2, const MeshPoint& p3, UInt32 depth)
		{
			const float toleranceSquared = collector.tolerance * collector.tolerance;
			if (depth >= 16
				|| (PointLineDistanceSquared(p1, p0, p3) <= toleranceSquared
					&& PointLineDistanceSquared(p2, p0, p3) <= toleranceSquared))
			{
				collector.contours.back().push_back(p3);
				return;
			}
			const MeshPoint p01 = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
			const MeshPoint p12 = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
			const MeshPoint p23 = { (p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f };
			const MeshPoint p012 = { (p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f };
			const MeshPoint p123 = { (p12.x + p23.x) * 0.5f, (p12.y + p23.y) * 0.5f };
			const MeshPoint p0123 = { (p012.x + p123.x) * 0.5f, (p012.y + p123.y) * 0.5f };
			FlattenCubic(collector, p0, p01, p012, p0123, depth + 1);
			FlattenCubic(collector, p0123, p123, p23, p3, depth + 1);
		}

		int MoveToCallback(const FT_Vector* to, void* user)
		{
			auto& collector = *static_cast<OutlineCollector*>(user);
			collector.contours.emplace_back();
			collector.contours.back().push_back(ToPoint(*to));
			return 0;
		}

		int LineToCallback(const FT_Vector* to, void* user)
		{
			auto& collector = *static_cast<OutlineCollector*>(user);
			collector.contours.back().push_back(ToPoint(*to));
			return 0;
		}

		int ConicToCallback(const FT_Vector* control, const FT_Vector* to, void* user)
		{
			auto& collector = *static_cast<OutlineCollector*>(user);
			FlattenQuadratic(collector, collector.current(), ToPoint(*control), ToPoint(*to), 0);
			return 0;
		}

		int CubicToCallback(const FT_Vector* control1, const FT_Vector* control2,
			const FT_Vector* to, void* user)
		{
			auto& collector = *static_cast<OutlineCollector*>(user);
			FlattenCubic(collector, collector.current(), ToPoint(*control1),
				ToPoint(*control2), ToPoint(*to), 0);
			return 0;
		}

		bool TessellateOutline(FT_Outline outline, float tolerance, GlyphMesh& mesh)
		{
			mesh = {};
			if (!outline.n_points || !outline.n_contours)
				return true;

			OutlineCollector collector;
			collector.tolerance = tolerance;
			FT_Outline_Funcs callbacks = {};
			callbacks.move_to = MoveToCallback;
			callbacks.line_to = LineToCallback;
			callbacks.conic_to = ConicToCallback;
			callbacks.cubic_to = CubicToCallback;
			callbacks.shift = 0;
			callbacks.delta = 0;
			if (FT_Outline_Decompose(&outline, &callbacks, &collector))
				return false;

			TESStesselator* tess = tessNewTess(nullptr);
			if (!tess)
				return false;
			for (std::vector<MeshPoint>& contour : collector.contours)
			{
				if (contour.size() > 1 && contour.front().x == contour.back().x
					&& contour.front().y == contour.back().y)
				{
					contour.pop_back();
				}
				if (contour.size() >= 3)
					tessAddContour(tess, 2, contour.data(), sizeof(MeshPoint), static_cast<int>(contour.size()));
			}

			const int ok = tessTesselate(tess, TESS_WINDING_NONZERO,
				TESS_POLYGONS, 3, 2, nullptr);
			if (!ok)
			{
				tessDeleteTess(tess);
				return false;
			}

			const int vertexCount = tessGetVertexCount(tess);
			const TESSreal* vertices = tessGetVertices(tess);
			mesh.vertices.reserve(vertexCount);
			for (int i = 0; i < vertexCount; ++i)
				mesh.vertices.push_back({ vertices[i * 2], vertices[i * 2 + 1] });

			const int elementCount = tessGetElementCount(tess);
			const TESSindex* elements = tessGetElements(tess);
			mesh.indices.reserve(static_cast<size_t>(elementCount) * 3);
			for (int i = 0; i < elementCount; ++i)
			{
				const TESSindex* triangle = elements + i * 3;
				if (triangle[0] == TESS_UNDEF || triangle[1] == TESS_UNDEF || triangle[2] == TESS_UNDEF)
					continue;
				mesh.indices.push_back(static_cast<UInt32>(triangle[0]));
				mesh.indices.push_back(static_cast<UInt32>(triangle[1]));
				mesh.indices.push_back(static_cast<UInt32>(triangle[2]));
			}
			tessDeleteTess(tess);
			return true;
		}

		std::shared_ptr<GlyphMesh> BuildGlyphMesh(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, bool outline);

		void TouchCacheEntry(MeshCacheEntry& entry, const MeshCacheKey& key)
		{
			s_meshLru.splice(s_meshLru.begin(), s_meshLru, entry.lru);
			entry.lru = s_meshLru.begin();
		}

		void TrimMeshCache()
		{
			while (s_meshCacheBytes > kMeshCacheLimit && !s_meshLru.empty())
			{
				const MeshCacheKey key = s_meshLru.back();
				auto it = s_meshCache.find(key);
				if (it != s_meshCache.end())
				{
					s_meshCacheBytes -= it->second.bytes;
					s_meshCache.erase(it);
				}
				s_meshLru.pop_back();
			}
		}

		void TouchBitmapCacheEntry(BitmapCacheEntry& entry, const BitmapCacheKey& key)
		{
			s_bitmapLru.splice(s_bitmapLru.begin(), s_bitmapLru, entry.lru);
			entry.lru = s_bitmapLru.begin();
		}

		void TouchLayoutCacheEntry(LayoutCacheEntry& entry)
		{
			s_layoutLru.splice(s_layoutLru.begin(), s_layoutLru, entry.lru);
			entry.lru = s_layoutLru.begin();
		}

		void TrimBitmapCache()
		{
			while (s_bitmapCacheBytes > GetBitmapCacheLimit() && !s_bitmapLru.empty())
			{
				const BitmapCacheKey key = s_bitmapLru.back();
				auto it = s_bitmapCache.find(key);
				if (it != s_bitmapCache.end())
				{
					s_bitmapCacheBytes -= it->second.bytes;
					s_bitmapCache.erase(it);
				}
				s_bitmapLru.pop_back();
			}
		}

		UInt64 HashBitmapKey(const BitmapCacheKey& key)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t i = 0; i < size; ++i)
				{
					hash ^= bytes[i];
					hash *= 1099511628211ull;
				}
			};
			add(&key.fontContentHash, sizeof(key.fontContentHash));
			add(&key.fontFaceIndex, sizeof(key.fontFaceIndex));
			add(&key.glyphIndex, sizeof(key.glyphIndex));
			add(&key.effectiveWidth, sizeof(key.effectiveWidth));
			add(&key.effectiveHeight, sizeof(key.effectiveHeight));
			add(&key.embolden26Dot6, sizeof(key.embolden26Dot6));
			add(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6));
			add(&key.slant16Dot16, sizeof(key.slant16Dot16));
			add(&key.sdfSpread, sizeof(key.sdfSpread));
			add(&key.maskType, sizeof(key.maskType));
			return hash;
		}

		UInt64 HashPersistentBitmapProfileKey(
			const PersistentBitmapProfileKey& key)
		{
			UInt64 hash = HashBytes64(&kPersistentBitmapVersion,
				sizeof(kPersistentBitmapVersion));
			hash = HashBytes64(&key.fontContentHash, sizeof(key.fontContentHash), hash);
			hash = HashBytes64(&key.fontFaceIndex, sizeof(key.fontFaceIndex), hash);
			hash = HashBytes64(&key.effectiveWidth, sizeof(key.effectiveWidth), hash);
			hash = HashBytes64(&key.effectiveHeight, sizeof(key.effectiveHeight), hash);
			hash = HashBytes64(&key.embolden26Dot6, sizeof(key.embolden26Dot6), hash);
			hash = HashBytes64(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6), hash);
			hash = HashBytes64(&key.slant16Dot16, sizeof(key.slant16Dot16), hash);
			hash = HashBytes64(&key.sdfSpread, sizeof(key.sdfSpread), hash);
			return HashBytes64(&key.maskType, sizeof(key.maskType), hash);
		}

		PersistentBitmapProfileKey MakePersistentBitmapProfileKey(
			const BitmapCacheKey& key, UInt64 fontContentHash)
		{
			return {
				fontContentHash,
				key.fontFaceIndex,
				key.effectiveWidth,
				key.effectiveHeight,
				key.embolden26Dot6,
				key.strokeWidth26Dot6,
				key.slant16Dot16,
				key.sdfSpread,
				key.maskType
			};
		}

		PersistentBitmapFileHeader MakePersistentBitmapFileHeader(
			const PersistentBitmapProfileKey& key, UInt64 profileHash,
			UInt32 glyphCapacity)
		{
			PersistentBitmapFileHeader header;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'M', 'S', 'K', '1' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kPersistentBitmapVersion;
			header.headerSize = sizeof(header);
			header.profileHash = profileHash;
			header.fontContentHash = key.fontContentHash;
			header.fontFaceIndex = key.fontFaceIndex;
			header.effectiveWidth = key.effectiveWidth;
			header.effectiveHeight = key.effectiveHeight;
			header.embolden26Dot6 = key.embolden26Dot6;
			header.strokeWidth26Dot6 = key.strokeWidth26Dot6;
			header.slant16Dot16 = key.slant16Dot16;
			header.sdfSpread = key.sdfSpread;
			header.maskType = key.maskType;
			header.glyphCapacity = glyphCapacity;
			header.indexEntrySize = sizeof(PersistentBitmapIndexEntry);
			header.dataOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(glyphCapacity) * sizeof(PersistentBitmapIndexEntry);
			header.checksum = HashBytes64(&header,
				offsetof(PersistentBitmapFileHeader, checksum));
			return header;
		}

		bool MatchesPersistentBitmapFileHeader(
			const PersistentBitmapFileHeader& header,
			const PersistentBitmapProfileKey& key, UInt64 profileHash,
			UInt32 glyphCapacity)
		{
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'M', 'S', 'K', '1' };
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kPersistentBitmapVersion
				&& header.headerSize == sizeof(header)
				&& header.profileHash == profileHash
				&& header.fontContentHash == key.fontContentHash
				&& header.fontFaceIndex == key.fontFaceIndex
				&& header.effectiveWidth == key.effectiveWidth
				&& header.effectiveHeight == key.effectiveHeight
				&& header.embolden26Dot6 == key.embolden26Dot6
				&& header.strokeWidth26Dot6 == key.strokeWidth26Dot6
				&& header.slant16Dot16 == key.slant16Dot16
				&& header.sdfSpread == key.sdfSpread
				&& header.maskType == key.maskType
				&& header.glyphCapacity == glyphCapacity
				&& header.indexEntrySize == sizeof(PersistentBitmapIndexEntry)
				&& header.dataOffset == sizeof(PersistentBitmapFileHeader)
					+ static_cast<UInt64>(glyphCapacity) * sizeof(PersistentBitmapIndexEntry)
				&& header.checksum == HashBytes64(&header,
					offsetof(PersistentBitmapFileHeader, checksum));
		}

		std::wstring GetPersistentBitmapDirectory()
		{
			std::array<wchar_t, MAX_PATH> modulePath = {};
			const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
				static_cast<DWORD>(modulePath.size()));
			if (!length || length >= modulePath.size())
				return {};
			std::wstring gameDirectory(modulePath.data(), length);
			const size_t slash = gameDirectory.find_last_of(L"\\/");
			if (slash == std::wstring::npos)
				return {};
			gameDirectory.resize(slash);
			return gameDirectory + L"\\Data\\NVSE\\plugins\\tnvse\\fontdata";
		}

		std::wstring SanitizePersistentBitmapFontName(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			std::wstring name = slash == std::wstring::npos
				? path : path.substr(slash + 1);
			if (name.empty())
				name = L"font";
			for (wchar_t& character : name)
			{
				if (character < 0x20 || std::wcschr(L"<>:\"/\\|?*", character))
					character = L'_';
			}
			constexpr size_t kMaximumPersistentFontNameLength = 80;
			if (name.size() > kMaximumPersistentFontNameLength)
				name.resize(kMaximumPersistentFontNameLength);
			return name;
		}

		bool IsPersistentBitmapFontIdName(const wchar_t* fileName)
		{
			if (!fileName)
				return false;
			wchar_t* end = nullptr;
			const unsigned long fontId = std::wcstoul(fileName, &end, 10);
			return end != fileName && end && *end == L'_'
				&& fontId <= std::numeric_limits<UInt32>::max();
		}

		std::wstring FindPersistentBitmapByHash(const std::wstring& directory,
			UInt64 profileHash)
		{
			wchar_t pattern[160] = {};
			_snwprintf_s(pattern, _countof(pattern), _TRUNCATE,
				L"%ls\\*_%016llX.tnvfmask", directory.c_str(),
				static_cast<unsigned long long>(profileHash));
			WIN32_FIND_DATAW found = {};
			HANDLE search = FindFirstFileW(pattern, &found);
			if (search == INVALID_HANDLE_VALUE)
				return {};
			std::wstring path;
			do
			{
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					&& IsPersistentBitmapFontIdName(found.cFileName))
				{
					path = directory + L"\\" + found.cFileName;
					break;
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
			return path;
		}

		std::wstring FormatPersistentBitmapPath(const std::wstring& directory,
			const PersistentBitmapProfile& profile)
		{
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"%u_%ls_%016llX.tnvfmask", profile.fontId,
				profile.fontFileName.c_str(),
				static_cast<unsigned long long>(profile.profileHash));
			return directory + L"\\" + fileName;
		}

		bool DirectoryExists(const std::wstring& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES
				&& (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		bool EnsurePersistentBitmapDirectory(std::wstring& directory)
		{
			directory = GetPersistentBitmapDirectory();
			if (directory.empty())
				return false;
			const size_t suffixLength = std::wcslen(
				L"\\Data\\NVSE\\plugins\\tnvse\\fontdata");
			if (directory.size() <= suffixLength)
				return false;
			const std::wstring gameDirectory = directory.substr(
				0, directory.size() - suffixLength);
			const std::array<std::wstring, 5> paths = {
				gameDirectory + L"\\Data",
				gameDirectory + L"\\Data\\NVSE",
				gameDirectory + L"\\Data\\NVSE\\plugins",
				gameDirectory + L"\\Data\\NVSE\\plugins\\tnvse",
				directory
			};
			for (const std::wstring& path : paths)
			{
				if (!DirectoryExists(path)
					&& !CreateDirectoryW(path.c_str(), nullptr)
					&& GetLastError() != ERROR_ALREADY_EXISTS)
				{
					return false;
				}
			}
			if (!s_loggedPersistentBitmapDirectory)
			{
				s_loggedPersistentBitmapDirectory = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: persistent bitmap cache directory=%ls version=%u",
					directory.c_str(), kPersistentBitmapVersion);
			}
			return true;
		}

		bool GetFileSize64(HANDLE file, UInt64& size)
		{
			LARGE_INTEGER value = {};
			if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &value)
				|| value.QuadPart < 0)
			{
				return false;
			}
			size = static_cast<UInt64>(value.QuadPart);
			return true;
		}

		bool SetFileSize64(HANDLE file, UInt64 size)
		{
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(size);
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& SetEndOfFile(file);
		}

		bool TryEnableSparseFile(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0,
				nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool TryEnableFileCompression(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			USHORT format = COMPRESSION_FORMAT_DEFAULT;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_COMPRESSION,
				&format, sizeof(format), nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool ReadFileAt(HANDLE file, UInt64 offset, void* data, UInt32 size)
		{
			if (!size)
				return true;
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(offset);
			DWORD read = 0;
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& ReadFile(file, data, size, &read, nullptr)
				&& read == size;
		}

		bool WriteFileAt(HANDLE file, UInt64 offset, const void* data, UInt32 size)
		{
			if (!size)
				return true;
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(offset);
			DWORD written = 0;
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& WriteFile(file, data, size, &written, nullptr)
				&& written == size;
		}

		bool ResolvePersistentFontContentHash(MappedFontFile& mapped,
			const std::wstring& normalizedPath)
		{
			BY_HANDLE_FILE_INFORMATION information = {};
			if (mapped.file == INVALID_HANDLE_VALUE
				|| !GetFileInformationByHandle(mapped.file, &information))
				return false;
			const UInt64 pathHash = HashBytes64(normalizedPath.data(),
				normalizedPath.size() * sizeof(wchar_t));
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
				return false;
			wchar_t fileName[256] = {};
			const std::wstring fontName = SanitizePersistentBitmapFontName(mapped.path);
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"hash_%ls_%016llX.tnvfhash", fontName.c_str(),
				static_cast<unsigned long long>(pathHash));
			const std::wstring cachePath = directory + L"\\" + fileName;
			s_usedPersistentCachePaths.insert(NormalizePathKey(cachePath));
			PersistentFontHashRecord record;
			HANDLE cache = CreateFileW(cachePath.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (cache != INVALID_HANDLE_VALUE)
			{
				const bool read = ReadFileAt(cache, 0, &record, sizeof(record));
				CloseHandle(cache);
				const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'H', 'S', 'H', '1' };
				if (read && std::memcmp(record.magic, magic, sizeof(magic)) == 0
					&& record.version == 1 && record.recordSize == sizeof(record)
					&& record.normalizedPathHash == pathHash
					&& record.fileSize == static_cast<UInt64>(mapped.size)
					&& record.volumeSerial == information.dwVolumeSerialNumber
					&& record.fileIndexHigh == information.nFileIndexHigh
					&& record.fileIndexLow == information.nFileIndexLow
					&& record.lastWriteHigh == information.ftLastWriteTime.dwHighDateTime
					&& record.lastWriteLow == information.ftLastWriteTime.dwLowDateTime
					&& record.checksum == HashBytes64(&record,
						offsetof(PersistentFontHashRecord, checksum)))
				{
					mapped.contentHash = record.contentHash;
					return true;
				}
			}

			mapped.contentHash = HashBytes64(mapped.data,
				static_cast<size_t>(mapped.size));
			record = {};
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'H', 'S', 'H', '1' };
			std::memcpy(record.magic, magic, sizeof(magic));
			record.version = 1;
			record.recordSize = sizeof(record);
			record.normalizedPathHash = pathHash;
			record.fileSize = static_cast<UInt64>(mapped.size);
			record.volumeSerial = information.dwVolumeSerialNumber;
			record.fileIndexHigh = information.nFileIndexHigh;
			record.fileIndexLow = information.nFileIndexLow;
			record.lastWriteHigh = information.ftLastWriteTime.dwHighDateTime;
			record.lastWriteLow = information.ftLastWriteTime.dwLowDateTime;
			record.contentHash = mapped.contentHash;
			record.checksum = HashBytes64(&record,
				offsetof(PersistentFontHashRecord, checksum));
			cache = CreateFileW(cachePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (cache != INVALID_HANDLE_VALUE)
			{
				WriteFileAt(cache, 0, &record, sizeof(record));
				CloseHandle(cache);
			}
			return true;
		}

		void UnmapPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			if (profile.mappedData)
				UnmapViewOfFile(profile.mappedData);
			if (profile.mapping)
				CloseHandle(profile.mapping);
			profile.mappedData = nullptr;
			profile.mapping = nullptr;
			profile.mappedSize = 0;
		}

		void MapPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			UnmapPersistentBitmapProfile(profile);
			UInt64 size = 0;
			if (!GetFileSize64(profile.file, size) || !size
				|| size > kMaximumPersistentProfileBytes)
			{
				return;
			}
			profile.mapping = CreateFileMappingW(
				profile.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!profile.mapping)
				return;
			profile.mappedData = static_cast<const UInt8*>(MapViewOfFile(
				profile.mapping, FILE_MAP_READ, 0, 0, 0));
			if (!profile.mappedData)
			{
				CloseHandle(profile.mapping);
				profile.mapping = nullptr;
				return;
			}
			profile.mappedSize = size;
		}

		bool ReadPersistentProfileBytes(const PersistentBitmapProfile& profile,
			UInt64 offset, void* data, UInt32 size)
		{
			if (offset > profile.validSize
				|| static_cast<UInt64>(size) > profile.validSize - offset)
			{
				return false;
			}
			if (profile.mappedData && offset + size <= profile.mappedSize)
			{
				std::memcpy(data, profile.mappedData + static_cast<size_t>(offset), size);
				return true;
			}
			return ReadFileAt(profile.file, offset, data, size);
		}

		bool ResetPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			if (!profile.writable)
				return false;
			UnmapPersistentBitmapProfile(profile);
			const PersistentBitmapFileHeader header =
				MakePersistentBitmapFileHeader(profile.key, profile.profileHash,
					profile.glyphCapacity);
			const UInt64 indexBytes = static_cast<UInt64>(profile.glyphCapacity)
				* sizeof(PersistentBitmapIndexEntry);
			if (header.dataOffset != sizeof(header) + indexBytes
				|| !SetFileSize64(profile.file, 0))
			{
				return false;
			}
			TryEnableSparseFile(profile.file);
			TryEnableFileCompression(profile.file);
			if (!SetFileSize64(profile.file, header.dataOffset)
				|| !WriteFileAt(profile.file, 0, &header, sizeof(header)))
			{
				return false;
			}
			profile.recordCount = 0;
			profile.validSize = header.dataOffset;
			MapPersistentBitmapProfile(profile);
			return true;
		}

		bool IsValidPersistentRecordHeader(
			const PersistentBitmapRecordHeader& record)
		{
			if (record.magic != kPersistentBitmapRecordMagic
				|| record.headerSize != sizeof(record)
				|| record.width <= 0 || record.height <= 0
				|| !record.alphaSize
				|| record.alphaSize > kMaximumPersistentBitmapBytes)
			{
				return false;
			}
			return static_cast<UInt64>(record.width)
				* static_cast<UInt64>(record.height) == record.alphaSize;
		}

		bool InitializePersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			std::wstring directory;
			if (s_persistentBitmapUnavailable
				|| !EnsurePersistentBitmapDirectory(directory))
			{
				s_persistentBitmapUnavailable = true;
				return false;
			}
			profile.path = FindPersistentBitmapByHash(directory,
				profile.profileHash);
			if (profile.path.empty())
				profile.path = FormatPersistentBitmapPath(directory, profile);
			s_usedPersistentCachePaths.insert(NormalizePathKey(profile.path));
			profile.file = CreateFileW(profile.path.c_str(),
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			profile.writable = profile.file != INVALID_HANDLE_VALUE;
			if (profile.writable)
			{
				TryEnableSparseFile(profile.file);
				TryEnableFileCompression(profile.file);
			}
			if (!profile.writable)
			{
				profile.file = CreateFileW(profile.path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (profile.file == INVALID_HANDLE_VALUE)
				return false;

			UInt64 fileSize = 0;
			if (!GetFileSize64(profile.file, fileSize))
				return false;
			profile.validSize = fileSize;
			if (!fileSize || fileSize > kMaximumPersistentProfileBytes)
			{
				if (!ResetPersistentBitmapProfile(profile))
					return false;
				fileSize = profile.validSize;
			}
			else
			{
				MapPersistentBitmapProfile(profile);
				PersistentBitmapFileHeader header;
				if (!ReadPersistentProfileBytes(profile, 0, &header, sizeof(header))
					|| !MatchesPersistentBitmapFileHeader(
						header, profile.key, profile.profileHash,
						profile.glyphCapacity))
				{
					if (!ResetPersistentBitmapProfile(profile))
						return false;
					fileSize = profile.validSize;
				}
			}

			const UInt64 dataOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(profile.glyphCapacity)
					* sizeof(PersistentBitmapIndexEntry);
			if (fileSize < dataOffset)
			{
				if (!ResetPersistentBitmapProfile(profile))
					return false;
				fileSize = profile.validSize;
			}
			profile.recordCount = 0;
			auto inspectEntry = [&](UInt32 glyphIndex,
				const PersistentBitmapIndexEntry& entry)
			{
				if (!entry.offset && !entry.size)
					return;
				if (entry.offset < dataOffset || entry.size < sizeof(PersistentBitmapRecordHeader)
					|| entry.offset > fileSize || entry.size > fileSize - entry.offset)
				{
					if (profile.writable)
					{
						const PersistentBitmapIndexEntry empty;
						const UInt64 entryOffset = sizeof(PersistentBitmapFileHeader)
							+ static_cast<UInt64>(glyphIndex) * sizeof(entry);
						WriteFileAt(profile.file, entryOffset, &empty, sizeof(empty));
					}
					return;
				}
				++profile.recordCount;
			};
			const UInt64 indexBytes = static_cast<UInt64>(profile.glyphCapacity)
				* sizeof(PersistentBitmapIndexEntry);
			if (profile.mappedData
				&& sizeof(PersistentBitmapFileHeader) + indexBytes <= profile.mappedSize)
			{
				const auto* entries = reinterpret_cast<const PersistentBitmapIndexEntry*>(
					profile.mappedData + sizeof(PersistentBitmapFileHeader));
				for (UInt32 glyphIndex = 0; glyphIndex < profile.glyphCapacity; ++glyphIndex)
					inspectEntry(glyphIndex, entries[glyphIndex]);
			}
			else
			{
				constexpr UInt32 kIndexEntriesPerChunk = 4096;
				std::vector<PersistentBitmapIndexEntry> entries(kIndexEntriesPerChunk);
				for (UInt32 first = 0; first < profile.glyphCapacity;)
				{
					const UInt32 count = std::min<UInt32>(kIndexEntriesPerChunk,
						profile.glyphCapacity - first);
					const UInt64 offset = sizeof(PersistentBitmapFileHeader)
						+ static_cast<UInt64>(first) * sizeof(PersistentBitmapIndexEntry);
					const UInt32 bytes = count * sizeof(PersistentBitmapIndexEntry);
					if (!ReadFileAt(profile.file, offset, entries.data(), bytes))
						return false;
					for (UInt32 index = 0; index < count; ++index)
						inspectEntry(first + index, entries[index]);
					first += count;
				}
			}
			profile.validSize = fileSize;
			profile.initialized = true;
			return true;
		}

		PersistentBitmapProfile* GetPersistentBitmapProfile(
			const PersistentBitmapProfileKey& key,
			const std::wstring& fontPath, UInt32 fontId, UInt32 glyphCapacity)
		{
			auto existing = s_persistentBitmapProfiles.find(key);
			if (existing != s_persistentBitmapProfiles.end())
				return existing->second->initialized ? existing->second.get() : nullptr;
			auto profile = std::make_unique<PersistentBitmapProfile>();
			profile->key = key;
			profile->profileHash = HashPersistentBitmapProfileKey(key);
			profile->fontId = fontId;
			profile->glyphCapacity = std::max<UInt32>(1, glyphCapacity);
			profile->fontFileName = SanitizePersistentBitmapFontName(fontPath);
			PersistentBitmapProfile* result = profile.get();
			s_persistentBitmapProfiles.emplace(key, std::move(profile));
			if (InitializePersistentBitmapProfile(*result))
				return result;
			if (s_persistentBitmapFailureLogCount++ < 8)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: persistent bitmap profile unavailable hash=%016llX path=%ls",
					static_cast<unsigned long long>(result->profileHash),
					result->path.empty() ? L"<unresolved>" : result->path.c_str());
			}
			return nullptr;
		}

		UInt64 HashPersistentBitmapRecord(
			const PersistentBitmapRecordHeader& record, const UInt8* alpha)
		{
			UInt64 hash = HashBytes64(&record,
				offsetof(PersistentBitmapRecordHeader, checksum));
			return HashBytes64(alpha, record.alphaSize, hash);
		}

		std::shared_ptr<GlyphBitmap> LoadPersistentGlyphBitmap(
			PersistentBitmapProfile& profile, const BitmapCacheKey& key)
		{
			if (key.glyphIndex >= profile.glyphCapacity)
				return nullptr;
			PersistentBitmapIndexEntry entry;
			const UInt64 entryOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(key.glyphIndex) * sizeof(entry);
			if (!ReadPersistentProfileBytes(profile, entryOffset, &entry, sizeof(entry))
				|| !entry.offset || !entry.size)
				return nullptr;
			PersistentBitmapRecordHeader record;
			if (!ReadPersistentProfileBytes(profile, entry.offset,
					&record, sizeof(record))
				|| !IsValidPersistentRecordHeader(record)
				|| record.glyphIndex != key.glyphIndex
				|| entry.size != sizeof(record) + record.alphaSize)
				return nullptr;
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->width = record.width;
			bitmap->height = record.height;
			bitmap->left = record.left;
			bitmap->top = record.top;
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			bitmap->maskType = static_cast<GlyphMaskType>(key.maskType);
			bitmap->sdfSpread = key.sdfSpread;
			bitmap->strokeWidth26Dot6 = key.strokeWidth26Dot6;
			bitmap->alpha.resize(record.alphaSize);
			const UInt64 alphaOffset = entry.offset + sizeof(record);
			if (!ReadPersistentProfileBytes(profile, alphaOffset,
					bitmap->alpha.data(), record.alphaSize)
				|| record.checksum != HashPersistentBitmapRecord(
					record, bitmap->alpha.data()))
			{
				return nullptr;
			}
			return bitmap;
		}

		bool StorePersistentGlyphBitmap(PersistentBitmapProfile& profile,
			const BitmapCacheKey& key, const GlyphBitmap& bitmap)
		{
			if (!profile.writable || key.glyphIndex >= profile.glyphCapacity
				|| bitmap.width <= 0 || bitmap.height <= 0 || bitmap.alpha.empty()
				|| bitmap.alpha.size() > kMaximumPersistentBitmapBytes
				|| static_cast<UInt64>(bitmap.width) * bitmap.height
					!= bitmap.alpha.size())
			{
				return false;
			}
			PersistentBitmapIndexEntry existing;
			const UInt64 indexOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(key.glyphIndex) * sizeof(existing);
			if (!ReadPersistentProfileBytes(profile, indexOffset, &existing, sizeof(existing))
				|| existing.offset || existing.size)
				return false;
			PersistentBitmapRecordHeader record;
			record.magic = kPersistentBitmapRecordMagic;
			record.headerSize = sizeof(record);
			record.glyphIndex = key.glyphIndex;
			record.width = bitmap.width;
			record.height = bitmap.height;
			record.left = bitmap.left;
			record.top = bitmap.top;
			record.alphaSize = static_cast<UInt32>(bitmap.alpha.size());
			record.checksum = HashPersistentBitmapRecord(record, bitmap.alpha.data());
			const UInt64 recordSize = sizeof(record) + bitmap.alpha.size();
			if (recordSize > kMaximumPersistentProfileBytes - profile.validSize)
				return false;
			std::vector<UInt8> serialized(static_cast<size_t>(recordSize));
			std::memcpy(serialized.data(), &record, sizeof(record));
			std::memcpy(serialized.data() + sizeof(record), bitmap.alpha.data(),
				bitmap.alpha.size());
			const UInt64 offset = profile.validSize;
			// Extending a file while an older, shorter view is still mapped has
			// platform-dependent failure modes. Existing records remain readable
			// through ReadFileAt after the view is released.
			UnmapPersistentBitmapProfile(profile);
			if (!WriteFileAt(profile.file, offset, serialized.data(),
					static_cast<UInt32>(serialized.size())))
			{
				return false;
			}
			profile.validSize += recordSize;
			const PersistentBitmapIndexEntry entry = {
				offset, static_cast<UInt32>(recordSize)
			};
			if (!WriteFileAt(profile.file, indexOffset, &entry, sizeof(entry)))
				return false;
			++profile.recordCount;
			return true;
		}

		bool CopyGrayBitmap(const FT_Bitmap& source, GlyphBitmap& target,
			bool preserveEncodedValues = false)
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
					// SDF pixels are encoded distances, not coverage levels.  FreeType's
					// SDF renderers report num_grays=255 while still using the full 0..255
					// byte range, so normalizing it would turn 255 into 256 and then wrap
					// the glyph interior to zero.
					if (preserveEncodedValues || source.num_grays == 256)
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
	}

	struct RuntimeFont
	{
		const FontConfig* config = nullptr;
		std::array<RuntimeRole, 2> roles;
		std::vector<hb_feature_t> hbFeatures;
		float baseLine = 0.0f;
		float glyphTop = 0.0f;
		float minBottom = 0.0f;
		float glyphHeight = 0.0f;
		float fontHeight = 0.0f;
		bool manualBaseline = false;
		bool initialized = false;
		UInt64 layoutContentHash = 0;
		UInt64 maskContentHash = 0;
		std::unique_ptr<PersistentGlyphManifest> manifest;
	};

	namespace
	{
		UInt64 HashRuntimeFontFaces(RuntimeFont& runtime, UInt64 hash)
		{
			for (const RuntimeRole& role : runtime.roles)
			{
				const UInt32 count = static_cast<UInt32>(role.faces.size());
				hash = HashBytes64(&count, sizeof(count), hash);
				for (const RuntimeFace& face : role.faces)
				{
					const UInt64 contentHash = face.file ? face.file->contentHash : 0;
					const SInt32 faceIndex = face.face
						? static_cast<SInt32>(face.face->face_index) : 0;
					hash = HashBytes64(&contentHash, sizeof(contentHash), hash);
					hash = HashBytes64(&faceIndex, sizeof(faceIndex), hash);
				}
			}
			return hash ? hash : 1;
		}

		UInt64 ComputeRuntimeLayoutContentHash(RuntimeFont& runtime)
		{
			if (runtime.layoutContentHash)
				return runtime.layoutContentHash;
			UInt64 hash = HashBytes64(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion));
			hash = HashBytes64(&runtime.config->layoutHash,
				sizeof(runtime.config->layoutHash), hash);
			runtime.layoutContentHash = HashRuntimeFontFaces(runtime, hash);
			return runtime.layoutContentHash;
		}

		UInt64 ComputeRuntimeMaskContentHash(RuntimeFont& runtime)
		{
			if (runtime.maskContentHash)
				return runtime.maskContentHash;
			UInt64 hash = HashBytes64(&kPersistentBitmapVersion,
				sizeof(kPersistentBitmapVersion));
			hash = HashBytes64(&runtime.config->maskGenerationHash,
				sizeof(runtime.config->maskGenerationHash));
			runtime.maskContentHash = HashRuntimeFontFaces(runtime, hash);
			return runtime.maskContentHash;
		}

		PersistentGlyphManifestHeader MakeGlyphManifestHeader(
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash)
		{
			PersistentGlyphManifestHeader header;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kPersistentGlyphManifestVersion;
			header.headerSize = sizeof(header);
			header.manifestHash = manifestHash;
			header.layoutContentHash = layoutContentHash;
			header.layoutHash = runtime.config->layoutHash;
			header.reservedFontId = 0;
			header.codePage = g_usingWinEncoding;
			header.entryCount = kPersistentGlyphManifestEntries;
			header.entrySize = sizeof(PersistentGlyphManifestEntry);
			header.checksum = HashBytes64(&header,
				offsetof(PersistentGlyphManifestHeader, checksum));
			return header;
		}

		bool MatchesGlyphManifestHeader(const PersistentGlyphManifestHeader& header,
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash)
		{
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kPersistentGlyphManifestVersion
				&& header.headerSize == sizeof(header)
				&& header.manifestHash == manifestHash
				&& header.layoutContentHash == layoutContentHash
				&& header.layoutHash == runtime.config->layoutHash
				&& header.reservedFontId == 0
				&& header.codePage == g_usingWinEncoding
				&& header.entryCount == kPersistentGlyphManifestEntries
				&& header.entrySize == sizeof(PersistentGlyphManifestEntry)
				&& header.checksum == HashBytes64(&header,
					offsetof(PersistentGlyphManifestHeader, checksum));
		}

		PersistentGlyphManifest* GetGlyphManifest(RuntimeFont& runtime)
		{
			if (runtime.manifest)
				return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
			auto manifest = std::make_unique<PersistentGlyphManifest>();
			const UInt64 layoutContentHash = ComputeRuntimeLayoutContentHash(runtime);
			UInt64 manifestHash = HashBytes64(&layoutContentHash,
				sizeof(layoutContentHash));
			manifestHash = HashBytes64(&g_usingWinEncoding,
				sizeof(g_usingWinEncoding), manifestHash);
			manifest->manifestHash = manifestHash;
			manifest->layoutContentHash = layoutContentHash;
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
			{
				runtime.manifest = std::move(manifest);
				return nullptr;
			}
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX.tnvfmanifest",
				static_cast<unsigned long long>(manifestHash));
			manifest->path = directory + L"\\" + fileName;
			s_usedPersistentCachePaths.insert(NormalizePathKey(manifest->path));
			manifest->file = CreateFileW(manifest->path.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			manifest->writable = manifest->file != INVALID_HANDLE_VALUE;
			if (manifest->writable)
			{
				// Packed entries rarely leave full sparse clusters. NTFS compression
				// complements sparse allocation without changing mapped-file access.
				TryEnableSparseFile(manifest->file);
				TryEnableFileCompression(manifest->file);
			}
			if (!manifest->writable)
			{
				manifest->file = CreateFileW(manifest->path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (manifest->file == INVALID_HANDLE_VALUE)
			{
				runtime.manifest = std::move(manifest);
				return nullptr;
			}
			const UInt64 expectedSize = sizeof(PersistentGlyphManifestHeader)
				+ static_cast<UInt64>(kPersistentGlyphManifestEntries)
					* sizeof(PersistentGlyphManifestEntry);
			UInt64 fileSize = 0;
			PersistentGlyphManifestHeader header;
			bool valid = GetFileSize64(manifest->file, fileSize)
				&& fileSize == expectedSize
				&& ReadFileAt(manifest->file, 0, &header, sizeof(header))
				&& MatchesGlyphManifestHeader(header, runtime, manifestHash,
					layoutContentHash);
			if (!valid)
			{
				if (!manifest->writable)
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
				header = MakeGlyphManifestHeader(runtime, manifestHash,
					layoutContentHash);
				if (!SetFileSize64(manifest->file, 0))
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
				TryEnableSparseFile(manifest->file);
				TryEnableFileCompression(manifest->file);
				if (!SetFileSize64(manifest->file, expectedSize)
					|| !WriteFileAt(manifest->file, 0, &header, sizeof(header)))
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
			}
			manifest->mapping = CreateFileMappingW(manifest->file, nullptr,
				manifest->writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
			if (manifest->mapping)
			{
				manifest->mappedData = static_cast<UInt8*>(MapViewOfFile(manifest->mapping,
					manifest->writable ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ,
					0, 0, 0));
			}
			runtime.manifest = std::move(manifest);
			return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
		}

		PersistentGlyphManifestEntry* GetGlyphManifestEntry(
			PersistentGlyphManifest& manifest, UInt32 encodedCode)
		{
			if (!manifest.mappedData || encodedCode >= kPersistentGlyphManifestEntries)
				return nullptr;
			return reinterpret_cast<PersistentGlyphManifestEntry*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader)) + encodedCode;
		}

		bool LoadGlyphManifest(RuntimeFont& runtime, UInt32 encodedCode,
			VectorFontByteClass byteClass, VectorEncodedGlyph* glyph, FontLetter* metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* entry = manifest
				? GetGlyphManifestEntry(*manifest, encodedCode) : nullptr;
			if (!entry || !entry->valid
				|| entry->byteClass != static_cast<UInt8>(byteClass)
				|| entry->checksum != HashBytes64(entry,
					offsetof(PersistentGlyphManifestEntry, checksum)))
				return false;
			RuntimeRole& role = runtime.roles[static_cast<size_t>(byteClass)];
			if (entry->faceIndex >= role.faces.size())
				return false;
			if (glyph)
			{
				glyph->encodedCode = encodedCode;
				glyph->byteClass = byteClass;
				glyph->byteLength = byteClass == VectorFontByteClass::DoubleByte ? 2 : 1;
				glyph->codePoint = entry->codePoint;
				glyph->faceIndex = entry->faceIndex;
				glyph->glyphIndex = entry->glyphIndex;
				glyph->hasGlyphIdentity = true;
			}
			if (metrics)
			{
				metrics->iTextureIndex = entry->textureIndex;
				metrics->fWidth = entry->width;
				metrics->fLeadingEdge = entry->leadingEdge;
				metrics->fHeight = entry->height;
				metrics->fTopEdge = entry->topEdge;
				metrics->fSpacing = entry->spacing;
				ApplyEffectExtentsToMetrics(*runtime.config,
					entry->codePoint, *metrics);
			}
			role.glyphIdentities.emplace(entry->codePoint, CachedGlyphIdentity{
				entry->faceIndex, entry->glyphIndex, entry->renderedCodePoint });
			return true;
		}

		void StoreGlyphManifest(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
			const ResolvedGlyph& resolved, const FontLetter& metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* destination = manifest
				? GetGlyphManifestEntry(*manifest, glyph.encodedCode) : nullptr;
			if (!destination || !manifest->writable || destination->valid)
				return;
			PersistentGlyphManifestEntry entry;
			entry.valid = 1;
			entry.byteClass = static_cast<UInt8>(glyph.byteClass);
			entry.faceIndex = static_cast<UInt16>(resolved.faceIndex);
			entry.glyphIndex = resolved.glyphIndex;
			entry.codePoint = glyph.codePoint;
			entry.renderedCodePoint = resolved.renderedCodePoint;
			FontLetter bodyMetrics = metrics;
			RemoveEffectExtentsFromMetrics(*runtime.config,
				glyph.codePoint, bodyMetrics);
			entry.textureIndex = bodyMetrics.iTextureIndex;
			entry.width = bodyMetrics.fWidth;
			entry.leadingEdge = bodyMetrics.fLeadingEdge;
			entry.height = bodyMetrics.fHeight;
			entry.topEdge = bodyMetrics.fTopEdge;
			entry.spacing = bodyMetrics.fSpacing;
			entry.checksum = HashBytes64(&entry,
				offsetof(PersistentGlyphManifestEntry, checksum));
			std::memcpy(destination, &entry, sizeof(entry));
		}

		bool ResolveVectorGlyph(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
			ResolvedGlyph& result)
		{
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			if (glyph.hasGlyphIdentity && glyph.faceIndex < role.faces.size())
			{
				RuntimeFace& face = role.faces[glyph.faceIndex];
				result = { &role, &face, glyph.faceIndex, glyph.glyphIndex, glyph.codePoint };
				return true;
			}
			return ResolveGlyph(role, glyph.codePoint, result);
		}

		void TrimLayoutCache()
		{
			while (s_layoutCacheBytes > GetLayoutCacheLimit() && !s_layoutLru.empty())
			{
				const LayoutCacheKey key = s_layoutLru.back();
				auto it = s_layoutCache.find(key);
				if (it != s_layoutCache.end())
				{
					s_layoutCacheBytes -= it->second.bytes;
					s_layoutCache.erase(it);
				}
				s_layoutLru.pop_back();
			}
		}

		std::unique_ptr<RuntimeFont> CreateRuntimeFont(const FontConfig& config)
		{
			if (!InitializeLibrary())
				return nullptr;
			auto runtime = std::make_unique<RuntimeFont>();
			runtime->config = &config;
			for (const std::string& featureText : config.shapingFeatures)
			{
				hb_feature_t feature = {};
				if (hb_feature_from_string(featureText.data(),
					static_cast<int>(featureText.size()), &feature))
				{
					runtime->hbFeatures.push_back(feature);
				}
			}

			for (size_t i = 0; i < runtime->roles.size(); ++i)
			{
				RuntimeRole& role = runtime->roles[i];
				role.style = &config.styles[i];
				role.resolvedBaselineOffset = role.style->baselineOffset;
				for (const FaceConfig& faceConfig : role.style->faces)
				{
					RuntimeFace face;
					if (CreateRuntimeFace(faceConfig, *role.style, face))
						role.faces.push_back(std::move(face));
					else
						gLog.FormattedMessage("tnvse_freetype_font: failed to load face font=%u path=%ls index=%ld",
							config.fontId, faceConfig.path.c_str(), faceConfig.faceIndex);
				}
				if (role.faces.empty())
					return nullptr;

				FT_Face primary = role.faces.front().face;
				role.ascender = static_cast<float>(primary->size->metrics.ascender) / 64.0f
					* role.style->scaleY + role.style->embolden;
				role.descender = static_cast<float>(primary->size->metrics.descender) / 64.0f
					* role.style->scaleY - role.style->embolden;
			}

			runtime->manualBaseline = config.baseline > 0.0f;
			if (!runtime->manualBaseline
				|| config.verticalMetrics == VerticalMetricsMode::Original)
			{
				static constexpr UInt32 kSingleReferences[] = { 'H', 'M', 'W', 'A', '0', '8', 'B', 'E', 'N', 'T', 'X' };
				static constexpr UInt32 kDoubleReferences[] = { 0x4E2D, 0x56FD, 0x6F22, 0x3042, 0xAC00 };
				float singleCenter = 0.0f;
				float doubleCenter = 0.0f;
				if (MeasureVisualCenter(runtime->roles[0], kSingleReferences, std::size(kSingleReferences), singleCenter)
					&& MeasureVisualCenter(runtime->roles[1], kDoubleReferences, std::size(kDoubleReferences), doubleCenter))
				{
					const float correction = std::clamp(std::round(singleCenter - doubleCenter), -1.0f, 1.0f);
					runtime->roles[1].visualCenterCorrection = correction;
					runtime->roles[1].resolvedBaselineOffset += correction;
				}
			}

			float maxTop = -std::numeric_limits<float>::infinity();
			float minBottom = std::numeric_limits<float>::infinity();
			for (const RuntimeRole& role : runtime->roles)
			{
				maxTop = std::max(maxTop, role.ascender + role.resolvedBaselineOffset);
				minBottom = std::min(minBottom, role.descender + role.resolvedBaselineOffset);
			}
			const VerticalEffectExtents effects = GetVerticalEffectExtents(config);
			maxTop += effects.top;
			minBottom -= effects.bottom;
			runtime->baseLine = std::ceil(runtime->manualBaseline ? config.baseline : std::max(1.0f, maxTop));
			runtime->glyphTop = maxTop;
			runtime->minBottom = std::min(0.0f, minBottom);
			runtime->glyphHeight = std::max(0.0f, runtime->glyphTop - runtime->minBottom);
			runtime->fontHeight = runtime->baseLine - runtime->minBottom;
			runtime->initialized = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: runtime font id=%u baselineMode=%s baseline=%.2f glyphTop=%.2f bottom=%.2f glyphHeight=%.2f fontHeight=%.2f",
					config.fontId, runtime->manualBaseline ? "manual" : "auto",
					runtime->baseLine, runtime->glyphTop, runtime->minBottom,
					runtime->glyphHeight, runtime->fontHeight);
			}
			return runtime;
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

		bool AppendHarfBuzzLayout(RuntimeFont& runtime,
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
				if (s_loggedHarfBuzzVerticalRoles.insert(logKey).second)
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

		bool BuildLayoutRun(RuntimeFont& runtime, const char* text,
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
					if (!AppendHarfBuzzLayout(runtime, input, begin, end, *glyphs, layout))
					{
						if (g_bEnableFreeTypeFontRenderingLog && s_shapingFallbackLogCount < 32)
						{
							++s_shapingFallbackLogCount;
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
			return true;
		}

		std::shared_ptr<GlyphMesh> BuildGlyphMesh(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, GlyphMeshType meshType)
		{
			auto mesh = std::make_shared<GlyphMesh>();
			ResolvedGlyph resolved;
			if (!ResolveVectorGlyph(runtime, glyph, resolved))
				return mesh;
			RuntimeRole& role = *resolved.role;
			if (!LoadGlyph(role, *resolved.runtimeFace, resolved.glyphIndex))
				return mesh;
			FT_GlyphSlot slot = resolved.runtimeFace->face->glyph;
			if (slot->format != FT_GLYPH_FORMAT_OUTLINE || !slot->outline.n_points)
				return mesh;

			if (meshType == GlyphMeshType::Fill)
			{
				if (!TessellateOutline(slot->outline, runtime.config->curveTolerance, *mesh))
					return nullptr;
				for (MeshPoint& point : mesh->vertices)
					point.y += role.resolvedBaselineOffset;
				return mesh;
			}

			const EffectStyle& stroke = meshType == GlyphMeshType::Glow
				? runtime.config->glow : runtime.config->outline;
			if (!stroke.enabled)
				return mesh;
			FT_Glyph strokedGlyph = nullptr;
			FT_Stroker stroker = nullptr;
			if (FT_Get_Glyph(slot, &strokedGlyph)
				|| FT_Stroker_New(s_library, &stroker))
			{
				if (strokedGlyph)
					FT_Done_Glyph(strokedGlyph);
				return nullptr;
			}
			FT_Stroker_Set(stroker,
				static_cast<FT_Fixed>(std::lround(stroke.width * 64.0f)),
				FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
			const FT_Error strokeError = FT_Glyph_StrokeBorder(&strokedGlyph, stroker, false, true);
			FT_Stroker_Done(stroker);
			if (strokeError || !strokedGlyph || strokedGlyph->format != FT_GLYPH_FORMAT_OUTLINE)
			{
				if (strokedGlyph)
					FT_Done_Glyph(strokedGlyph);
				return nullptr;
			}
			const FT_Outline& strokedOutline = reinterpret_cast<FT_OutlineGlyph>(strokedGlyph)->outline;
			const bool tessellated = TessellateOutline(strokedOutline, runtime.config->curveTolerance, *mesh);
			FT_Done_Glyph(strokedGlyph);
			if (tessellated)
			{
				for (MeshPoint& point : mesh->vertices)
					point.y += role.resolvedBaselineOffset;
			}
			return tessellated ? mesh : nullptr;
		}

		UInt8 ReadCoveragePixel(const FT_Bitmap& bitmap, int x, int y)
		{
			const int pitch = bitmap.pitch;
			const int sourceY = pitch >= 0 ? y : static_cast<int>(bitmap.rows) - 1 - y;
			const UInt8* row = bitmap.buffer
				+ static_cast<ptrdiff_t>(sourceY) * std::abs(pitch);
			if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
				return (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
			if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
				return 0;
			if (bitmap.num_grays == 256)
				return row[x];
			const UInt32 denominator = std::max<UInt32>(1, bitmap.num_grays - 1);
			return static_cast<UInt8>(std::min<UInt32>(255,
				static_cast<UInt32>(row[x]) * 255u / denominator));
		}

		bool HasSdfCoverageMismatch(const FT_Bitmap& sdf, SInt32 sdfLeft,
			SInt32 sdfTop, const FT_BitmapGlyph coverage)
		{
			if (!coverage || !sdf.buffer || !coverage->bitmap.buffer
				|| sdf.pixel_mode != FT_PIXEL_MODE_GRAY
				|| (coverage->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY
					&& coverage->bitmap.pixel_mode != FT_PIXEL_MODE_MONO))
			{
				return false;
			}

			UInt32 solidPixels = 0;
			UInt32 emptyPixels = 0;
			UInt32 lostSolidPixels = 0;
			UInt32 inventedSolidPixels = 0;
			for (int y = 0; y < static_cast<int>(coverage->bitmap.rows); ++y)
			{
				const int sdfY = sdfTop - coverage->top + y;
				if (sdfY < 0 || sdfY >= static_cast<int>(sdf.rows))
					continue;
				const int sdfSourceY = sdf.pitch >= 0
					? sdfY : static_cast<int>(sdf.rows) - 1 - sdfY;
				const UInt8* sdfRow = sdf.buffer
					+ static_cast<ptrdiff_t>(sdfSourceY) * std::abs(sdf.pitch);
				for (int x = 0; x < static_cast<int>(coverage->bitmap.width); ++x)
				{
					const int sdfX = coverage->left + x - sdfLeft;
					if (sdfX < 0 || sdfX >= static_cast<int>(sdf.width))
						continue;
					const UInt8 coverageValue = ReadCoveragePixel(coverage->bitmap, x, y);
					const UInt8 sdfValue = sdfRow[sdfX];
					if (coverageValue >= 224)
					{
						++solidPixels;
						if (sdfValue <= 96)
							++lostSolidPixels;
					}
					else if (coverageValue <= 31)
					{
						++emptyPixels;
						if (sdfValue >= 160)
							++inventedSolidPixels;
					}
				}
			}

			const bool lostCoverage = lostSolidPixels >= 8
				&& static_cast<UInt64>(lostSolidPixels) * 100
					>= static_cast<UInt64>(solidPixels) * 2;
			const bool inventedCoverage = inventedSolidPixels >= 8
				&& static_cast<UInt64>(inventedSolidPixels) * 100
					>= static_cast<UInt64>(emptyPixels) * 2;
			return lostCoverage || inventedCoverage;
		}

		std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
			float rasterScale, const BitmapCacheKey& key)
		{
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			bitmap->maskType = maskType;
			bitmap->sdfSpread = key.sdfSpread;
			bitmap->strokeWidth26Dot6 = key.strokeWidth26Dot6;
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			ResolvedGlyph resolved;
			if (!ResolveVectorGlyph(runtime, glyph, resolved))
				return nullptr;
			if (!ConfigureRuntimeFace(*resolved.runtimeFace, *role.style, rasterScale, true))
				return nullptr;

			const FT_Int32 loadFlags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL
				| FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
			if (FT_Load_Glyph(resolved.runtimeFace->face, resolved.glyphIndex, loadFlags))
				return nullptr;
			FT_GlyphSlot slot = resolved.runtimeFace->face->glyph;
			if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
				return bitmap;
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
				return CopyGrayBitmap(slot->bitmap, *bitmap) ? bitmap : nullptr;
			}

			if (maskType == GlyphMaskType::DistanceField)
			{
				if (key.sdfSpread < 2 || key.sdfSpread > 32)
					return nullptr;
				FT_Int spread = key.sdfSpread;
				// FT_Load_Glyph above applies the face's native/autohinter at the exact
				// device source size.  Render that hinted outline directly: rendering
				// NORMAL first would convert the slot to a bitmap and silently select
				// FreeType's bsdf coverage-to-distance path instead.
				FT_Bool overlaps = (slot->outline.flags & FT_OUTLINE_OVERLAP)
					? 1 : 0;
				const int contourCount = slot->outline.n_contours;
				FT_Glyph coverageGlyph = nullptr;
				if (slot->outline.n_contours > 1
					&& !FT_Get_Glyph(slot, &coverageGlyph))
				{
					if (FT_Glyph_To_Bitmap(&coverageGlyph, FT_RENDER_MODE_NORMAL,
						nullptr, true)
						|| !coverageGlyph
						|| coverageGlyph->format != FT_GLYPH_FORMAT_BITMAP)
					{
						if (coverageGlyph)
							FT_Done_Glyph(coverageGlyph);
						coverageGlyph = nullptr;
					}
				}
				auto releaseCoverage = [&]()
				{
					if (coverageGlyph)
					{
						FT_Done_Glyph(coverageGlyph);
						coverageGlyph = nullptr;
					}
				};
				auto reloadOutline = [&]()
				{
					if (FT_Load_Glyph(resolved.runtimeFace->face,
						resolved.glyphIndex, loadFlags))
						return false;
					slot = resolved.runtimeFace->face->glyph;
					if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
						return false;
					if (role.style->embolden > 0.0f && slot->outline.n_points)
					{
						FT_Outline_EmboldenXY(&slot->outline,
							key.embolden26Dot6, key.embolden26Dot6);
					}
					return true;
				};
				if (FT_Property_Set(s_library, "sdf", "spread", &spread)
					|| FT_Property_Set(s_library, "sdf", "overlaps", &overlaps)
					|| FT_Render_Glyph(slot, FT_RENDER_MODE_SDF))
				{
					releaseCoverage();
					return nullptr;
				}

				bool coverageMismatch = coverageGlyph
					&& HasSdfCoverageMismatch(slot->bitmap, slot->bitmap_left,
						slot->bitmap_top,
						reinterpret_cast<FT_BitmapGlyph>(coverageGlyph));
				bool usedOverlapFallback = false;
				bool usedBsdfFallback = false;
				if (coverageMismatch && !overlaps)
				{
					if (!reloadOutline())
					{
						releaseCoverage();
						return nullptr;
					}
					overlaps = 1;
					if (FT_Property_Set(s_library, "sdf", "spread", &spread)
						|| FT_Property_Set(s_library, "sdf", "overlaps", &overlaps)
						|| FT_Render_Glyph(slot, FT_RENDER_MODE_SDF))
					{
						releaseCoverage();
						return nullptr;
					}
					usedOverlapFallback = true;
					coverageMismatch = HasSdfCoverageMismatch(slot->bitmap,
						slot->bitmap_left, slot->bitmap_top,
						reinterpret_cast<FT_BitmapGlyph>(coverageGlyph));
				}

				if (coverageMismatch)
				{
					// Some fonts mark overlap correctly but still contain contour geometry
					// that FreeType's overlap SDF cannot resolve (for example U+56FE in
					// Sarasa Fixed SC). The normal rasterizer resolves the fill rule first;
					// converting that coverage bitmap to BSDF preserves the intended glyph.
					if (!reloadOutline()
						|| FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)
						|| FT_Property_Set(s_library, "bsdf", "spread", &spread)
						|| FT_Render_Glyph(slot, FT_RENDER_MODE_SDF))
					{
						releaseCoverage();
						return nullptr;
					}
					usedBsdfFallback = true;
				}
				releaseCoverage();

				if ((usedOverlapFallback || usedBsdfFallback)
					&& g_bEnableFreeTypeFontRenderingLog
					&& s_overlapSdfFallbackLogCount < 32)
				{
					++s_overlapSdfFallbackLogCount;
					if (usedBsdfFallback)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: coverage BSDF fallback font=%u glyph=%u codepoint=U+%04X contours=%d",
							runtime.config->fontId, resolved.glyphIndex,
							glyph.codePoint, contourCount);
					}
					else
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: overlap SDF fallback font=%u glyph=%u codepoint=U+%04X contours=%d",
							runtime.config->fontId, resolved.glyphIndex,
							glyph.codePoint, contourCount);
					}
				}
				bitmap->left = slot->bitmap_left;
				bitmap->top = slot->bitmap_top;
				return CopyGrayBitmap(slot->bitmap, *bitmap, true) ? bitmap : nullptr;
			}

			const EffectStyle& effect = maskType == GlyphMaskType::Glow
				? runtime.config->glow : runtime.config->outline;
			if (!effect.enabled || effect.width <= 0.0f || !slot->outline.n_points)
				return bitmap;

			FT_Glyph strokedGlyph = nullptr;
			FT_Stroker stroker = nullptr;
			if (FT_Get_Glyph(slot, &strokedGlyph)
				|| FT_Stroker_New(s_library, &stroker))
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
			return copied ? bitmap : nullptr;
		}
	}

	RuntimeFont* FindRuntimeFont(UInt32 auiFontId)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		auto it = s_runtimeFonts.find(auiFontId);
		return it == s_runtimeFonts.end() ? nullptr : it->second.get();
	}

	RuntimeFont* FindActiveRuntime(const Font* apFont)
	{
		if (!apFont || !g_bEnableFreeTypeFontRendering)
			return nullptr;

		const UInt32 fontId = static_cast<UInt32>(apFont->iFontNum);
		if (s_activeRuntimeCache.font == apFont
			&& s_activeRuntimeCache.data == apFont->pFontData
			&& s_activeRuntimeCache.fontId == fontId
			&& s_activeRuntimeCache.runtime)
		{
			return s_activeRuntimeCache.runtime;
		}

		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		const auto active = s_activeFonts.find(apFont);
		if (active == s_activeFonts.end() || active->second.data != apFont->pFontData
			|| active->second.fontId != fontId)
		{
			return nullptr;
		}
		const auto runtime = s_runtimeFonts.find(fontId);
		if (runtime == s_runtimeFonts.end())
			return nullptr;
		s_activeRuntimeCache = {
			apFont, apFont->pFontData, fontId, runtime->second.get()
		};
		return s_activeRuntimeCache.runtime;
	}

	RuntimeFont* EnsureRuntimeFont(UInt32 auiFontId)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		if (RuntimeFont* runtime = FindRuntimeFont(auiFontId))
			return runtime;
		const FontConfig* config = FindConfig(auiFontId);
		if (!config)
			return nullptr;
		std::unique_ptr<RuntimeFont> runtime = CreateRuntimeFont(*config);
		if (!runtime)
		{
			gLog.FormattedMessage("tnvse_freetype_font: failed to initialize font id=%u", auiFontId);
			return nullptr;
		}
		RuntimeFont* result = runtime.get();
		s_runtimeFonts.emplace(auiFontId, std::move(runtime));
		return result;
	}

	ActiveFontState::OriginalVerticalMetrics CaptureOriginalVerticalMetrics(const Font& font)
	{
		ActiveFontState::OriginalVerticalMetrics result;
		if (!font.pFontData)
			return result;

		const FontLetter& space = font.pFontData->pFontLetters[' '];
		result.baseLine = font.pFontData->fBaseLine;
		result.fontHeight = font.fFontHeight;
		result.maxDrop = font.fMaxDrop;
		result.spaceHeight = space.fHeight;
		result.spaceTopEdge = space.fTopEdge;
		result.valid = std::isfinite(result.baseLine) && result.baseLine > 0.0f
			&& std::isfinite(result.fontHeight) && result.fontHeight >= 0.0f
			&& std::isfinite(result.maxDrop) && result.maxDrop <= 0.0f
			&& std::isfinite(result.spaceHeight) && result.spaceHeight >= 0.0f
			&& std::isfinite(result.spaceTopEdge);
		return result;
	}

	bool ApplyRuntimeMetrics(RuntimeFont& runtime, Font& font)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		if (!runtime.initialized || !font.pFontData)
			return false;

		ActiveFontState activeState;
		const auto active = s_activeFonts.find(&font);
		if (active != s_activeFonts.end()
			&& active->second.data == font.pFontData
			&& active->second.fontId == static_cast<UInt32>(font.iFontNum))
		{
			activeState = active->second;
		}
		else
		{
			activeState.data = font.pFontData;
			activeState.fontId = static_cast<UInt32>(font.iFontNum);
		}
		if (!activeState.originalMetricsCaptured)
		{
			activeState.originalMetrics = CaptureOriginalVerticalMetrics(font);
			activeState.originalMetricsCaptured = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const auto& original = activeState.originalMetrics;
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: original metrics snapshot font=%u valid=%d baseline=%.2f fontHeight=%.2f maxDrop=%.2f space=(top=%.2f height=%.2f)",
					font.iFontNum, original.valid ? 1 : 0, original.baseLine,
					original.fontHeight, original.maxDrop,
					original.spaceTopEdge, original.spaceHeight);
			}
		}

		for (UInt32 value = 0x20; value <= 0xFF; ++value)
		{
			if (value == 0x7F)
				continue;
			if (LoadGlyphManifest(runtime, value,
				VectorFontByteClass::SingleByte, nullptr,
				&font.pFontData->pFontLetters[value]))
				continue;
			char byte = static_cast<char>(value);
			UInt32 codePoint = 0xFFFD;
			DecodeCodePoint(&byte, 1, codePoint);
			FontLetter metrics = BuildFontLetter(
				runtime.roles[0], *runtime.config,
				VectorFontByteClass::SingleByte, codePoint);
			font.pFontData->pFontLetters[value] = metrics;
			ResolvedGlyph resolved;
			if (ResolveGlyph(runtime.roles[0], codePoint, resolved))
			{
				VectorEncodedGlyph glyph;
				glyph.encodedCode = value;
				glyph.byteClass = VectorFontByteClass::SingleByte;
				glyph.byteLength = 1;
				glyph.codePoint = codePoint;
				StoreGlyphManifest(runtime, glyph, resolved, metrics);
			}
		}

		const bool requestedOriginal = runtime.config->verticalMetrics == VerticalMetricsMode::Original;
		const bool useOriginal = requestedOriginal && activeState.originalMetrics.valid;
		if (requestedOriginal && !useOriginal && !activeState.originalFallbackLogged)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: invalid original metrics font=%u; falling back to freetype vertical metrics",
				font.iFontNum);
			activeState.originalFallbackLogged = true;
		}

		const auto& original = activeState.originalMetrics;
		const bool configuredBaseline = runtime.config->baseline > 0.0f;
		const float resolvedBaseline = useOriginal
			? (configuredBaseline
				? std::ceil(std::max(1.0f, runtime.config->baseline))
				: original.baseLine)
			: runtime.baseLine;
		const float resolvedMaxDrop = useOriginal ? original.maxDrop : runtime.minBottom;
		const float resolvedFontHeight = useOriginal
			? resolvedBaseline - original.maxDrop : runtime.fontHeight;
		const float resolvedSpaceHeight = useOriginal
			? original.spaceHeight : runtime.glyphHeight;
		const float resolvedSpaceTop = useOriginal
			? original.maxDrop + original.spaceHeight
			: runtime.minBottom + runtime.glyphHeight;

		font.pFontData->fBaseLine = resolvedBaseline;
		font.fMaxDrop = resolvedMaxDrop;
		font.fFontHeight = resolvedFontHeight;
		font.iLineOverlap = 0;
		FontLetter& space = font.pFontData->pFontLetters[' '];
		const float serializedSpaceWidth = space.fWidth;
		space.fWidth = space.fSpacing;
		space.fSpacing = serializedSpaceWidth;
		space.fHeight = resolvedSpaceHeight;
		space.fTopEdge = resolvedSpaceTop;
		font.pFontData->pFontLetters[160] = space;
		font.pFontData->pFontLetters[0x7F] = font.pFontData->pFontLetters['|'];
		font.pFontData->pFontLetters[0].fWidth = 0.0f;
		font.pFontData->pFontLetters[0].fSpacing = 0.0f;
		font.pFontData->pFontLetters[0].fHeight = resolvedSpaceHeight;
		font.pFontData->pFontLetters[0].fTopEdge = resolvedSpaceTop;
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: applied metrics font=%u verticalMetrics=%s baselineSource=%s requestedBaseline=%.2f resolvedBaseline=%.2f fontHeight=%.2f maxDrop=%.2f space=(width=%.2f spacing=%.2f top=%.2f height=%.2f)",
				font.iFontNum, useOriginal ? "original" : "freetype",
				useOriginal ? (configuredBaseline ? "configured" : "original")
					: (configuredBaseline ? "configured" : "freetype"),
				runtime.config->baseline, font.pFontData->fBaseLine,
				font.fFontHeight, font.fMaxDrop,
				space.fWidth, space.fSpacing, space.fTopEdge, space.fHeight);
		}

		ExtraGlyphMap& extra = gNumberedExtraLetters[font.iFontNum];
		extra.clear();
		extra.reserve(25000);
		s_activeFonts[&font] = activeState;
		return true;
	}

	FontLetter* EnsureDoubleByteMetrics(RuntimeFont& runtime, Font& font, UInt32 encodedCode)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		ExtraGlyphMap& extra = gNumberedExtraLetters[font.iFontNum];
		auto existing = extra.find(encodedCode);
		if (existing != extra.end())
			return &existing->second;
		FontLetter cachedMetrics;
		if (LoadGlyphManifest(runtime, encodedCode,
			VectorFontByteClass::DoubleByte, nullptr, &cachedMetrics))
		{
			auto [cached, inserted] = extra.emplace(encodedCode, cachedMetrics);
			return &cached->second;
		}

		const char bytes[2] = {
			static_cast<char>((encodedCode >> 8) & 0xFF),
			static_cast<char>(encodedCode & 0xFF)
		};
		UInt32 codePoint = 0xFFFD;
		DecodeCodePoint(bytes, 2, codePoint);
		FontLetter metrics = BuildFontLetter(runtime.roles[1], *runtime.config,
			VectorFontByteClass::DoubleByte, codePoint);
		auto [it, inserted] = extra.emplace(encodedCode, metrics);
		ResolvedGlyph resolved;
		if (ResolveGlyph(runtime.roles[1], codePoint, resolved))
		{
			VectorEncodedGlyph glyph;
			glyph.encodedCode = encodedCode;
			glyph.byteClass = VectorFontByteClass::DoubleByte;
			glyph.byteLength = 2;
			glyph.codePoint = codePoint;
			StoreGlyphManifest(runtime, glyph, resolved, metrics);
		}
		return &it->second;
	}

	bool DecodeEncodedGlyphIdentity(RuntimeFont& runtime, const char* text,
		VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return false;

		UInt32 encodedCode = 0;
		if (text[1] && TryDecodeDoubleByte(text, encodedCode))
		{
			glyph.encodedCode = encodedCode;
			glyph.byteLength = 2;
			glyph.byteClass = VectorFontByteClass::DoubleByte;
			const char bytes[2] = { text[0], text[1] };
			if (!DecodeCodePoint(bytes, 2, glyph.codePoint))
				glyph.codePoint = 0xFFFD;
			if (LoadGlyphManifest(runtime, encodedCode, glyph.byteClass, &glyph, nullptr))
				return true;
			ResolvedGlyph resolved;
			if (ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
				glyph.codePoint, resolved))
			{
				ApplyResolvedIdentity(glyph, resolved);
			}
			return true;
		}

		glyph.encodedCode = static_cast<UInt8>(text[0]);
		glyph.byteLength = 1;
		glyph.byteClass = VectorFontByteClass::SingleByte;
		if (!DecodeCodePoint(text, 1, glyph.codePoint))
			glyph.codePoint = 0xFFFD;
		if (LoadGlyphManifest(runtime, glyph.encodedCode, glyph.byteClass, &glyph, nullptr))
			return true;
		ResolvedGlyph resolved;
		if (ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
			glyph.codePoint, resolved))
		{
			ApplyResolvedIdentity(glyph, resolved);
		}
		return true;
	}

	bool DecodeEncodedGlyph(RuntimeFont& runtime, Font& font, const char* text,
		VectorEncodedGlyph& glyph)
	{
		if (!DecodeEncodedGlyphIdentity(runtime, text, glyph))
			return false;
		glyph.metrics = glyph.byteClass == VectorFontByteClass::DoubleByte
			? EnsureDoubleByteMetrics(runtime, font, glyph.encodedCode)
			: &font.pFontData->pFontLetters[glyph.encodedCode & 0xFF];
		return glyph.metrics != nullptr;
	}

	bool ResolvePrewarmGlyph(RuntimeFont& runtime, const char* bytes,
		size_t length, VectorEncodedGlyph& glyph)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		glyph = {};
		if (!bytes || (length != 1 && length != 2))
			return false;

		glyph.byteLength = static_cast<UInt8>(length);
		glyph.byteClass = length == 2
			? VectorFontByteClass::DoubleByte : VectorFontByteClass::SingleByte;
		glyph.encodedCode = length == 2
			? (static_cast<UInt32>(static_cast<UInt8>(bytes[0])) << 8)
				| static_cast<UInt8>(bytes[1])
			: static_cast<UInt8>(bytes[0]);
		if (LoadGlyphManifest(runtime, glyph.encodedCode, glyph.byteClass, &glyph, nullptr))
			return true;
		if (!DecodeCodePoint(bytes, static_cast<int>(length), glyph.codePoint))
			return false;

		ResolvedGlyph resolved;
		if (!ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
			glyph.codePoint, resolved))
		{
			return false;
		}
		ApplyResolvedIdentity(glyph, resolved);
		const FontLetter metrics = BuildFontLetter(
			runtime.roles[static_cast<size_t>(glyph.byteClass)], *runtime.config,
			glyph.byteClass, glyph.codePoint);
		StoreGlyphManifest(runtime, glyph, resolved, metrics);
		return glyph.hasGlyphIdentity;
	}

	const FontConfig& GetRuntimeConfig(const RuntimeFont& runtime)
	{
		return *runtime.config;
	}

	UInt64 GetRuntimeMaskContentHash(RuntimeFont& runtime)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		return ComputeRuntimeMaskContentHash(runtime);
	}

	bool GetFreeTypeFontCacheDirectory(std::wstring& directory)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		return EnsurePersistentBitmapDirectory(directory);
	}

	void MarkFreeTypeFontCacheFileUsed(const std::wstring& path)
	{
		if (path.empty())
			return;
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		s_usedPersistentCachePaths.insert(NormalizePathKey(path));
	}

	void DeleteUnusedFreeTypeFontCacheFiles()
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		std::wstring directory;
		if (!EnsurePersistentBitmapDirectory(directory))
			return;
		const std::wstring pattern = directory + L"\\*";
		WIN32_FIND_DATAW found = {};
		HANDLE search = FindFirstFileW(pattern.c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return;
		UInt32 deleted = 0;
		UInt32 failed = 0;
		UInt64 deletedBytes = 0;
		auto hasSuffix = [](const std::wstring& value, const wchar_t* suffix)
		{
			const size_t length = std::wcslen(suffix);
			return value.size() >= length
				&& value.compare(value.size() - length, length, suffix) == 0;
		};
		do
		{
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const std::wstring path = directory + L"\\" + found.cFileName;
			const std::wstring normalized = NormalizePathKey(path);
			const bool managed = hasSuffix(normalized, L".tnvfmask")
				|| hasSuffix(normalized, L".tnvfhash")
				|| hasSuffix(normalized, L".tnvfmanifest")
				|| hasSuffix(normalized, L".tnvfatlas")
				|| hasSuffix(normalized, L".tnvfatlas.tmp");
			if (!managed || s_usedPersistentCachePaths.count(normalized))
				continue;
			const UInt64 size = (static_cast<UInt64>(found.nFileSizeHigh) << 32)
				| found.nFileSizeLow;
			if (DeleteFileW(path.c_str()))
			{
				++deleted;
				deletedBytes += size;
			}
			else
			{
				++failed;
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
		gLog.FormattedMessage(
			"tnvse_freetype_font: unused persistent cache cleanup deleted=%u bytes=%llu failed=%u retained=%llu",
			deleted, static_cast<unsigned long long>(deletedBytes), failed,
			static_cast<unsigned long long>(s_usedPersistentCachePaths.size()));
	}

	bool HasCompleteGlyphManifest(RuntimeFont& runtime, FontPrewarmMode mode)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData)
			return false;
		const auto* header = reinterpret_cast<const PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		return header->completeMode >= static_cast<UInt8>(mode);
	}

	void MarkGlyphManifestComplete(RuntimeFont& runtime, FontPrewarmMode mode)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData || !manifest->writable)
			return;
		auto* header = reinterpret_cast<PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		if (header->completeMode >= static_cast<UInt8>(mode))
			return;
		header->completeMode = static_cast<UInt8>(mode);
		header->checksum = HashBytes64(header,
			offsetof(PersistentGlyphManifestHeader, checksum));
		FlushViewOfFile(header, sizeof(*header));
	}

	void FlushGlyphBitmapDiskCache()
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		UInt32 profileCount = 0;
		UInt64 recordCount = 0;
		UInt64 byteCount = 0;
		for (auto& pair : s_persistentBitmapProfiles)
		{
			PersistentBitmapProfile& profile = *pair.second;
			if (!profile.initialized || profile.file == INVALID_HANDLE_VALUE)
				continue;
			if (profile.writable)
				FlushFileBuffers(profile.file);
			++profileCount;
			recordCount += profile.recordCount;
			byteCount += profile.validSize;
		}
		for (auto& pair : s_runtimeFonts)
		{
			RuntimeFont& runtime = *pair.second;
			if (!runtime.manifest || !runtime.manifest->mappedData)
				continue;
			FlushViewOfFile(runtime.manifest->mappedData, 0);
			if (runtime.manifest->writable)
				FlushFileBuffers(runtime.manifest->file);
		}
		if (profileCount)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: persistent bitmap cache flushed profiles=%u records=%llu bytes=%llu",
				profileCount, static_cast<unsigned long long>(recordCount),
				static_cast<unsigned long long>(byteCount));
		}
	}

	float GetGlyphBaselineOffset(const RuntimeFont& runtime,
		VectorFontByteClass byteClass)
	{
		return runtime.roles[static_cast<size_t>(byteClass)].resolvedBaselineOffset;
	}

	bool LayoutRuntimeRun(RuntimeFont& runtime, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		const LayoutCacheLookupKey lookup = {
			runtime.config->layoutHash,
			runtime.config->fontId,
			g_usingWinEncoding,
			allowShaping,
			std::string_view(text, length)
		};
		auto existing = s_layoutCache.find(lookup);
		if (existing != s_layoutCache.end())
		{
			TouchLayoutCacheEntry(existing->second);
			layout = existing->second.layout;
			RecordFreeTypePerf(FreeTypePerfCounter::LayoutHit);
			return true;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::LayoutMiss);
		if (!BuildLayoutRun(runtime, text, length, allowShaping, layout))
			return false;
		if (length <= 65536)
		{
			LayoutCacheKey key = {
				runtime.config->layoutHash,
				runtime.config->fontId,
				g_usingWinEncoding,
				allowShaping,
				std::string(text, length)
			};
			FreeTypeLayoutRun cachedLayout = layout;
			const size_t bytes = sizeof(LayoutCacheEntry) + key.text.size()
				+ layout.glyphs->size() * sizeof(FreeTypeLayoutGlyph);
			s_layoutLru.push_front(key);
			s_layoutCache.emplace(std::move(key),
				LayoutCacheEntry{ std::move(cachedLayout), bytes, s_layoutLru.begin() });
			s_layoutCacheBytes += bytes;
			TrimLayoutCache();
		}
		return true;
	}

	std::shared_ptr<const GlyphMesh> GetGlyphMesh(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMeshType meshType)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		UInt64 generationHash = runtime.config->maskGenerationHash
			^ runtime.config->layoutHash;
		generationHash = HashBytes64(&runtime.config->curveTolerance,
			sizeof(runtime.config->curveTolerance), generationHash);
		if (meshType != GlyphMeshType::Fill)
		{
			const EffectStyle& effect = meshType == GlyphMeshType::Glow
				? runtime.config->glow : runtime.config->outline;
			generationHash = HashBytes64(&effect.enabled,
				sizeof(effect.enabled), generationHash);
			generationHash = HashBytes64(&effect.width,
				sizeof(effect.width), generationHash);
		}
		const MeshCacheKey key = {
			generationHash,
			runtime.config->fontId,
			glyph.glyphIndex,
			glyph.faceIndex,
			static_cast<UInt8>(glyph.byteClass),
			static_cast<UInt8>(meshType)
		};
		auto existing = s_meshCache.find(key);
		if (existing != s_meshCache.end())
		{
			TouchCacheEntry(existing->second, key);
			return existing->second.mesh;
		}

		std::shared_ptr<GlyphMesh> mesh = BuildGlyphMesh(runtime, glyph, meshType);
		if (!mesh)
			return nullptr;
		const size_t bytes = mesh->vertices.size() * sizeof(MeshPoint)
			+ mesh->indices.size() * sizeof(UInt32);
		s_meshLru.push_front(key);
		s_meshCache.emplace(key, MeshCacheEntry{ mesh, bytes, s_meshLru.begin() });
		s_meshCacheBytes += bytes;
		TrimMeshCache();
		return mesh;
	}

	std::shared_ptr<const GlyphBitmap> GetGlyphBitmap(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMaskType maskType, float rasterScale,
		UInt32 sdfSpread)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		const float safeScale = std::isfinite(rasterScale)
			&& rasterScale >= 0.1f && rasterScale <= 10.0f ? rasterScale : 1.0f;
		ResolvedGlyph resolved;
		if (!ResolveVectorGlyph(runtime, glyph, resolved) || !resolved.role
			|| !resolved.runtimeFace || !resolved.runtimeFace->face
			|| !resolved.runtimeFace->file)
		{
			return nullptr;
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
			return nullptr;
		const float slant = std::tan(style.slantDegrees
			* 3.14159265358979323846f / 180.0f);
		const SInt32 slant16Dot16 = static_cast<SInt32>(std::lround(
			slant * kFixedScale));
		const BitmapCacheKey key = {
			resolved.runtimeFace->file->contentHash,
			static_cast<SInt32>(resolved.runtimeFace->face->face_index),
			resolved.glyphIndex,
			static_cast<UInt16>(effectiveWidth),
			static_cast<UInt16>(effectiveHeight),
			embolden,
			strokeWidth,
			slant16Dot16,
			resolvedSdfSpread,
			static_cast<UInt8>(maskType)
		};
		auto existing = s_bitmapCache.find(key);
		if (existing != s_bitmapCache.end())
		{
			TouchBitmapCacheEntry(existing->second, key);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapMemoryHit);
			if (existing->second.sourceFontId != runtime.config->fontId)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::BitmapCrossFontHit);
				if (g_bEnableFreeTypeFontRenderingLog
					&& !s_loggedCrossFontBitmapShare)
				{
					s_loggedCrossFontBitmapShare = true;
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
		PersistentBitmapProfile* persistentProfile =
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
					&& !s_loggedPersistentBitmapHit)
				{
					s_loggedPersistentBitmapHit = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first persistent bitmap cache hit path=%ls font=%u glyph=%u size=%ux%u mask=%u bytes=%u records=%u",
						persistentProfile->path.c_str(), runtime.config->fontId,
						key.glyphIndex, key.effectiveWidth, key.effectiveHeight,
						key.maskType, static_cast<UInt32>(diskBitmap->alpha.size()),
						persistentProfile->recordCount);
				}
				const size_t bytes = sizeof(GlyphBitmap) + diskBitmap->alpha.size();
				s_bitmapLru.push_front(key);
				s_bitmapCache.emplace(key, BitmapCacheEntry{
					diskBitmap, bytes, s_bitmapLru.begin(), runtime.config->fontId });
				s_bitmapCacheBytes += bytes;
				TrimBitmapCache();
				return diskBitmap;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskMiss);
		}

		RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized);
		std::shared_ptr<GlyphBitmap> bitmap =
			BuildGlyphBitmap(runtime, glyph, maskType, safeScale, key);
		if (!bitmap)
			return nullptr;
		if (persistentProfile
			&& StorePersistentGlyphBitmap(*persistentProfile, key, *bitmap))
		{
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWrite);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapDiskWriteBytes,
				bitmap->alpha.size());
		}
		const size_t bytes = sizeof(GlyphBitmap) + bitmap->alpha.size();
		s_bitmapLru.push_front(key);
		s_bitmapCache.emplace(key,
			BitmapCacheEntry{ bitmap, bytes, s_bitmapLru.begin(), runtime.config->fontId });
		s_bitmapCacheBytes += bytes;
		TrimBitmapCache();
		return bitmap;
	}
}

namespace fonthook
{
	void FlushFreeTypePersistentFontCache()
	{
		vectorfont::FlushGlyphBitmapDiskCache();
	}

	bool LayoutFreeTypeRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping)
	{
		arLayout = {};
		if (!apFont || !apText || !auiLength)
			return false;
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && vectorfont::LayoutRuntimeRun(
			*runtime, apText, auiLength, abAllowShaping, arLayout);
	}

	bool GetFreeTypePairKerning(Font* apFont,
		const char* apLeft, size_t auiLeftLength,
		const char* apRight, size_t auiRightLength, float& arKerning)
	{
		arKerning = 0.0f;
		if (!apFont || !apLeft || !apRight || !auiLeftLength || !auiRightLength
			|| auiLeftLength > 2 || auiRightLength > 2)
		{
			return false;
		}
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		if (!runtime)
			return false;
		std::lock_guard<std::recursive_mutex> lock(vectorfont::s_mutex);
		auto packCode = [](const char* bytes, size_t length)
		{
			UInt16 code = static_cast<UInt8>(bytes[0]);
			if (length == 2)
				code = static_cast<UInt16>((code << 8) | static_cast<UInt8>(bytes[1]));
			return code;
		};
		const vectorfont::KerningCacheKey cacheKey = {
			runtime->config->layoutHash, runtime->config->fontId,
			g_usingWinEncoding, packCode(apLeft, auiLeftLength),
			packCode(apRight, auiRightLength),
			static_cast<UInt8>(auiLeftLength), static_cast<UInt8>(auiRightLength)
		};
		auto cached = vectorfont::s_kerningCache.find(cacheKey);
		if (cached != vectorfont::s_kerningCache.end())
		{
			arKerning = cached->second;
			vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningHit);
			return true;
		}
		auto storeResult = [&](float value)
		{
			constexpr size_t kKerningCacheLimit = 16384;
			while (vectorfont::s_kerningCache.size() >= kKerningCacheLimit
				&& !vectorfont::s_kerningCacheOrder.empty())
			{
				vectorfont::s_kerningCache.erase(
					vectorfont::s_kerningCacheOrder.front());
				vectorfont::s_kerningCacheOrder.pop_front();
			}
			vectorfont::s_kerningCache.emplace(cacheKey, value);
			vectorfont::s_kerningCacheOrder.push_back(cacheKey);
		};
		VectorEncodedGlyph left;
		VectorEncodedGlyph right;
		if (!vectorfont::DecodeEncodedGlyphIdentity(*runtime, apLeft, left)
			|| !vectorfont::DecodeEncodedGlyphIdentity(*runtime, apRight, right)
			|| left.byteLength != auiLeftLength || right.byteLength != auiRightLength
			|| left.byteClass != right.byteClass || left.faceIndex != right.faceIndex
			|| !left.glyphIndex || !right.glyphIndex)
		{
			return false;
		}
		vectorfont::RuntimeRole& role = runtime->roles[static_cast<size_t>(left.byteClass)];
		if (role.style->fixedWidth > 0.0f)
		{
			vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningMiss);
			storeResult(0.0f);
			return true;
		}
		if (left.faceIndex >= role.faces.size())
			return false;
		vectorfont::RuntimeFace& face = role.faces[left.faceIndex];
		vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningMiss);
		if (!vectorfont::ConfigureRuntimeFace(face, *role.style, 1.0f, false)
			|| !FT_HAS_KERNING(face.face))
		{
			storeResult(0.0f);
			return true;
		}
		FT_Vector delta = {};
		if (!FT_Get_Kerning(face.face, left.glyphIndex, right.glyphIndex,
			FT_KERNING_DEFAULT, &delta))
		{
			arKerning = static_cast<float>(delta.x) / 64.0f;
		}
		storeResult(arKerning);
		return true;
	}

	bool IsHarfBuzzShapingEnabled(const Font* apFont)
	{
		const vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && runtime->config && runtime->config->shaping;
	}

	bool ActivateFreeTypeFont(Font* apFont, bool abForce)
	{
		if (!apFont || !apFont->pFontData || !g_bEnableFreeTypeFontRendering)
			return false;
		if (!vectorfont::FindConfig(apFont->iFontNum))
		{
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				std::lock_guard<std::recursive_mutex> lock(vectorfont::s_mutex);
				if (vectorfont::s_loggedUnconfiguredFontIds.insert(apFont->iFontNum).second)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: font id=%d file=%s is not configured; keeping original renderer",
						apFont->iFontNum, apFont->pFontFile ? apFont->pFontFile : "");
				}
			}
			return false;
		}
		{
			std::lock_guard<std::recursive_mutex> lock(vectorfont::s_mutex);
			const auto active = vectorfont::s_activeFonts.find(apFont);
			if (!abForce && active != vectorfont::s_activeFonts.end()
				&& active->second.data == apFont->pFontData
				&& active->second.fontId == static_cast<UInt32>(apFont->iFontNum))
				return true;
		}
		if (!InitializeFreeTypeVectorRenderer())
		{
			gLog.FormattedMessage("tnvse_freetype_font: vector renderer unavailable for font id=%d",
				apFont->iFontNum);
			return false;
		}
		vectorfont::RuntimeFont* runtime = vectorfont::EnsureRuntimeFont(apFont->iFontNum);
		if (!runtime)
		{
			gLog.FormattedMessage("tnvse_freetype_font: runtime initialization failed for font id=%d",
				apFont->iFontNum);
			return false;
		}
		if (!vectorfont::ApplyRuntimeMetrics(*runtime, *apFont))
		{
			gLog.FormattedMessage("tnvse_freetype_font: metric replacement failed for font id=%d",
				apFont->iFontNum);
			return false;
		}
		gLog.FormattedMessage("tnvse_freetype_font: activated font id=%d file=%s",
			apFont->iFontNum, apFont->pFontFile ? apFont->pFontFile : "");
		vectorfont::QueueFontPrewarm(apFont->iFontNum);
		return true;
	}

	bool IsFreeTypeFontActive(const Font* apFont)
	{
		return vectorfont::FindActiveRuntime(apFont) != nullptr;
	}

	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 encodedCode)
	{
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime ? vectorfont::EnsureDoubleByteMetrics(*runtime, *apFont, encodedCode) : nullptr;
	}

	bool DecodeFreeTypeGlyph(Font* apFont, const char* text, VectorEncodedGlyph& glyph)
	{
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && vectorfont::DecodeEncodedGlyph(*runtime, *apFont, text, glyph);
	}
}
