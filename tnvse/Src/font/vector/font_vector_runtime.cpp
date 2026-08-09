#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_vector_runtime {}
	using namespace implementation::font_vector_runtime;

	namespace implementation::font_vector_runtime
	{
		FreeTypeState s_freeTypeState;
		thread_local FreeTypeThreadState s_freeTypeThreadState;
	}

	FreeTypeState& State()
	{
		return s_freeTypeState;
	}

	FreeTypeThreadState& ThreadState()
	{
		return s_freeTypeThreadState;
	}

	size_t GetBitmapCacheLimit()
	{
		const size_t configuredBytes = static_cast<size_t>(
			g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u;
		const size_t prewarmLimit = configuredBytes / 4u;
		if (!State().bitmapCacheReducedAfterPrewarm)
			return prewarmLimit;

		// Full atlas profiles no longer need a large CPU bitmap working set. Keep
		// an adaptive 8-16 MiB demand cache for cold misses without reducing the
		// unified text-artifact budget that affects every frame.
		constexpr size_t kMinimumPostPrewarmBitmapBytes = 8u * 1024u * 1024u;
		constexpr size_t kMaximumPostPrewarmBitmapBytes = 16u * 1024u * 1024u;
		const size_t adaptiveLimit = std::clamp(configuredBytes / 16u,
			kMinimumPostPrewarmBitmapBytes, kMaximumPostPrewarmBitmapBytes);
		return std::min(prewarmLimit, adaptiveLimit);
	}

		std::wstring NormalizePathKey(std::wstring path)
		{
			std::replace(path.begin(), path.end(), L'/', L'\\');
			std::transform(path.begin(), path.end(), path.begin(), towlower);
			return path;
		}

		UInt64 HashBytes64(const void* data, size_t size,
			UInt64 hash)
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
			auto existing = State().mappedFiles.find(key);
			if (existing != State().mappedFiles.end())
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
			mapped->cpuMemory.Reset(CpuMemoryCategory::PersistentMapping,
				sizeof(MappedFontFile) + mapped->path.capacity() * sizeof(wchar_t)
					+ static_cast<size_t>(mapped->size));

			State().mappedFiles[key] = mapped;
			return mapped;
		}

		bool InitializeLibrary()
		{
			if (State().library)
				return true;
			if (FT_Init_FreeType(&State().library))
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
			if (!ConfigureFace(runtimeFace.ftFace, style, safeScale, raster))
				return false;
			runtimeFace.configured = true;
			runtimeFace.configuredRaster = raster;
			runtimeFace.configuredWidth = width;
			runtimeFace.configuredHeight = height;
			return true;
		}

		bool CreateRuntimeFace(const FaceConfig& config, const ByteStyle& style, RuntimeFace& result)
		{
			result.file = MapFontFile(config.path);
			if (!result.file)
				return false;
			FT_Error error = FT_New_Memory_Face(State().library, result.file->data,
				result.file->size, config.faceIndex, &result.ftFace);
			if (error)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: FT_New_Memory_Face failed path=%ls index=%ld error=0x%02X",
					config.path.c_str(), config.faceIndex, static_cast<UInt32>(error));
				return false;
			}
			error = FT_Select_Charmap(result.ftFace, FT_ENCODING_UNICODE);
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
					result.ftFace->family_name ? result.ftFace->family_name : "",
					result.ftFace->style_name ? result.ftFace->style_name : "",
					result.ftFace->num_faces, result.ftFace->num_glyphs);
			}
			return true;
		}

		bool EnsureRuntimeFaceLoaded(RuntimeRole& role,
			RuntimeFace& runtimeFace)
		{
			if (runtimeFace.ftFace && runtimeFace.file)
				return true;
			if (!role.style
				|| runtimeFace.sourceConfigIndex
					>= role.style->faces.size()
				|| !InitializeLibrary())
			{
				return false;
			}
			const float baseline =
				runtimeFace.resolvedBaselineOffset;
			const float visualCorrection =
				runtimeFace.visualCenterCorrection;
			const UInt16 sourceIndex =
				runtimeFace.sourceConfigIndex;
			RuntimeFace loaded;
			loaded.sourceConfigIndex = sourceIndex;
			if (!CreateRuntimeFace(
				role.style->faces[sourceIndex], *role.style, loaded))
			{
				return false;
			}
			loaded.resolvedBaselineOffset = baseline;
			loaded.visualCenterCorrection = visualCorrection;
			runtimeFace = std::move(loaded);
			return true;
		}

		bool LoadGlyph(RuntimeRole& role, RuntimeFace& face, FT_UInt glyphIndex)
		{
			if (!EnsureRuntimeFaceLoaded(role, face))
				return false;
			if (!ConfigureRuntimeFace(face, *role.style, 1.0f, false))
				return false;
			if (FT_Load_Glyph(face.ftFace, glyphIndex, kGlyphLoadFlags))
				return false;
			if (face.ftFace->glyph->format == FT_GLYPH_FORMAT_OUTLINE && role.style->embolden > 0.0f)
			{
				const FT_Pos strength = static_cast<FT_Pos>(std::lround(role.style->embolden * 64.0f));
				FT_Outline_EmboldenXY(&face.ftFace->glyph->outline, strength, strength);
			}
			const size_t previousBytes = face.directLayoutMetrics.GetAllocatedBytes();
			if (DirectLayoutGlyphMetric* metric =
				face.directLayoutMetrics.GetOrCreate(glyphIndex))
			{
				metric->advance = static_cast<float>(face.ftFace->glyph->advance.x) / 64.0f;
				metric->fixedOffset = GetFixedCellGlyphOffset(
					*role.style, face.ftFace->glyph);
				metric->valid = true;
				const size_t allocatedBytes =
					face.directLayoutMetrics.GetAllocatedBytes();
				if (allocatedBytes != previousBytes)
				{
					face.directLayoutMetricMemory.Reset(
						CpuMemoryCategory::RuntimeMetadata, allocatedBytes);
				}
			}
			return true;
		}

		bool ResolveExactGlyph(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& result)
		{
			for (UInt32 i = 0; i < role.faces.size(); ++i)
			{
				RuntimeFace& face = role.faces[i];
				if (!EnsureRuntimeFaceLoaded(role, face))
					continue;
				const FT_UInt glyphIndex = FT_Get_Char_Index(face.ftFace, codePoint);
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
			if (cached != role.glyphIdentities.end()
				&& cached->second.faceIndex < role.faces.size())
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
			const auto inserted = role.glyphIdentities.emplace(codePoint,
				CachedGlyphIdentity{ static_cast<UInt16>(result.faceIndex),
					result.glyphIndex, result.renderedCodePoint });
			if (inserted.second && role.owner)
			{
				role.owner->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
					role.owner->cpuMemory.GetBytes()
						+ sizeof(std::pair<const UInt32, CachedGlyphIdentity>)
						+ 3u * sizeof(void*));
			}
			return true;
		}

		bool DecodeCodePoint(const char* bytes, int length, UInt32& codePoint)
		{
			if (!bytes || length <= 0)
				return false;
			const UInt32 codePage = GetFreeTypeTextCodePage();

			FreeTypeState& state = State();
			if (state.codePointCacheCodePage != codePage)
			{
				state.singleByteCodePoints.fill(UINT32_MAX);
				state.doubleByteCodePoints.Clear();
				state.codePointCacheMemory.Release();
				state.codePointCacheCodePage = codePage;
			}
			const UInt32 encoded = length == 1
				? static_cast<UInt8>(bytes[0])
				: (static_cast<UInt32>(static_cast<UInt8>(bytes[0])) << 8)
					| static_cast<UInt8>(bytes[1]);
			UInt32* cached = length == 1
				? &state.singleByteCodePoints[encoded]
				: state.doubleByteCodePoints.GetOrCreate(
					static_cast<UInt16>(encoded));
			if (cached && length != 1)
			{
				const size_t allocatedBytes =
					state.doubleByteCodePoints.GetAllocatedBytes();
				if (state.codePointCacheMemory.GetBytes() != allocatedBytes)
				{
					state.codePointCacheMemory.Reset(
						CpuMemoryCategory::RuntimeMetadata, allocatedBytes);
				}
			}
			if (cached && *cached != UINT32_MAX)
			{
				if (*cached == UINT32_MAX - 1)
					return false;
				codePoint = *cached;
				return true;
			}

			wchar_t wide[2] = {};
			int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
				bytes, length, wide, static_cast<int>(std::size(wide)));
			if (!count && GetLastError() == ERROR_INVALID_FLAGS)
			{
				count = MultiByteToWideChar(codePage, 0,
					bytes, length, wide, static_cast<int>(std::size(wide)));
			}
			if (count == 1)
			{
				codePoint = static_cast<UInt16>(wide[0]);
				if (cached)
					*cached = codePoint;
				return true;
			}
			if (count == 2 && wide[0] >= 0xD800 && wide[0] <= 0xDBFF
				&& wide[1] >= 0xDC00 && wide[1] <= 0xDFFF)
			{
				codePoint = 0x10000 + ((wide[0] - 0xD800) << 10) + (wide[1] - 0xDC00);
				if (cached)
					*cached = codePoint;
				return true;
			}
			if (cached)
				*cached = UINT32_MAX - 1;
			return false;
		}

		bool GetGlyphBox(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& glyph,
			FT_BBox& box, float& advance)
		{
			if (!ResolveGlyph(role, codePoint, glyph))
				return false;
			if (!LoadGlyph(role, *glyph.runtimeFace, glyph.glyphIndex))
				return false;
			FT_GlyphSlot slot = glyph.runtimeFace->ftFace->glyph;
			if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
				FT_Outline_Get_CBox(&slot->outline, &box);
			else
				box = {};
			advance = static_cast<float>(slot->advance.x) / 64.0f;
			return true;
		}

		float GetResolvedBaselineOffset(const RuntimeRole& role,
			const ResolvedGlyph& glyph)
		{
			return glyph.runtimeFace
				? glyph.runtimeFace->resolvedBaselineOffset
				: role.resolvedBaselineOffset;
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
			float shadowRadius = config.shadow.enabled
				? std::max(0.0f, config.shadow.blur) : 0.0f;
			if (HardShadowIncludesGlow(config))
				shadowRadius = std::max(shadowRadius, config.glow.outer);
			if (HardShadowIncludesOutline(config))
			{
				shadowRadius = std::max(shadowRadius,
					config.outline.width + config.outline.softness);
			}
			const float shadowTop = config.shadow.enabled
				? std::max(0.0f, shadowRadius - config.shadow.y) : 0.0f;
			const float shadowBottom = config.shadow.enabled
				? std::max(0.0f, shadowRadius + config.shadow.y) : 0.0f;
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
			ResolvedGlyph resolved;
			FT_BBox box = {};
			float advance = 0.0f;
			if (!GetGlyphBox(role, codePoint, resolved, box, advance))
				return result;
			const float baselineOffset = GetResolvedBaselineOffset(role, resolved);

			const float xMin = std::floor(static_cast<float>(box.xMin) / 64.0f);
			const float xMax = std::ceil(static_cast<float>(box.xMax) / 64.0f);
			const float bodyBottom = std::floor(static_cast<float>(box.yMin) / 64.0f)
				+ baselineOffset;
			const float bodyTop = std::ceil(static_cast<float>(box.yMax) / 64.0f)
				+ baselineOffset;
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
				if (State().loggedVerticalMetricRoles.insert(logKey).second)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: glyph metrics font=%u role=%s face=%u codepoint=U+%04X bodyTop=%.2f bodyBottom=%.2f effectTop=%.2f effectBottom=%.2f topEdge=%.2f height=%.2f drop=%.2f configuredBaselineOffset=%.2f visualCorrection=%.3f resolvedBaselineOffset=%.3f",
						config.fontId,
						byteClass == VectorFontByteClass::DoubleByte ? "doubleByte" : "singleByte",
						resolved.faceIndex, codePoint, bodyTop, bodyBottom, effects.top, effects.bottom,
						result.fTopEdge, result.fHeight,
						result.fHeight - result.fTopEdge, role.style->baselineOffset,
						resolved.runtimeFace ? resolved.runtimeFace->visualCenterCorrection
							: role.visualCenterCorrection,
						baselineOffset);
				}
			}
			return result;
		}

		struct VisualReferenceSet
		{
			const UInt32* codePoints = nullptr;
			size_t count = 0;
			const char* name = "unknown";
		};

		struct VisualCenterMeasurement
		{
			float center = 0.0f;
			float top = 0.0f;
			float bottom = 0.0f;
			UInt32 sampleCount = 0;
		};

		float RobustAverage(std::vector<float>& values)
		{
			std::sort(values.begin(), values.end());
			const size_t trim = values.size() >= 5 ? values.size() / 5 : 0;
			const size_t begin = trim;
			const size_t end = values.size() - trim;
			double total = 0.0;
			for (size_t index = begin; index < end; ++index)
				total += values[index];
			return static_cast<float>(total / static_cast<double>(end - begin));
		}

		bool BitmapRowHasVisibleInk(const FT_Bitmap& bitmap, UInt32 row)
		{
			if (!bitmap.buffer || row >= bitmap.rows || !bitmap.width || !bitmap.pitch)
				return false;
			const ptrdiff_t pitch = bitmap.pitch;
			const UInt8* rowData = pitch > 0
				? bitmap.buffer + static_cast<ptrdiff_t>(row) * pitch
				: bitmap.buffer + static_cast<ptrdiff_t>(bitmap.rows - row - 1) * -pitch;
			switch (bitmap.pixel_mode)
			{
			case FT_PIXEL_MODE_GRAY:
			{
				const UInt8 threshold = static_cast<UInt8>(std::max<UInt32>(1,
					bitmap.num_grays > 1 ? (bitmap.num_grays - 1) / 16 : 1));
				for (UInt32 column = 0; column < bitmap.width; ++column)
				{
					if (rowData[column] >= threshold)
						return true;
				}
				return false;
			}
			case FT_PIXEL_MODE_MONO:
				for (UInt32 column = 0; column < (bitmap.width + 7) / 8; ++column)
				{
					if (rowData[column])
						return true;
				}
				return false;
			case FT_PIXEL_MODE_BGRA:
				for (UInt32 column = 0; column < bitmap.width; ++column)
				{
					if (rowData[column * 4 + 3] >= 16)
						return true;
				}
				return false;
			default:
				return false;
			}
		}

		bool MeasureFaceVisualCenter(RuntimeRole& role, RuntimeFace& face,
			const VisualReferenceSet& references, float rasterScale,
			VisualCenterMeasurement& measurement)
		{
			const float safeScale = std::isfinite(rasterScale)
				&& rasterScale >= 0.1f && rasterScale <= 10.0f ? rasterScale : 1.0f;
			if (!ConfigureRuntimeFace(face, *role.style, safeScale, true))
				return false;

			std::vector<float> centers;
			std::vector<float> tops;
			std::vector<float> bottoms;
			centers.reserve(references.count);
			tops.reserve(references.count);
			bottoms.reserve(references.count);
			for (size_t index = 0; index < references.count; ++index)
			{
				const FT_UInt glyphIndex = FT_Get_Char_Index(face.ftFace,
					references.codePoints[index]);
				if (!glyphIndex || FT_Load_Glyph(face.ftFace, glyphIndex,
					kGlyphLoadFlags | FT_LOAD_TARGET_NORMAL))
				{
					continue;
				}
				FT_GlyphSlot slot = face.ftFace->glyph;
				if (slot->format != FT_GLYPH_FORMAT_OUTLINE || !slot->outline.n_points)
					continue;
				if (role.style->embolden > 0.0f)
				{
					const FT_Pos strength = static_cast<FT_Pos>(std::lround(
						role.style->embolden * safeScale * 64.0f));
					FT_Outline_EmboldenXY(&slot->outline, strength, strength);
				}
				if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
					continue;

				UInt32 firstRow = slot->bitmap.rows;
				UInt32 lastRow = 0;
				for (UInt32 row = 0; row < slot->bitmap.rows; ++row)
				{
					if (!BitmapRowHasVisibleInk(slot->bitmap, row))
						continue;
					firstRow = std::min(firstRow, row);
					lastRow = std::max(lastRow, row);
				}
				if (firstRow >= slot->bitmap.rows)
					continue;

				const float top = (static_cast<float>(slot->bitmap_top)
					- static_cast<float>(firstRow)) / safeScale;
				const float bottom = (static_cast<float>(slot->bitmap_top)
					- static_cast<float>(lastRow + 1)) / safeScale;
				tops.push_back(top);
				bottoms.push_back(bottom);
				centers.push_back((top + bottom) * 0.5f);
			}
			if (centers.size() < 3)
				return false;

			measurement.center = RobustAverage(centers);
			measurement.top = RobustAverage(tops);
			measurement.bottom = RobustAverage(bottoms);
			measurement.sampleCount = static_cast<UInt32>(centers.size());
			return true;
		}

		VisualReferenceSet GetSingleByteVisualReferences()
		{
			static constexpr UInt32 kReferences[] = {
				'H', 'I', 'M', 'N', 'A', 'B', 'D', 'E', 'F', 'L', 'T', 'W', 'X', 'Z',
				'0', '1', '2', '3', '5', '6', '8', '9'
			};
			return { kReferences, std::size(kReferences), "latin-cap" };
		}

		VisualReferenceSet GetDoubleByteVisualReferences()
		{
			static constexpr UInt32 kSimplifiedChinese[] = {
				0x4E2D, 0x56FD, 0x6C49, 0x6587, 0x5B57, 0x5929, 0x5730, 0x6C38,
				0x7530, 0x65E5, 0x76EE, 0x56DE, 0x6B63, 0x9AD8, 0x8BED, 0x4EBA
			};
			static constexpr UInt32 kTraditionalChinese[] = {
				0x4E2D, 0x570B, 0x6F22, 0x6587, 0x5B57, 0x5929, 0x5730, 0x6C38,
				0x7530, 0x65E5, 0x76EE, 0x56DE, 0x6B63, 0x9AD4, 0x8A9E, 0x4EBA
			};
			static constexpr UInt32 kJapanese[] = {
				0x65E5, 0x672C, 0x8A9E, 0x4E2D, 0x56FD, 0x6F22, 0x6587, 0x7530,
				0x3042, 0x304B, 0x3055, 0x306A, 0x30A2, 0x30AB, 0x30B5, 0x30CA
			};
			static constexpr UInt32 kKorean[] = {
				0xD55C, 0xAE00, 0xAC00, 0xB098, 0xB2E4, 0xB77C,
				0xB9C8, 0xBC14, 0xC0AC, 0xC544, 0xC790, 0xCC28
			};
			switch (GetFreeTypeTextCodePage())
			{
			case 932:
				return { kJapanese, std::size(kJapanese), "japanese" };
			case 949:
				return { kKorean, std::size(kKorean), "korean" };
			case 950:
				return { kTraditionalChinese, std::size(kTraditionalChinese), "traditional-chinese" };
			case 936:
			default:
				return { kSimplifiedChinese, std::size(kSimplifiedChinese), "simplified-chinese" };
			}
		}

		void ApplyAutomaticVisualAlignment(RuntimeFont& runtime, float rasterScale)
		{
			// Fallout treats FontData::fBaseLine as the shared line rise and derives a
			// character's drop from FontLetter::fHeight - fTopEdge. Keep the visual
			// correction face-local so BuildFontLetter and atlas placement move together;
			// the existing verticalMetrics policy remains the sole owner of the line box.
			RuntimeRole& single = runtime.roles[0];
			RuntimeRole& doubleByte = runtime.roles[1];
			const VisualReferenceSet singleReferences = GetSingleByteVisualReferences();
			const VisualReferenceSet doubleReferences = GetDoubleByteVisualReferences();
			VisualCenterMeasurement singlePrimary;
			VisualCenterMeasurement doublePrimary;
			if (!MeasureFaceVisualCenter(single, single.faces.front(), singleReferences,
				rasterScale, singlePrimary)
				|| !MeasureFaceVisualCenter(doubleByte, doubleByte.faces.front(),
					doubleReferences, rasterScale, doublePrimary))
			{
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: vertical alignment unavailable font=%u scale=%.3f singleSamples=%u doubleSamples=%u",
						runtime.config->fontId, rasterScale,
						singlePrimary.sampleCount, doublePrimary.sampleCount);
				}
				return;
			}

			const float targetCenter = singlePrimary.center;
			for (size_t roleIndex = 0; roleIndex < runtime.roles.size(); ++roleIndex)
			{
				RuntimeRole& role = runtime.roles[roleIndex];
				const VisualReferenceSet& references = roleIndex == 0
					? singleReferences : doubleReferences;
				const float primaryCorrection = roleIndex == 0
					? 0.0f : targetCenter - doublePrimary.center;
				for (size_t faceIndex = 0; faceIndex < role.faces.size(); ++faceIndex)
				{
					RuntimeFace& face = role.faces[faceIndex];
					VisualCenterMeasurement measured;
					const VisualReferenceSet* measuredReferences = &references;
					bool exact = false;
					if (faceIndex == 0)
					{
						measured = roleIndex == 0 ? singlePrimary : doublePrimary;
						exact = true;
					}
					else
					{
						exact = MeasureFaceVisualCenter(
							role, face, references, rasterScale, measured);
						if (!exact && roleIndex == 1)
						{
							// A symbol fallback often has no CJK calibration glyphs. Its
							// Latin cap box is still a better face-local anchor than blindly
							// inheriting the primary CJK face's correction.
							measuredReferences = &singleReferences;
							exact = MeasureFaceVisualCenter(role, face,
								singleReferences, rasterScale, measured);
						}
					}
					const float rawCorrection = exact
						? targetCenter - measured.center : primaryCorrection;
					const float visualHeight = std::max(single.style->pixelSize * single.style->scaleY,
						role.style->pixelSize * role.style->scaleY);
					const float correctionLimit = std::max(2.0f, visualHeight * 0.25f);
					const float correction = std::clamp(rawCorrection,
						-correctionLimit, correctionLimit);
					face.visualCenterCorrection = correction;
					face.resolvedBaselineOffset = role.style->baselineOffset + correction;
					if (g_bEnableFreeTypeFontRenderingLog)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: vertical alignment font=%u role=%s face=%u references=%s source=%s samples=%u scale=%.3f center=%.3f top=%.3f bottom=%.3f target=%.3f rawCorrection=%.3f appliedCorrection=%.3f configuredOffset=%.3f resolvedOffset=%.3f limit=%.3f",
							runtime.config->fontId, roleIndex == 0 ? "singleByte" : "doubleByte",
							static_cast<UInt32>(faceIndex), measuredReferences->name,
							exact ? "raster-ink" : "primary-fallback",
							exact ? measured.sampleCount : 0, rasterScale,
							exact ? measured.center : 0.0f,
							exact ? measured.top : 0.0f,
							exact ? measured.bottom : 0.0f,
							targetCenter, rawCorrection, correction,
							role.style->baselineOffset, face.resolvedBaselineOffset,
							correctionLimit);
					}
				}
				role.visualCenterCorrection = role.faces.front().visualCenterCorrection;
				role.resolvedBaselineOffset = role.faces.front().resolvedBaselineOffset;
			}
		}

	bool ResolveVectorGlyph(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
		ResolvedGlyph& result)
	{
		RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
		if (glyph.hasGlyphIdentity && glyph.faceIndex < role.faces.size())
		{
			RuntimeFace& face = role.faces[glyph.faceIndex];
			if (!EnsureRuntimeFaceLoaded(role, face))
				return false;
			result = { &role, &face, glyph.faceIndex, glyph.glyphIndex, glyph.codePoint };
			return true;
		}
		return ResolveGlyph(role, glyph.codePoint, result);
	}

		std::unique_ptr<RuntimeFont> CreateRuntimeFont(const FontConfig& config)
		{
			if (!InitializeLibrary())
				return nullptr;
			auto runtime = std::make_unique<RuntimeFont>();
			runtime->config = &config;
			for (RuntimeRole& role : runtime->roles)
				role.owner = runtime.get();

			for (size_t i = 0; i < runtime->roles.size(); ++i)
			{
				RuntimeRole& role = runtime->roles[i];
				role.style = &config.styles[i];
				role.resolvedBaselineOffset = role.style->baselineOffset;
				for (size_t sourceFaceIndex = 0;
					sourceFaceIndex < role.style->faces.size();
					++sourceFaceIndex)
				{
					const FaceConfig& faceConfig =
						role.style->faces[sourceFaceIndex];
					RuntimeFace face;
					face.sourceConfigIndex =
						static_cast<UInt16>(sourceFaceIndex);
					if (CreateRuntimeFace(faceConfig, *role.style, face))
					{
						face.resolvedBaselineOffset = role.style->baselineOffset;
						role.faces.push_back(std::move(face));
					}
					else
						gLog.FormattedMessage("tnvse_freetype_font: failed to load face font=%u path=%ls index=%ld",
							config.fontId, faceConfig.path.c_str(), faceConfig.faceIndex);
				}
				if (role.faces.empty())
					return nullptr;

				FT_Face primary = role.faces.front().ftFace;
				role.ascender = static_cast<float>(primary->size->metrics.ascender) / 64.0f
					* role.style->scaleY + role.style->embolden;
				role.descender = static_cast<float>(primary->size->metrics.descender) / 64.0f
					* role.style->scaleY - role.style->embolden;
			}

			runtime->manualBaseline = config.baseline > 0.0f;
			if (!runtime->manualBaseline
				|| config.verticalMetrics == VerticalMetricsMode::Vanilla)
			{
				runtime->verticalAlignmentRasterScale = GetCanonicalFreeTypeRasterScale();
				ApplyAutomaticVisualAlignment(
					*runtime, runtime->verticalAlignmentRasterScale);
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
			ComputeRuntimeLayoutContentHash(*runtime);
			size_t runtimeBytes = sizeof(RuntimeFont);
			for (const RuntimeRole& role : runtime->roles)
			{
				runtimeBytes += role.faces.capacity() * sizeof(RuntimeFace);
				runtimeBytes += role.glyphIdentities.size()
					* (sizeof(std::pair<const UInt32, CachedGlyphIdentity>)
						+ 3u * sizeof(void*));
			}
			runtime->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				runtimeBytes);
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

	RuntimeFont* FindRuntimeFont(UInt32 auiFontId)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		auto it = State().runtimeFonts.find(auiFontId);
		return it == State().runtimeFonts.end() ? nullptr : it->second.get();
	}

	RuntimeFont* FindActiveRuntime(const Font* apFont)
	{
		if (!apFont || !g_bEnableFreeTypeFontRendering)
			return nullptr;

		FreeTypeState& state = State();
		FreeTypeThreadState& thread = ThreadState();
		const UInt32 fontId = static_cast<UInt32>(apFont->iFontNum);
		for (size_t index = 0; index < thread.activeRuntimes.size(); ++index)
		{
			const ActiveRuntimeCache& cached = thread.activeRuntimes[index];
			if (cached.font != apFont || cached.data != apFont->pFontData
				|| cached.fontId != fontId || !cached.runtime)
			{
				continue;
			}
			RuntimeFont* runtime = cached.runtime;
			if (index)
			{
				const ActiveRuntimeCache hit = cached;
				for (size_t position = index; position > 0; --position)
					thread.activeRuntimes[position] =
						thread.activeRuntimes[position - 1];
				thread.activeRuntimes[0] = hit;
			}
			return runtime;
		}

		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const auto active = state.activeFonts.find(apFont);
		if (active == state.activeFonts.end() || active->second.data != apFont->pFontData
			|| active->second.fontId != fontId)
		{
			return nullptr;
		}
		const auto runtime = state.runtimeFonts.find(fontId);
		if (runtime == state.runtimeFonts.end())
			return nullptr;
		for (size_t index = thread.activeRuntimes.size() - 1; index > 0; --index)
			thread.activeRuntimes[index] = thread.activeRuntimes[index - 1];
		thread.activeRuntimes[0] = {
			apFont, apFont->pFontData, fontId, runtime->second.get()
		};
		return thread.activeRuntimes[0].runtime;
	}

	RuntimeFont* EnsureRuntimeFont(UInt32 auiFontId)
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		const auto existing = state.runtimeFonts.find(auiFontId);
		if (existing != state.runtimeFonts.end())
			return existing->second.get();
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
		state.runtimeFonts.emplace(auiFontId, std::move(runtime));
		return result;
	}

	ActiveFontState::VanillaVerticalMetrics CaptureVanillaVerticalMetrics(const Font& font)
	{
		ActiveFontState::VanillaVerticalMetrics result;
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
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (!runtime.initialized || !font.pFontData)
			return false;

		ActiveFontState activeState;
		const auto active = State().activeFonts.find(&font);
		if (active != State().activeFonts.end()
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
		if (!activeState.vanillaMetricsCaptured)
		{
			activeState.vanillaMetrics = CaptureVanillaVerticalMetrics(font);
			activeState.vanillaMetricsCaptured = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const auto& vanilla = activeState.vanillaMetrics;
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: vanilla metrics snapshot font=%u valid=%d baseline=%.2f fontHeight=%.2f maxDrop=%.2f space=(top=%.2f height=%.2f)",
					font.iFontNum, vanilla.valid ? 1 : 0, vanilla.baseLine,
					vanilla.fontHeight, vanilla.maxDrop,
					vanilla.spaceTopEdge, vanilla.spaceHeight);
			}
		}

		std::shared_ptr<DirectExtraGlyphTable> directCodePageMetrics;
		const bool directLayoutReady =
			TryApplyDirectCachedLayoutMetrics(runtime, font,
				GetCanonicalFreeTypeRasterScale(),
				directCodePageMetrics);
		if (directLayoutReady && directCodePageMetrics)
		{
			size_t runtimeBytes = runtime.cpuMemory.GetBytes();
			if (runtime.codePageMetrics)
			{
				const size_t previous =
					runtime.codePageMetrics->GetAllocatedBytes();
				runtimeBytes -= std::min(runtimeBytes, previous);
			}
			runtimeBytes += directCodePageMetrics->GetAllocatedBytes();
			runtime.codePageMetrics =
				std::move(directCodePageMetrics);
			runtime.cpuMemory.Reset(
				CpuMemoryCategory::RuntimeMetadata, runtimeBytes);
		}
		if (!directLayoutReady)
		{
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
		}

		const bool requestedVanilla = runtime.config->verticalMetrics == VerticalMetricsMode::Vanilla;
		const bool useVanilla = requestedVanilla && activeState.vanillaMetrics.valid;
		if (requestedVanilla && !useVanilla && !activeState.vanillaFallbackLogged)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: invalid vanilla metrics font=%u; falling back to freetype vertical metrics",
				font.iFontNum);
			activeState.vanillaFallbackLogged = true;
		}

		const auto& vanilla = activeState.vanillaMetrics;
		const bool configuredBaseline = runtime.config->baseline > 0.0f;
		const float resolvedBaseline = useVanilla
			? (configuredBaseline
				? std::ceil(std::max(1.0f, runtime.config->baseline))
				: vanilla.baseLine)
			: runtime.baseLine;
		const float resolvedMaxDrop = useVanilla ? vanilla.maxDrop : runtime.minBottom;
		const float resolvedFontHeight = useVanilla
			? resolvedBaseline - vanilla.maxDrop : runtime.fontHeight;
		const float resolvedSpaceHeight = useVanilla
			? vanilla.spaceHeight : runtime.glyphHeight;
		const float resolvedSpaceTop = useVanilla
			? vanilla.maxDrop + vanilla.spaceHeight
			: runtime.minBottom + runtime.glyphHeight;

		font.pFontData->fBaseLine = resolvedBaseline;
		font.fMaxDrop = resolvedMaxDrop;
		font.fFontHeight = resolvedFontHeight;
		font.iLineOverlap = 0;
		FontLetter& space = font.pFontData->pFontLetters[' '];
		if (space.fWidth <= 0.0f && space.fSpacing > 0.0f)
		{
			const float serializedSpaceWidth = space.fWidth;
			space.fWidth = space.fSpacing;
			space.fSpacing = serializedSpaceWidth;
		}
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
				font.iFontNum, useVanilla ? "vanilla" : "freetype",
				useVanilla ? (configuredBaseline ? "configured" : "vanilla")
					: (configuredBaseline ? "configured" : "freetype"),
				runtime.config->baseline, font.pFontData->fBaseLine,
				font.fFontHeight, font.fMaxDrop,
				space.fWidth, space.fSpacing, space.fTopEdge, space.fHeight);
		}

		ExtraGlyphStore& extraStore = gNumberedExtraLetters[font.iFontNum];
		extraStore.serialized.clear();
		extraStore.generatedCodePage.reset();
		extraStore.generated.clear();
		if (runtime.codePageMetrics
			|| (UsesDbcsTextLayout()
				&& EnsureCompleteCodePageMetricTable(runtime)))
		{
			extraStore.generatedCodePage = runtime.codePageMetrics;
			ExtraGlyphMap().swap(extraStore.generated);
		}
		else if (UsesDbcsTextLayout())
		{
			extraStore.generated.reserve(SerializedExtraGlyphTable::kGlyphCount);
		}
		State().activeFonts[&font] = activeState;
		return true;
	}

	FontLetter* EnsureDoubleByteMetrics(RuntimeFont& runtime, Font& font, UInt32 encodedCode)
	{
		const char encodedBytes[3] = {
			static_cast<char>((encodedCode >> 8) & 0xFF),
			static_cast<char>(encodedCode & 0xFF),
			0
		};
		VectorEncodedGlyph directGlyph;
		switch (DecodeSealedDirectGlyph(runtime, encodedBytes, directGlyph))
		{
		case SealedDirectGlyphLookup::Resolved:
		{
			// Rich-text layout still consumes the vanilla FontLetter metric
			// interface.  Serve it from one TLS view of the immutable direct
			// record instead of recreating the released DBCS metrics map.
			thread_local FontLetter directMetrics = {};
			directMetrics = {};
			directMetrics.iTextureIndex = -1;
			directMetrics.fWidth = directGlyph.directWidth;
			directMetrics.fHeight = directGlyph.directHeight;
			directMetrics.fLeadingEdge = directGlyph.directLeadingEdge;
			directMetrics.fSpacing = directGlyph.directSpacing;
			directMetrics.fTopEdge = directGlyph.directTopEdge;
			return &directMetrics;
		}
		case SealedDirectGlyphLookup::Invalid:
			// A reset or corrupt direct record rejects this layout request.  It
			// must not reopen FreeType while sealed GPU publication is revoked.
			return nullptr;
		default:
			break;
		}
		if (runtime.codePageMetrics)
		{
			if (FontLetter* direct = runtime.codePageMetrics->find(encodedCode))
				return direct;
		}
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		ExtraGlyphStore& extraStore = gNumberedExtraLetters[font.iFontNum];
		if (extraStore.generatedCodePage)
		{
			if (FontLetter* direct = extraStore.generatedCodePage->find(encodedCode))
				return direct;
		}
		ExtraGlyphMap& extra = extraStore.generated;
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
		if (text[1] && TryDecodeFreeTypeDoubleByte(text, encodedCode))
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
		switch (DecodeSealedDirectGlyph(runtime, text, glyph))
		{
		case SealedDirectGlyphLookup::Resolved:
			return true;
		case SealedDirectGlyphLookup::Invalid:
			return false;
		default:
			break;
		}
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
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
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
		{
			// A persistent manifest can bypass BuildFontLetter entirely. Still touch
			// the base-size FreeType slot during prewarm so the no-shaping layout path
			// starts with a complete advance/fixed-offset metric cache.
			RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
			if (glyph.faceIndex < role.faces.size())
			{
				RuntimeFace& face = role.faces[glyph.faceIndex];
				const DirectLayoutGlyphMetric* metric =
					face.directLayoutMetrics.Find(glyph.glyphIndex);
				if (!metric || !metric->valid)
					LoadGlyph(role, face, glyph.glyphIndex);
			}
			return true;
		}
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

	UInt64 GetRuntimeDirectLayoutIdentity(const RuntimeFont& runtime)
	{
		return runtime.layoutContentHash;
	}

	UInt64 GetRuntimeDirectRoleLayoutIdentity(RuntimeFont& runtime,
		VectorFontByteClass byteClass)
	{
		const size_t roleIndex = static_cast<size_t>(byteClass);
		if (roleIndex >= runtime.roles.size() || !runtime.config)
			return 0;
		UInt64& cached = runtime.layoutContentRoleHashes[roleIndex];
		if (cached)
			return cached;

		constexpr UInt32 kDirectRoleLayoutIdentityRevision = 1;
		UInt64 hash = HashBytes64(&kDirectRoleLayoutIdentityRevision,
			sizeof(kDirectRoleLayoutIdentityRevision),
			1469598103934665603ull);
		hash = HashBytes64(&runtime.config->layoutRoleHashes[roleIndex],
			sizeof(runtime.config->layoutRoleHashes[roleIndex]), hash);
		hash = HashBytes64(&runtime.verticalAlignmentRasterScale,
			sizeof(runtime.verticalAlignmentRasterScale), hash);
		const RuntimeRole& role = runtime.roles[roleIndex];
		const UInt32 count = static_cast<UInt32>(role.faces.size());
		hash = HashBytes64(&count, sizeof(count), hash);
		for (const RuntimeFace& face : role.faces)
		{
			const UInt64 contentHash = face.file ? face.file->contentHash : 0;
			const SInt32 faceIndex = face.ftFace
				? static_cast<SInt32>(face.ftFace->face_index) : 0;
			hash = HashBytes64(&contentHash, sizeof(contentHash), hash);
			hash = HashBytes64(&faceIndex, sizeof(faceIndex), hash);
		}
		cached = hash ? hash : 1;
		return cached;
	}

	size_t GetRuntimeDirectFaceCount(const RuntimeFont& runtime,
		VectorFontByteClass byteClass)
	{
		const size_t roleIndex = static_cast<size_t>(byteClass);
		return roleIndex < runtime.roles.size()
			? runtime.roles[roleIndex].faces.size() : 0;
	}

	void GetRuntimeDirectBaselineOffsets(const RuntimeFont& runtime,
		VectorFontByteClass byteClass, float& roleBaseline,
		std::vector<float>& faceBaselines)
	{
		const size_t roleIndex = static_cast<size_t>(byteClass);
		roleBaseline = 0.0f;
		faceBaselines.clear();
		if (roleIndex >= runtime.roles.size())
			return;
		const RuntimeRole& role = runtime.roles[roleIndex];
		roleBaseline = role.resolvedBaselineOffset;
		faceBaselines.reserve(role.faces.size());
		for (const RuntimeFace& face : role.faces)
			faceBaselines.push_back(face.resolvedBaselineOffset);
	}

	std::shared_ptr<const SealedDirectFontProfile>
		LoadRuntimeSealedDirectProfile(const RuntimeFont& runtime)
	{
		return runtime.sealedDirectProfile.load(
			std::memory_order_acquire);
	}

	static UInt64 BeginSealedDirectProfilePublication(RuntimeFont& runtime)
	{
		UInt64 observed = runtime.sealedDirectProfilePublicationEpoch.load(
			std::memory_order_acquire);
		for (;;)
		{
			while (!observed)
			{
				YieldProcessor();
				observed = runtime.sealedDirectProfilePublicationEpoch.load(
					std::memory_order_acquire);
			}
			if (runtime.sealedDirectProfilePublicationEpoch.compare_exchange_weak(
				observed, 0, std::memory_order_acq_rel,
				std::memory_order_acquire))
			{
				return observed;
			}
		}
	}

	static void EndSealedDirectProfilePublication(RuntimeFont& runtime,
		UInt64 previousEpoch, bool changed)
	{
		UInt64 publishedEpoch = previousEpoch;
		if (changed)
		{
			publishedEpoch = previousEpoch + 1u;
			if (!publishedEpoch)
				publishedEpoch = 1u;
		}
		runtime.sealedDirectProfilePublicationEpoch.store(
			publishedEpoch, std::memory_order_release);
	}

	void StoreRuntimeSealedDirectProfile(RuntimeFont& runtime,
		std::shared_ptr<const SealedDirectFontProfile> profile)
	{
		const SealedDirectFontProfile* incoming = profile.get();
		const UInt64 previousEpoch = BeginSealedDirectProfilePublication(runtime);
		const std::shared_ptr<const SealedDirectFontProfile> previous =
			runtime.sealedDirectProfile.load(std::memory_order_acquire);
		const bool changed = previous.get() != incoming;
		if (changed)
		{
			runtime.sealedDirectProfile.store(
				std::move(profile), std::memory_order_release);
		}
		EndSealedDirectProfilePublication(runtime, previousEpoch, changed);
	}

	void InvalidateSealedDirectFontProfileIfCurrent(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& expected)
	{
		if (!expected)
			return;
		const UInt64 previousEpoch = BeginSealedDirectProfilePublication(runtime);
		const std::shared_ptr<const SealedDirectFontProfile> current =
			runtime.sealedDirectProfile.load(std::memory_order_acquire);
		const bool changed = current.get() == expected.get();
		if (changed)
		{
			std::shared_ptr<const SealedDirectFontProfile> empty;
			runtime.sealedDirectProfile.store(
				std::move(empty), std::memory_order_release);
		}
		EndSealedDirectProfilePublication(runtime, previousEpoch, changed);
	}

	void ReleaseSealedRuntimeFreeTypeState(RuntimeFont& runtime)
	{
		FreeTypeState& state = State();
		std::lock_guard<std::recursive_mutex> lock(state.mutex);
		runtime.manifest.reset();
		runtime.codePageMetrics.reset();
		if (runtime.config)
		{
			const auto extra =
				gNumberedExtraLetters.find(runtime.config->fontId);
			if (extra != gNumberedExtraLetters.end())
			{
				extra->second.generated.clear();
				extra->second.generatedCodePage.reset();
			}
		}
		size_t releasedFaces = 0;
		for (RuntimeRole& role : runtime.roles)
		{
			std::unordered_map<UInt32, CachedGlyphIdentity>().swap(
				role.glyphIdentities);
			for (RuntimeFace& face : role.faces)
			{
				face.directLayoutMetrics.Clear();
				face.directLayoutMetricMemory.Release();
				if (face.ftFace)
				{
					FT_Done_Face(face.ftFace);
					face.ftFace = nullptr;
					++releasedFaces;
				}
				face.file.reset();
				face.configured = false;
				face.configuredRaster = false;
				face.configuredWidth = 0;
				face.configuredHeight = 0;
			}
		}
		for (auto it = state.mappedFiles.begin();
			it != state.mappedFiles.end();)
		{
			if (it->second.expired())
				it = state.mappedFiles.erase(it);
			else
				++it;
		}
		for (auto it = state.persistentGlyphManifests.begin();
			it != state.persistentGlyphManifests.end();)
		{
			if (it->second.expired())
				it = state.persistentGlyphManifests.erase(it);
			else
				++it;
		}
		size_t runtimeBytes = sizeof(RuntimeFont);
		for (const RuntimeRole& role : runtime.roles)
			runtimeBytes += role.faces.capacity()
				* sizeof(RuntimeFace);
		runtime.cpuMemory.Reset(
			CpuMemoryCategory::RuntimeMetadata, runtimeBytes);

		bool hasLiveFaces = false;
		for (const auto& entry : state.runtimeFonts)
		{
			if (!entry.second)
				continue;
			for (const RuntimeRole& role : entry.second->roles)
			{
				for (const RuntimeFace& face : role.faces)
				{
					if (face.ftFace)
					{
						hasLiveFaces = true;
						break;
					}
				}
				if (hasLiveFaces)
					break;
			}
			if (hasLiveFaces)
				break;
		}
		if (!hasLiveFaces && state.library)
		{
			FT_Done_FreeType(state.library);
			state.library = nullptr;
			state.singleByteCodePoints.fill(UINT32_MAX);
			state.doubleByteCodePoints.Clear();
			state.codePointCacheMemory.Release();
			state.codePointCacheCodePage = UINT32_MAX;
		}
		if (!hasLiveFaces)
		{
			ReleaseGlyphBitmapDiskCacheMappings();
			state.bitmapCache.clear();
			state.bitmapCache.rehash(0);
			state.bitmapLru.clear();
			state.bitmapCacheBytes = 0;
			state.persistentBitmapProfiles.clear();
			state.persistentBitmapProfiles.rehash(0);
			state.mappedFiles.rehash(0);
			state.persistentGlyphManifests.rehash(0);
		}
		if (releasedFaces || g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: sealed runtime released font=%u faces=%u manifest=released layoutMaps=released library=%s",
				runtime.config ? runtime.config->fontId : 0,
				static_cast<UInt32>(releasedFaces),
				state.library ? "retained" : "released");
		}
	}

	UInt64 GetRuntimeMaskContentHash(RuntimeFont& runtime,
		VectorFontByteClass byteClass)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		return ComputeRuntimeMaskContentHash(runtime, byteClass);
	}


	float GetGlyphBaselineOffset(const RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph)
	{
		if (glyph.hasDirectMetrics)
			return glyph.directBaselineOffset;
		const RuntimeRole& role = runtime.roles[static_cast<size_t>(glyph.byteClass)];
		return glyph.hasGlyphIdentity && glyph.faceIndex < role.faces.size()
			? role.faces[glyph.faceIndex].resolvedBaselineOffset
			: role.resolvedBaselineOffset;
	}
}
