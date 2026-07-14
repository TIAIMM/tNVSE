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
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook::vectorfont
{
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
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 glyphIndex = 0;
			UInt16 faceIndex = 0;
			UInt8 byteClass = 0;
			UInt8 meshType = 0;

			bool operator==(const MeshCacheKey& other) const
			{
				return styleHash == other.styleHash
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
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
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
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 glyphIndex = 0;
			UInt16 faceIndex = 0;
			UInt16 effectiveWidth = 0;
			UInt16 effectiveHeight = 0;
			SInt32 embolden26Dot6 = 0;
			SInt32 strokeWidth26Dot6 = 0;
			UInt8 sdfSpread = 0;
			UInt8 byteClass = 0;
			UInt8 maskType = 0;

			bool operator==(const BitmapCacheKey& other) const
			{
				return styleHash == other.styleHash
					&& fontId == other.fontId
					&& glyphIndex == other.glyphIndex
					&& faceIndex == other.faceIndex
					&& effectiveWidth == other.effectiveWidth
					&& effectiveHeight == other.effectiveHeight
					&& embolden26Dot6 == other.embolden26Dot6
					&& strokeWidth26Dot6 == other.strokeWidth26Dot6
					&& sdfSpread == other.sdfSpread
					&& byteClass == other.byteClass
					&& maskType == other.maskType;
			}
		};

		struct BitmapCacheKeyHash
		{
			size_t operator()(const BitmapCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.faceIndex) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.effectiveWidth) << 16;
				result ^= static_cast<size_t>(key.effectiveHeight);
				result ^= static_cast<size_t>(key.embolden26Dot6) * 0x27D4EB2Du;
				result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.sdfSpread) * 0x165667B1u;
				result ^= static_cast<size_t>(key.byteClass) << 8;
				result ^= key.maskType;
				return result;
			}
		};

		struct BitmapCacheEntry
		{
			std::shared_ptr<GlyphBitmap> bitmap;
			size_t bytes = 0;
			std::list<BitmapCacheKey>::iterator lru;
		};

		struct LayoutCacheKey
		{
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 codePage = 0;
			bool allowShaping = false;
			std::string text;

			bool operator==(const LayoutCacheKey& other) const
			{
				return styleHash == other.styleHash && fontId == other.fontId
					&& codePage == other.codePage && allowShaping == other.allowShaping
					&& text == other.text;
			}
		};

		struct LayoutCacheKeyHash
		{
			size_t operator()(const LayoutCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.codePage) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.allowShaping) << 7;
				for (UInt8 value : key.text)
					result = (result ^ value) * static_cast<size_t>(16777619u);
				return result;
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
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 leftGlyph = 0;
			UInt32 rightGlyph = 0;
			UInt16 faceIndex = 0;
			UInt8 byteClass = 0;

			bool operator==(const KerningCacheKey& other) const
			{
				return styleHash == other.styleHash && fontId == other.fontId
					&& leftGlyph == other.leftGlyph && rightGlyph == other.rightGlyph
					&& faceIndex == other.faceIndex && byteClass == other.byteClass;
			}
		};

		struct KerningCacheKeyHash
		{
			size_t operator()(const KerningCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.leftGlyph) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.rightGlyph) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.faceIndex) << 8;
				result ^= key.byteClass;
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
		std::unordered_map<MeshCacheKey, MeshCacheEntry, MeshCacheKeyHash> s_meshCache;
		std::list<MeshCacheKey> s_meshLru;
		std::unordered_map<BitmapCacheKey, BitmapCacheEntry, BitmapCacheKeyHash> s_bitmapCache;
		std::list<BitmapCacheKey> s_bitmapLru;
		std::unordered_map<LayoutCacheKey, LayoutCacheEntry, LayoutCacheKeyHash> s_layoutCache;
		std::list<LayoutCacheKey> s_layoutLru;
		std::unordered_map<KerningCacheKey, float, KerningCacheKeyHash> s_kerningCache;
		std::array<UInt32, 256> s_singleByteCodePoints = {};
		std::array<UInt32, 65536> s_doubleByteCodePoints = {};
		UInt32 s_codePointCacheCodePage = UINT32_MAX;
		std::unordered_set<UInt32> s_loggedUnconfiguredFontIds;
		std::unordered_set<UInt64> s_loggedVerticalMetricRoles;
		std::unordered_set<UInt64> s_loggedHarfBuzzVerticalRoles;
		UInt32 s_shapingFallbackLogCount = 0;
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
			add(&key.styleHash, sizeof(key.styleHash));
			add(&key.fontId, sizeof(key.fontId));
			add(&key.glyphIndex, sizeof(key.glyphIndex));
			add(&key.faceIndex, sizeof(key.faceIndex));
			add(&key.effectiveWidth, sizeof(key.effectiveWidth));
			add(&key.effectiveHeight, sizeof(key.effectiveHeight));
			add(&key.embolden26Dot6, sizeof(key.embolden26Dot6));
			add(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6));
			add(&key.sdfSpread, sizeof(key.sdfSpread));
			add(&key.byteClass, sizeof(key.byteClass));
			add(&key.maskType, sizeof(key.maskType));
			return hash;
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
					// bitmap SDF renderer reports num_grays=255 while still using the full
					// 0..255 byte range, so normalizing it would turn 255 into 256 and then
					// wrap the glyph interior to zero.
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
	};

	namespace
	{
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

		hb_language_t GetLayoutLanguage()
		{
			switch (g_uiEncoding)
			{
			case 1: return hb_language_from_string("zh-Hans", -1);
			case 2: return hb_language_from_string("zh-Hant", -1);
			case 3: return hb_language_from_string("ja", -1);
			case 4: return hb_language_from_string("ko", -1);
			default: return hb_language_from_string("en", -1);
			}
		}

		hb_buffer_t* GetThreadHarfBuzzBuffer()
		{
			thread_local hb_buffer_t* buffer = hb_buffer_create();
			if (buffer)
				hb_buffer_clear_contents(buffer);
			return buffer;
		}

		bool DecodeLayoutInput(RuntimeFont& runtime, Font& font,
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
				if (!DecodeEncodedGlyph(runtime, font, encodedText, glyph)
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
			FreeTypeLayoutRun& layout)
		{
			FT_UInt previousGlyph = 0;
			RuntimeFace* previousFace = nullptr;
			VectorFontByteClass previousClass = VectorFontByteClass::SingleByte;
			for (size_t index = begin; index < end; ++index)
			{
				const LayoutInputGlyph& item = input[index];
				RuntimeRole& role = *item.resolved.role;
				RuntimeFace& face = *item.resolved.runtimeFace;
				ConfigureRuntimeFace(face, *role.style, 1.0f, false);
				const bool fixedCell = role.style->fixedWidth > 0.0f;
				float kerning = 0.0f;
				if (!fixedCell && previousFace == &face && previousClass == item.glyph.byteClass
					&& previousGlyph && item.resolved.glyphIndex
					&& FT_HAS_KERNING(face.face))
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
					? GetFixedCellAdvance(*role.style)
					: baseAdvance + role.style->tracking + kerning;
				layout.advance += positioned.xAdvance;
				layout.glyphs.push_back(std::move(positioned));
				previousGlyph = item.resolved.glyphIndex;
				previousFace = &face;
				previousClass = item.glyph.byteClass;
			}
		}

		bool AppendHarfBuzzLayout(RuntimeFont& runtime,
			const std::vector<LayoutInputGlyph>& input, size_t begin, size_t end,
			FreeTypeLayoutRun& layout)
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
			hb_buffer_guess_segment_properties(buffer);
			hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
			hb_buffer_set_language(buffer, GetLayoutLanguage());

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

			for (unsigned int glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex)
			{
				const UInt32 cluster = infos[glyphIndex].cluster;
				size_t sourceIndex = begin;
				for (size_t candidate = begin; candidate < end; ++candidate)
				{
					if (input[candidate].byteOffset > cluster)
						break;
					sourceIndex = candidate;
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
				layout.glyphs.push_back(std::move(positioned));
			}
			layout.shaped = true;
			return true;
		}

		bool BuildLayoutRun(RuntimeFont& runtime, Font& font, const char* text,
			size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
		{
			layout = {};
			std::vector<LayoutInputGlyph> input;
			if (!DecodeLayoutInput(runtime, font, text, length, input))
				return false;
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
					const size_t glyphStart = layout.glyphs.size();
					const float advanceStart = layout.advance;
					if (!AppendHarfBuzzLayout(runtime, input, begin, end, layout))
					{
						if (g_bEnableFreeTypeFontRenderingLog && s_shapingFallbackLogCount < 32)
						{
							++s_shapingFallbackLogCount;
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: HarfBuzz run failed font=%u units=%u; using precise FreeType kerning",
								runtime.config->fontId, static_cast<UInt32>(end - begin));
						}
						layout.glyphs.resize(glyphStart);
						layout.advance = advanceStart;
						AppendPreciseLayout(runtime, input, begin, end, layout);
					}
				}
				else
				{
					AppendPreciseLayout(runtime, input, begin, end, layout);
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

		std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
			float rasterScale, const BitmapCacheKey& key)
		{
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			bitmap->baselineOffset = role.resolvedBaselineOffset;
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
				if (key.sdfSpread < 2 || key.sdfSpread > 32
					|| FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
					return nullptr;
				FT_Int spread = key.sdfSpread;
				if (FT_Property_Set(s_library, "bsdf", "spread", &spread)
					|| FT_Render_Glyph(slot, FT_RENDER_MODE_SDF))
					return nullptr;
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
			char byte = static_cast<char>(value);
			UInt32 codePoint = 0xFFFD;
			DecodeCodePoint(&byte, 1, codePoint);
			font.pFontData->pFontLetters[value] = BuildFontLetter(
				runtime.roles[0], *runtime.config,
				VectorFontByteClass::SingleByte, codePoint);
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

		const char bytes[2] = {
			static_cast<char>((encodedCode >> 8) & 0xFF),
			static_cast<char>(encodedCode & 0xFF)
		};
		UInt32 codePoint = 0xFFFD;
		DecodeCodePoint(bytes, 2, codePoint);
		auto [it, inserted] = extra.emplace(encodedCode,
			BuildFontLetter(runtime.roles[1], *runtime.config,
				VectorFontByteClass::DoubleByte, codePoint));
		return &it->second;
	}

	bool DecodeEncodedGlyph(RuntimeFont& runtime, Font& font, const char* text, VectorEncodedGlyph& glyph)
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
			glyph.metrics = EnsureDoubleByteMetrics(runtime, font, encodedCode);
			ResolvedGlyph resolved;
			if (ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
				glyph.codePoint, resolved))
			{
				ApplyResolvedIdentity(glyph, resolved);
			}
			return glyph.metrics != nullptr;
		}

		glyph.encodedCode = static_cast<UInt8>(text[0]);
		glyph.byteLength = 1;
		glyph.byteClass = VectorFontByteClass::SingleByte;
		if (!DecodeCodePoint(text, 1, glyph.codePoint))
			glyph.codePoint = 0xFFFD;
		glyph.metrics = &font.pFontData->pFontLetters[static_cast<UInt8>(text[0])];
		ResolvedGlyph resolved;
		if (ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
			glyph.codePoint, resolved))
		{
			ApplyResolvedIdentity(glyph, resolved);
		}
		return true;
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
		if (!DecodeCodePoint(bytes, static_cast<int>(length), glyph.codePoint))
			return false;

		ResolvedGlyph resolved;
		if (!ResolveGlyph(runtime.roles[static_cast<size_t>(glyph.byteClass)],
			glyph.codePoint, resolved))
		{
			return false;
		}
		ApplyResolvedIdentity(glyph, resolved);
		return glyph.hasGlyphIdentity;
	}

	const FontConfig& GetRuntimeConfig(const RuntimeFont& runtime)
	{
		return *runtime.config;
	}

	void HydrateLayoutMetrics(RuntimeFont& runtime, Font& font,
		FreeTypeLayoutRun& layout)
	{
		for (FreeTypeLayoutGlyph& positioned : layout.glyphs)
		{
			VectorEncodedGlyph& glyph = positioned.glyph;
			if (glyph.byteClass == VectorFontByteClass::DoubleByte)
				glyph.metrics = EnsureDoubleByteMetrics(runtime, font, glyph.encodedCode);
			else
				glyph.metrics = &font.pFontData->pFontLetters[glyph.encodedCode & 0xFF];
		}
	}

	bool LayoutRuntimeRun(RuntimeFont& runtime, Font& font, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		LayoutCacheKey key = {
			runtime.config->styleHash,
			runtime.config->fontId,
			g_usingWinEncoding,
			allowShaping,
			std::string(text, length)
		};
		auto existing = s_layoutCache.find(key);
		if (existing != s_layoutCache.end())
		{
			TouchLayoutCacheEntry(existing->second);
			layout = existing->second.layout;
			HydrateLayoutMetrics(runtime, font, layout);
			RecordFreeTypePerf(FreeTypePerfCounter::LayoutHit);
			return true;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::LayoutMiss);
		if (!BuildLayoutRun(runtime, font, text, length, allowShaping, layout))
			return false;
		if (length <= 65536)
		{
			FreeTypeLayoutRun cachedLayout = layout;
			for (FreeTypeLayoutGlyph& glyph : cachedLayout.glyphs)
				glyph.glyph.metrics = nullptr;
			const size_t bytes = sizeof(LayoutCacheEntry) + key.text.size()
				+ layout.glyphs.size() * sizeof(FreeTypeLayoutGlyph);
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
		const MeshCacheKey key = {
			runtime.config->styleHash,
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
		const ByteStyle& style = runtime.config->styles[static_cast<size_t>(glyph.byteClass)];
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
		const BitmapCacheKey key = {
			runtime.config->styleHash,
			runtime.config->fontId,
			glyph.glyphIndex,
			glyph.faceIndex,
			static_cast<UInt16>(effectiveWidth),
			static_cast<UInt16>(effectiveHeight),
			embolden,
			strokeWidth,
			resolvedSdfSpread,
			static_cast<UInt8>(glyph.byteClass),
			static_cast<UInt8>(maskType)
		};
		auto existing = s_bitmapCache.find(key);
		if (existing != s_bitmapCache.end())
		{
			TouchBitmapCacheEntry(existing->second, key);
			RecordFreeTypePerf(FreeTypePerfCounter::BitmapMemoryHit);
			return existing->second.bitmap;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::BitmapRasterized);
		std::shared_ptr<GlyphBitmap> bitmap =
			BuildGlyphBitmap(runtime, glyph, maskType, safeScale, key);
		if (!bitmap)
			return nullptr;
		const size_t bytes = sizeof(GlyphBitmap) + bitmap->alpha.size();
		s_bitmapLru.push_front(key);
		s_bitmapCache.emplace(key,
			BitmapCacheEntry{ bitmap, bytes, s_bitmapLru.begin() });
		s_bitmapCacheBytes += bytes;
		TrimBitmapCache();
		return bitmap;
	}
}

namespace fonthook
{
	bool LayoutFreeTypeRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping)
	{
		arLayout = {};
		if (!apFont || !apText || !auiLength || !IsFreeTypeFontActive(apFont))
			return false;
		vectorfont::RuntimeFont* runtime = vectorfont::FindRuntimeFont(apFont->iFontNum);
		return runtime && vectorfont::LayoutRuntimeRun(
			*runtime, *apFont, apText, auiLength, abAllowShaping, arLayout);
	}

	bool GetFreeTypePairKerning(Font* apFont,
		const char* apLeft, size_t auiLeftLength,
		const char* apRight, size_t auiRightLength, float& arKerning)
	{
		arKerning = 0.0f;
		if (!apFont || !apLeft || !apRight || !auiLeftLength || !auiRightLength
			|| !IsFreeTypeFontActive(apFont))
		{
			return false;
		}
		std::lock_guard<std::recursive_mutex> lock(vectorfont::s_mutex);
		vectorfont::RuntimeFont* runtime = vectorfont::FindRuntimeFont(apFont->iFontNum);
		if (!runtime)
			return false;
		VectorEncodedGlyph left;
		VectorEncodedGlyph right;
		if (!vectorfont::DecodeEncodedGlyph(*runtime, *apFont, apLeft, left)
			|| !vectorfont::DecodeEncodedGlyph(*runtime, *apFont, apRight, right)
			|| left.byteLength != auiLeftLength || right.byteLength != auiRightLength
			|| left.byteClass != right.byteClass || left.faceIndex != right.faceIndex
			|| !left.glyphIndex || !right.glyphIndex)
		{
			return false;
		}
		vectorfont::RuntimeRole& role = runtime->roles[static_cast<size_t>(left.byteClass)];
		if (role.style->fixedWidth > 0.0f)
			return true;
		if (left.faceIndex >= role.faces.size())
			return false;
		vectorfont::RuntimeFace& face = role.faces[left.faceIndex];
		const vectorfont::KerningCacheKey cacheKey = {
			runtime->config->styleHash, runtime->config->fontId,
			left.glyphIndex, right.glyphIndex, left.faceIndex,
			static_cast<UInt8>(left.byteClass)
		};
		auto cached = vectorfont::s_kerningCache.find(cacheKey);
		if (cached != vectorfont::s_kerningCache.end())
		{
			arKerning = cached->second;
			vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningHit);
			return true;
		}
		vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningMiss);
		if (!vectorfont::ConfigureRuntimeFace(face, *role.style, 1.0f, false)
			|| !FT_HAS_KERNING(face.face))
		{
			vectorfont::s_kerningCache.emplace(cacheKey, 0.0f);
			return true;
		}
		FT_Vector delta = {};
		if (!FT_Get_Kerning(face.face, left.glyphIndex, right.glyphIndex,
			FT_KERNING_DEFAULT, &delta))
		{
			arKerning = static_cast<float>(delta.x) / 64.0f;
		}
		if (vectorfont::s_kerningCache.size() >= 16384)
			vectorfont::s_kerningCache.clear();
		vectorfont::s_kerningCache.emplace(cacheKey, arKerning);
		return true;
	}

	bool IsHarfBuzzShapingEnabled(const Font* apFont)
	{
		if (!IsFreeTypeFontActive(apFont))
			return false;
		const vectorfont::FontConfig* config = vectorfont::FindConfig(apFont->iFontNum);
		return config && config->shaping;
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
		if (!apFont || !g_bEnableFreeTypeFontRendering)
			return false;
		std::lock_guard<std::recursive_mutex> lock(vectorfont::s_mutex);
		const auto active = vectorfont::s_activeFonts.find(apFont);
		return active != vectorfont::s_activeFonts.end()
			&& active->second.data == apFont->pFontData
			&& active->second.fontId == static_cast<UInt32>(apFont->iFontNum);
	}

	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 encodedCode)
	{
		if (!IsFreeTypeFontActive(apFont))
			return nullptr;
		vectorfont::RuntimeFont* runtime = vectorfont::FindRuntimeFont(apFont->iFontNum);
		return runtime ? vectorfont::EnsureDoubleByteMetrics(*runtime, *apFont, encodedCode) : nullptr;
	}

	bool DecodeFreeTypeGlyph(Font* apFont, const char* text, VectorEncodedGlyph& glyph)
	{
		if (!IsFreeTypeFontActive(apFont))
			return false;
		vectorfont::RuntimeFont* runtime = vectorfont::FindRuntimeFont(apFont->iFontNum);
		return runtime && vectorfont::DecodeEncodedGlyph(*runtime, *apFont, text, glyph);
	}
}
