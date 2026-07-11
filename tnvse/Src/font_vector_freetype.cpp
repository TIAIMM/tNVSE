#include "font_vector_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H
#include FT_STROKER_H

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
		constexpr size_t kMeshCacheLimit = 64u * 1024u * 1024u;
		constexpr size_t kBitmapCacheLimit = 64u * 1024u * 1024u;
		constexpr FT_Int32 kGlyphLoadFlags =
			FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
		constexpr float kFixedScale = 65536.0f;

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

			RuntimeFace() = default;
			RuntimeFace(const RuntimeFace&) = delete;
			RuntimeFace& operator=(const RuntimeFace&) = delete;
			RuntimeFace(RuntimeFace&& other) noexcept : file(std::move(other.file)), face(other.face)
			{
				other.face = nullptr;
			}
			RuntimeFace& operator=(RuntimeFace&& other) noexcept
			{
				if (this != &other)
				{
					if (face)
						FT_Done_Face(face);
					file = std::move(other.file);
					face = other.face;
					other.face = nullptr;
				}
				return *this;
			}
			~RuntimeFace()
			{
				if (face)
					FT_Done_Face(face);
			}
		};

		struct RuntimeRole
		{
			const ByteStyle* style = nullptr;
			std::vector<RuntimeFace> faces;
			float resolvedBaselineOffset = 0.0f;
			float ascender = 0.0f;
			float descender = 0.0f;
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
			UInt32 codePoint = 0;
			UInt8 byteClass = 0;
			UInt8 meshType = 0;

			bool operator==(const MeshCacheKey& other) const
			{
				return styleHash == other.styleHash
					&& fontId == other.fontId
					&& codePoint == other.codePoint
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
				result ^= static_cast<size_t>(key.codePoint) * 0x85EBCA77u;
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
			UInt32 codePoint = 0;
			UInt16 effectiveWidth = 0;
			UInt16 effectiveHeight = 0;
			SInt32 embolden26Dot6 = 0;
			SInt32 strokeWidth26Dot6 = 0;
			UInt8 byteClass = 0;
			UInt8 maskType = 0;
			UInt8 renderMode = 0;

			bool operator==(const BitmapCacheKey& other) const
			{
				return styleHash == other.styleHash
					&& fontId == other.fontId
					&& codePoint == other.codePoint
					&& effectiveWidth == other.effectiveWidth
					&& effectiveHeight == other.effectiveHeight
					&& embolden26Dot6 == other.embolden26Dot6
					&& strokeWidth26Dot6 == other.strokeWidth26Dot6
					&& byteClass == other.byteClass
					&& maskType == other.maskType
					&& renderMode == other.renderMode;
			}
		};

		struct BitmapCacheKeyHash
		{
			size_t operator()(const BitmapCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.codePoint) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.effectiveWidth) << 16;
				result ^= static_cast<size_t>(key.effectiveHeight);
				result ^= static_cast<size_t>(key.embolden26Dot6) * 0x27D4EB2Du;
				result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
				result ^= static_cast<size_t>(key.byteClass) << 8;
				result ^= key.maskType;
				result ^= static_cast<size_t>(key.renderMode) << 16;
				return result;
			}
		};

		struct BitmapCacheEntry
		{
			std::shared_ptr<GlyphBitmap> bitmap;
			size_t bytes = 0;
			std::list<BitmapCacheKey>::iterator lru;
		};

		struct ActiveFontState
		{
			const FontData* data = nullptr;
			UInt32 fontId = 0;
		};

		FT_Library s_library = nullptr;
		bool s_lcdRasterAvailable = false;
		std::unordered_map<std::wstring, std::weak_ptr<MappedFontFile>> s_mappedFiles;
		std::unordered_map<UInt32, std::unique_ptr<RuntimeFont>> s_runtimeFonts;
		std::unordered_map<const Font*, ActiveFontState> s_activeFonts;
		std::unordered_map<MeshCacheKey, MeshCacheEntry, MeshCacheKeyHash> s_meshCache;
		std::list<MeshCacheKey> s_meshLru;
		std::unordered_map<BitmapCacheKey, BitmapCacheEntry, BitmapCacheKeyHash> s_bitmapCache;
		std::list<BitmapCacheKey> s_bitmapLru;
		std::unordered_set<UInt32> s_loggedUnconfiguredFontIds;
		size_t s_meshCacheBytes = 0;
		size_t s_bitmapCacheBytes = 0;
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
			s_lcdRasterAvailable = FT_Library_SetLcdFilter(
				s_library, FT_LCD_FILTER_DEFAULT) == 0;
			if (!s_lcdRasterAvailable)
				gLog.FormattedMessage("tnvse_freetype_font: LCD filter unavailable; LCD styles use gray rendering");
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
			if (!ConfigureFace(result.face, style, 1.0f, false))
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
			if (!ConfigureFace(face.face, *role.style, 1.0f, false))
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
				if (!glyphIndex || !LoadGlyph(role, face, glyphIndex))
					continue;
				result = { &role, &face, i, glyphIndex, codePoint };
				return true;
			}
			return false;
		}

		bool ResolveGlyph(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& result)
		{
			if (ResolveExactGlyph(role, codePoint, result))
				return true;
			if (codePoint != 0xFFFD && ResolveExactGlyph(role, 0xFFFD, result))
				return true;
			if (codePoint != '?' && ResolveExactGlyph(role, '?', result))
				return true;
			if (role.faces.empty() || !LoadGlyph(role, role.faces.front(), 0))
				return false;
			result = { &role, &role.faces.front(), 0, 0, 0 };
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
				codePoint = static_cast<UInt16>(wide[0]);
				return true;
			}
			if (count == 2 && wide[0] >= 0xD800 && wide[0] <= 0xDBFF
				&& wide[1] >= 0xDC00 && wide[1] <= 0xDFFF)
			{
				codePoint = 0x10000 + ((wide[0] - 0xD800) << 10) + (wide[1] - 0xDC00);
				return true;
			}
			return false;
		}

		bool GetGlyphBox(RuntimeRole& role, UInt32 codePoint, FT_BBox& box, float& advance)
		{
			ResolvedGlyph glyph;
			if (!ResolveGlyph(role, codePoint, glyph))
				return false;
			FT_GlyphSlot slot = glyph.runtimeFace->face->glyph;
			if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
				FT_Outline_Get_CBox(&slot->outline, &box);
			else
				box = {};
			advance = static_cast<float>(slot->advance.x) / 64.0f;
			return true;
		}

		FontLetter BuildFontLetter(RuntimeRole& role, UInt32 codePoint)
		{
			FontLetter result = {};
			result.iTextureIndex = 0;
			FT_BBox box = {};
			float advance = 0.0f;
			if (!GetGlyphBox(role, codePoint, box, advance))
				return result;

			const float xMin = std::floor(static_cast<float>(box.xMin) / 64.0f);
			const float xMax = std::ceil(static_cast<float>(box.xMax) / 64.0f);
			const float yMin = std::floor(static_cast<float>(box.yMin) / 64.0f) + role.resolvedBaselineOffset;
			const float yMax = std::ceil(static_cast<float>(box.yMax) / 64.0f) + role.resolvedBaselineOffset;
			result.fLeadingEdge = xMin;
			result.fWidth = std::max(0.0f, xMax - xMin);
			result.fHeight = std::max(0.0f, yMax - yMin);
			result.fTopEdge = yMax;
			const float requestedAdvance = std::max(0.0f, advance + role.style->tracking);
			const float totalAdvance = static_cast<float>(ConditionalFloatToUInt(requestedAdvance));
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
			s_meshLru.erase(entry.lru);
			s_meshLru.push_front(key);
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
			s_bitmapLru.erase(entry.lru);
			s_bitmapLru.push_front(key);
			entry.lru = s_bitmapLru.begin();
		}

		void TrimBitmapCache()
		{
			while (s_bitmapCacheBytes > kBitmapCacheLimit && !s_bitmapLru.empty())
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
			add(&key.codePoint, sizeof(key.codePoint));
			add(&key.effectiveWidth, sizeof(key.effectiveWidth));
			add(&key.effectiveHeight, sizeof(key.effectiveHeight));
			add(&key.embolden26Dot6, sizeof(key.embolden26Dot6));
			add(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6));
			add(&key.byteClass, sizeof(key.byteClass));
			add(&key.maskType, sizeof(key.maskType));
			add(&key.renderMode, sizeof(key.renderMode));
			return hash;
		}

		bool CopyGrayBitmap(const FT_Bitmap& source, GlyphBitmap& target)
		{
			target.width = static_cast<int>(source.width);
			target.height = static_cast<int>(source.rows);
			if (target.width <= 0 || target.height <= 0)
			{
				target.width = 0;
				target.height = 0;
				return true;
			}
			if (!source.buffer)
				return false;
			target.alpha.assign(static_cast<size_t>(target.width) * target.height, 0);
			const int pitch = source.pitch;
			for (int y = 0; y < target.height; ++y)
			{
				const int sourceY = pitch >= 0 ? y : target.height - 1 - y;
				const UInt8* row = source.buffer + static_cast<ptrdiff_t>(sourceY) * std::abs(pitch);
				UInt8* output = target.alpha.data() + static_cast<size_t>(y) * target.width;
				if (source.pixel_mode == FT_PIXEL_MODE_GRAY)
				{
					if (source.num_grays == 256)
						std::copy(row, row + target.width, output);
					else
					{
						const UInt32 denominator = std::max<UInt32>(1, source.num_grays - 1);
						for (int x = 0; x < target.width; ++x)
							output[x] = static_cast<UInt8>(row[x] * 255u / denominator);
					}
				}
				else if (source.pixel_mode == FT_PIXEL_MODE_MONO)
				{
					for (int x = 0; x < target.width; ++x)
						output[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
				}
				else
				{
					return false;
				}
			}
			return true;
		}

		bool CopyLcdBitmap(const FT_Bitmap& source, GlyphBitmap& target,
			GlyphRenderMode renderMode)
		{
			if (source.pixel_mode != FT_PIXEL_MODE_LCD || source.width % 3 != 0)
				return false;
			target.width = static_cast<int>(source.width / 3);
			target.height = static_cast<int>(source.rows);
			target.renderMode = renderMode;
			if (target.width <= 0 || target.height <= 0)
			{
				target.width = 0;
				target.height = 0;
				return true;
			}
			if (!source.buffer)
				return false;
			target.lcd.assign(static_cast<size_t>(target.width) * target.height * 3, 0);
			const int pitch = source.pitch;
			for (int y = 0; y < target.height; ++y)
			{
				const int sourceY = pitch >= 0 ? y : target.height - 1 - y;
				const UInt8* row = source.buffer
					+ static_cast<ptrdiff_t>(sourceY) * std::abs(pitch);
				UInt8* output = target.lcd.data()
					+ static_cast<size_t>(y) * target.width * 3;
				for (int x = 0; x < target.width; ++x)
				{
					const UInt8* input = row + x * 3;
					if (renderMode == GlyphRenderMode::LcdBgr)
					{
						output[x * 3 + 0] = input[2];
						output[x * 3 + 1] = input[1];
						output[x * 3 + 2] = input[0];
					}
					else
					{
						output[x * 3 + 0] = input[0];
						output[x * 3 + 1] = input[1];
						output[x * 3 + 2] = input[2];
					}
				}
			}
			return true;
		}
	}

	struct RuntimeFont
	{
		const FontConfig* config = nullptr;
		std::array<RuntimeRole, 2> roles;
		float baseLine = 0.0f;
		float minBottom = 0.0f;
		float fontHeight = 0.0f;
		bool initialized = false;
	};

	namespace
	{
		std::unique_ptr<RuntimeFont> CreateRuntimeFont(const FontConfig& config)
		{
			if (!InitializeLibrary())
				return nullptr;
			auto runtime = std::make_unique<RuntimeFont>();
			runtime->config = &config;

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

			static constexpr UInt32 kSingleReferences[] = { 'H', 'M', 'W', 'A', '0', '8', 'B', 'E', 'N', 'T', 'X' };
			static constexpr UInt32 kDoubleReferences[] = { 0x4E2D, 0x56FD, 0x6F22, 0x3042, 0xAC00 };
			float singleCenter = 0.0f;
			float doubleCenter = 0.0f;
			if (MeasureVisualCenter(runtime->roles[0], kSingleReferences, std::size(kSingleReferences), singleCenter)
				&& MeasureVisualCenter(runtime->roles[1], kDoubleReferences, std::size(kDoubleReferences), doubleCenter))
			{
				const float correction = std::clamp(std::round(singleCenter - doubleCenter), -1.0f, 1.0f);
				runtime->roles[1].resolvedBaselineOffset += correction;
			}

			float maxTop = -std::numeric_limits<float>::infinity();
			float minBottom = std::numeric_limits<float>::infinity();
			for (const RuntimeRole& role : runtime->roles)
			{
				maxTop = std::max(maxTop, role.ascender + role.resolvedBaselineOffset);
				minBottom = std::min(minBottom, role.descender + role.resolvedBaselineOffset);
			}
			const float strokeWidth = std::max(
				config.glow.enabled ? config.glow.width : 0.0f,
				config.outline.enabled ? config.outline.width : 0.0f);
			if (strokeWidth > 0.0f)
			{
				maxTop += strokeWidth;
				minBottom -= strokeWidth;
			}
			if (config.shadow.enabled)
			{
				maxTop += std::max(0.0f, -config.shadow.y);
				minBottom -= std::max(0.0f, config.shadow.y);
			}
			runtime->baseLine = config.lineHeight > 0.0f ? config.lineHeight : std::max(1.0f, maxTop);
			runtime->minBottom = std::min(0.0f, minBottom);
			runtime->fontHeight = runtime->baseLine - runtime->minBottom;
			runtime->initialized = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: runtime font id=%u baseline=%.2f bottom=%.2f height=%.2f",
					config.fontId, runtime->baseLine, runtime->minBottom, runtime->fontHeight);
			}
			return runtime;
		}

		std::shared_ptr<GlyphMesh> BuildGlyphMesh(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, GlyphMeshType meshType)
		{
			auto mesh = std::make_shared<GlyphMesh>();
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			ResolvedGlyph resolved;
			if (!ResolveGlyph(role, glyph.codePoint, resolved))
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
			bitmap->renderMode = static_cast<GlyphRenderMode>(key.renderMode);
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			bitmap->baselineOffset = role.resolvedBaselineOffset;
			ResolvedGlyph resolved;
			if (!ResolveGlyph(role, glyph.codePoint, resolved))
				return nullptr;
			if (!ConfigureFace(resolved.runtimeFace->face, *role.style, rasterScale, true))
				return nullptr;

			const bool fillMask = maskType == GlyphMaskType::Fill
				|| maskType == GlyphMaskType::Shadow;
			const bool lcd = maskType == GlyphMaskType::Fill
				&& bitmap->renderMode != GlyphRenderMode::Gray;
			const FT_Int32 loadFlags = FT_LOAD_DEFAULT
				| (lcd ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL)
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

			if (fillMask)
			{
				if (FT_Render_Glyph(slot, lcd ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL))
					return nullptr;
				bitmap->left = slot->bitmap_left;
				bitmap->top = slot->bitmap_top;
				return lcd
					? (CopyLcdBitmap(slot->bitmap, *bitmap, bitmap->renderMode) ? bitmap : nullptr)
					: (CopyGrayBitmap(slot->bitmap, *bitmap) ? bitmap : nullptr);
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

	bool ApplyRuntimeMetrics(RuntimeFont& runtime, Font& font)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		if (!runtime.initialized || !font.pFontData)
			return false;

		for (UInt32 value = 0x20; value <= 0xFF; ++value)
		{
			if (value == 0x7F)
				continue;
			char byte = static_cast<char>(value);
			UInt32 codePoint = 0xFFFD;
			DecodeCodePoint(&byte, 1, codePoint);
			font.pFontData->pFontLetters[value] = BuildFontLetter(runtime.roles[0], codePoint);
		}

		font.pFontData->fBaseLine = runtime.baseLine;
		font.fMaxDrop = runtime.minBottom;
		font.fFontHeight = runtime.fontHeight;
		font.iLineOverlap = 0;
		FontLetter& space = font.pFontData->pFontLetters[' '];
		space.fHeight = runtime.fontHeight;
		space.fTopEdge = runtime.baseLine;
		font.pFontData->pFontLetters[160] = space;
		font.pFontData->pFontLetters[0].fWidth = 0.0f;
		font.pFontData->pFontLetters[0].fSpacing = 0.0f;
		font.pFontData->pFontLetters[0].fHeight = runtime.fontHeight;
		font.pFontData->pFontLetters[0].fTopEdge = runtime.baseLine;

		ExtraGlyphMap& extra = gNumberedExtraLetters[font.iFontNum];
		extra.clear();
		extra.reserve(25000);
		s_activeFonts[&font] = { font.pFontData, static_cast<UInt32>(font.iFontNum) };
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
		auto [it, inserted] = extra.emplace(encodedCode, BuildFontLetter(runtime.roles[1], codePoint));
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
			return glyph.metrics != nullptr;
		}

		glyph.encodedCode = static_cast<UInt8>(text[0]);
		glyph.byteLength = 1;
		glyph.byteClass = VectorFontByteClass::SingleByte;
		if (!DecodeCodePoint(text, 1, glyph.codePoint))
			glyph.codePoint = 0xFFFD;
		glyph.metrics = &font.pFontData->pFontLetters[static_cast<UInt8>(text[0])];
		return true;
	}

	const FontConfig& GetRuntimeConfig(const RuntimeFont& runtime)
	{
		return *runtime.config;
	}

	std::shared_ptr<const GlyphMesh> GetGlyphMesh(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMeshType meshType)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		const MeshCacheKey key = {
			runtime.config->styleHash,
			runtime.config->fontId,
			glyph.codePoint,
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
		const VectorEncodedGlyph& glyph, GlyphMaskType maskType, float rasterScale)
	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		const float safeScale = std::isfinite(rasterScale)
			&& rasterScale >= 0.1f && rasterScale <= 10.0f ? rasterScale : 1.0f;
		const ByteStyle& style = runtime.config->styles[static_cast<size_t>(glyph.byteClass)];
		GlyphRenderMode renderMode = GlyphRenderMode::Gray;
		if (maskType == GlyphMaskType::Fill && s_lcdRasterAvailable
			&& IsLcdRendererAvailable())
		{
			renderMode = style.renderMode;
		}
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
		const BitmapCacheKey key = {
			runtime.config->styleHash,
			runtime.config->fontId,
			glyph.codePoint,
			static_cast<UInt16>(effectiveWidth),
			static_cast<UInt16>(effectiveHeight),
			embolden,
			strokeWidth,
			static_cast<UInt8>(glyph.byteClass),
			static_cast<UInt8>(maskType),
			static_cast<UInt8>(renderMode)
		};
		auto existing = s_bitmapCache.find(key);
		if (existing != s_bitmapCache.end())
		{
			TouchBitmapCacheEntry(existing->second, key);
			return existing->second.bitmap;
		}

		std::shared_ptr<GlyphBitmap> bitmap = BuildGlyphBitmap(
			runtime, glyph, maskType, safeScale, key);
		if (!bitmap)
			return nullptr;
		const size_t bytes = sizeof(GlyphBitmap) + bitmap->alpha.size()
			+ bitmap->lcd.size();
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
