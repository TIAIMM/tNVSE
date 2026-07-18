#include "multibyte_input_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <array>

// FreeType CPU rasterizer for the IME composition and candidate overlay.

namespace fonthook
{
	namespace multibyte_input
	{
		namespace
		{
			constexpr UInt32 kOverlayFontSize = 18;
			constexpr UInt32 kOverlayPadding = 10;
			constexpr UInt32 kOverlayLineHeight = 24;
			constexpr UInt32 kOverlayMinWidth = 260;
			constexpr UInt32 kOverlayMaxWidth = 620;
			constexpr UInt32 kOverlayAlpha = 0xE8;
			constexpr UInt32 kBackgroundColor = 0xE8121212;
			constexpr UInt32 kBorderColor = 0xE8DCDCDC;
			constexpr UInt32 kHighlightColor = 0xE83A547E;
			constexpr UInt32 kTextColor = 0x00E6E6E6;
			constexpr UInt32 kHighlightedTextColor = 0x00FFFFFF;
			constexpr FT_Int32 kGlyphLoadFlags =
				FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;

			struct MappedFontFile
			{
				std::wstring path;
				HANDLE file = INVALID_HANDLE_VALUE;
				HANDLE mapping = nullptr;
				const FT_Byte* data = nullptr;
				FT_Long size = 0;
				std::vector<FT_Face> faces;
			};

			struct GlyphPlacement
			{
				FT_Face face = nullptr;
				FT_UInt glyphIndex = 0;
				SInt32 kerning = 0;
				SInt32 advance = 0;
			};

			struct LineLayout
			{
				std::vector<GlyphPlacement> glyphs;
				SInt32 width = 0;
				bool highlighted = false;
			};

			FT_Library s_library = nullptr;
			std::vector<std::unique_ptr<MappedFontFile>> s_fontFiles;
			std::vector<FT_Face> s_faces;
			bool s_rendererInitialized = false;
			bool s_rendererAvailable = false;
			SInt32 s_maxAscender = 0;
			SInt32 s_maxDescender = 0;

			bool IsAbsolutePath(const std::wstring& path)
			{
				return (path.size() >= 2 && path[1] == L':')
					|| (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
			}

			std::wstring GetGameDirectory()
			{
				std::array<wchar_t, MAX_PATH> modulePath = {};
				const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
				if (!length || length >= modulePath.size())
					return {};

				std::wstring result(modulePath.data(), length);
				const size_t slash = result.find_last_of(L"\\/");
				return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
			}

			std::wstring ResolveConfiguredFontPath(const std::wstring& path)
			{
				if (path.empty() || IsAbsolutePath(path))
					return path;

				std::wstring gameDirectory = GetGameDirectory();
				if (gameDirectory.empty())
					return path;

				return gameDirectory + L"\\" + path;
			}

			std::wstring GetWindowsFontsDirectory()
			{
				std::array<wchar_t, MAX_PATH> windowsDirectory = {};
				const UINT length = GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
				if (!length || length >= windowsDirectory.size())
					return {};

				return std::wstring(windowsDirectory.data(), length) + L"\\Fonts";
			}

			void AddUniquePath(std::vector<std::wstring>& paths, const std::wstring& path)
			{
				if (path.empty())
					return;

				for (const std::wstring& existing : paths)
				{
					if (!_wcsicmp(existing.c_str(), path.c_str()))
						return;
				}

				paths.push_back(path);
			}

			std::vector<std::wstring> GetFontPaths()
			{
				std::vector<std::wstring> paths;
				AddUniquePath(paths, ResolveConfiguredFontPath(g_sMultibyteInputOverlayFontPath));

				const std::wstring fontsDirectory = GetWindowsFontsDirectory();
				if (fontsDirectory.empty())
					return paths;

				auto addSystemFont = [&](const wchar_t* filename)
					{
						AddUniquePath(paths, fontsDirectory + L"\\" + filename);
					};

				switch (g_uiEncoding)
				{
				case 2:
					addSystemFont(L"msjh.ttc");
					addSystemFont(L"mingliu.ttc");
					break;
				case 3:
					addSystemFont(L"meiryo.ttc");
					addSystemFont(L"YuGothM.ttc");
					addSystemFont(L"msgothic.ttc");
					break;
				case 4:
					addSystemFont(L"malgun.ttf");
					addSystemFont(L"gulim.ttc");
					break;
				default:
					addSystemFont(L"msyh.ttc");
					addSystemFont(L"simsun.ttc");
					addSystemFont(L"NotoSansSC-VF.ttf");
					break;
				}

				return paths;
			}

			SInt32 Ceil26Dot6(FT_Pos value)
			{
				if (value >= 0)
					return static_cast<SInt32>((value + 63) / 64);
				return static_cast<SInt32>(value / 64);
			}

			void UpdateFaceMetrics(FT_Face face)
			{
				if (!face || !face->size)
					return;

				s_maxAscender = std::max(s_maxAscender, Ceil26Dot6(face->size->metrics.ascender));
				s_maxDescender = std::max(s_maxDescender, Ceil26Dot6(-face->size->metrics.descender));
			}

			bool LoadFontFile(const std::wstring& path)
			{
				auto fontFile = std::make_unique<MappedFontFile>();
				fontFile->path = path;
				fontFile->file = CreateFileW(
					path.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					nullptr);
				if (fontFile->file == INVALID_HANDLE_VALUE)
					return false;

				LARGE_INTEGER fileSize = {};
				if (!GetFileSizeEx(fontFile->file, &fileSize)
					|| fileSize.QuadPart <= 0
					|| fileSize.QuadPart > std::numeric_limits<FT_Long>::max())
				{
					CloseHandle(fontFile->file);
					return false;
				}

				fontFile->size = static_cast<FT_Long>(fileSize.QuadPart);
				fontFile->mapping = CreateFileMappingW(fontFile->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				if (!fontFile->mapping)
				{
					CloseHandle(fontFile->file);
					return false;
				}

				fontFile->data = static_cast<const FT_Byte*>(MapViewOfFile(fontFile->mapping, FILE_MAP_READ, 0, 0, 0));
				if (!fontFile->data)
				{
					CloseHandle(fontFile->mapping);
					CloseHandle(fontFile->file);
					return false;
				}

				FT_Face firstFace = nullptr;
				if (FT_New_Memory_Face(s_library, fontFile->data, fontFile->size, 0, &firstFace))
				{
					UnmapViewOfFile(fontFile->data);
					CloseHandle(fontFile->mapping);
					CloseHandle(fontFile->file);
					return false;
				}

				const FT_Long faceCount = std::max<FT_Long>(firstFace->num_faces, 1);
				for (FT_Long faceIndex = 0; faceIndex < faceCount; ++faceIndex)
				{
					FT_Face face = faceIndex == 0 ? firstFace : nullptr;
					if (faceIndex && FT_New_Memory_Face(s_library, fontFile->data, fontFile->size, faceIndex, &face))
						continue;

					if (FT_Select_Charmap(face, FT_ENCODING_UNICODE)
						|| FT_Set_Pixel_Sizes(face, 0, kOverlayFontSize))
					{
						FT_Done_Face(face);
						continue;
					}

					fontFile->faces.push_back(face);
					s_faces.push_back(face);
					UpdateFaceMetrics(face);
				}

				if (fontFile->faces.empty())
				{
					UnmapViewOfFile(fontFile->data);
					CloseHandle(fontFile->mapping);
					CloseHandle(fontFile->file);
					return false;
				}

				DebugLog(
					"tnvse_multibyte_input: FreeType loaded font path='%ls' faces=%u",
					path.c_str(),
					static_cast<UInt32>(fontFile->faces.size()));
				s_fontFiles.push_back(std::move(fontFile));
				return true;
			}

			std::vector<UInt32> DecodeCodePoints(const std::wstring& text)
			{
				std::vector<UInt32> result;
				result.reserve(text.size());

				for (size_t i = 0; i < text.size(); ++i)
				{
					const UInt32 value = static_cast<UInt16>(text[i]);
					if (value >= 0xD800 && value <= 0xDBFF && i + 1 < text.size())
					{
						const UInt32 trail = static_cast<UInt16>(text[i + 1]);
						if (trail >= 0xDC00 && trail <= 0xDFFF)
						{
							result.push_back(0x10000 + ((value - 0xD800) << 10) + (trail - 0xDC00));
							++i;
							continue;
						}
					}

					if (value >= 0xDC00 && value <= 0xDFFF)
						result.push_back(0xFFFD);
					else
						result.push_back(value);
				}

				return result;
			}

			bool ResolveLoadedGlyph(UInt32 codePoint, FT_Face& face, FT_UInt& glyphIndex)
			{
				for (FT_Face candidate : s_faces)
				{
					const FT_UInt candidateIndex = FT_Get_Char_Index(candidate, codePoint);
					if (!candidateIndex || FT_Load_Glyph(candidate, candidateIndex, kGlyphLoadFlags))
						continue;

					face = candidate;
					glyphIndex = candidateIndex;
					return true;
				}

				return false;
			}

			bool HasGlyph(UInt32 codePoint)
			{
				for (FT_Face face : s_faces)
				{
					if (FT_Get_Char_Index(face, codePoint))
						return true;
				}
				return false;
			}

			bool ResolveGlyphWithFallback(UInt32 codePoint, FT_Face& face, FT_UInt& glyphIndex)
			{
				if (ResolveLoadedGlyph(codePoint, face, glyphIndex))
					return true;
				if (codePoint != 0xFFFD && ResolveLoadedGlyph(0xFFFD, face, glyphIndex))
					return true;
				return codePoint != '?' && ResolveLoadedGlyph('?', face, glyphIndex);
			}

			LineLayout BuildLineLayout(const CandidateOverlayLine& line)
			{
				LineLayout result;
				result.highlighted = line.highlighted;
				FT_Face previousFace = nullptr;
				FT_UInt previousGlyph = 0;

				for (UInt32 codePoint : DecodeCodePoints(line.text))
				{
					FT_Face face = nullptr;
					FT_UInt glyphIndex = 0;
					if (!ResolveGlyphWithFallback(codePoint, face, glyphIndex))
						continue;

					SInt32 kerning = 0;
					if (face == previousFace && previousGlyph && FT_HAS_KERNING(face))
					{
						FT_Vector delta = {};
						if (!FT_Get_Kerning(face, previousGlyph, glyphIndex, FT_KERNING_DEFAULT, &delta))
							kerning = static_cast<SInt32>(delta.x / 64);
					}

					const SInt32 advance = std::max<SInt32>(0, static_cast<SInt32>(face->glyph->advance.x / 64));
					result.glyphs.push_back({ face, glyphIndex, kerning, advance });
					result.width += kerning + advance;
					previousFace = face;
					previousGlyph = glyphIndex;
				}

				return result;
			}

			void TrimLineWithEllipsis(LineLayout& line, const LineLayout& ellipsis, SInt32 maxWidth)
			{
				if (line.width <= maxWidth || ellipsis.glyphs.empty())
					return;

				const SInt32 available = std::max<SInt32>(0, maxWidth - ellipsis.width);
				SInt32 width = 0;
				size_t keep = 0;
				for (; keep < line.glyphs.size(); ++keep)
				{
					const GlyphPlacement& glyph = line.glyphs[keep];
					const SInt32 nextWidth = width + glyph.kerning + glyph.advance;
					if (nextWidth > available)
						break;
					width = nextWidth;
				}

				line.glyphs.resize(keep);
				line.glyphs.insert(line.glyphs.end(), ellipsis.glyphs.begin(), ellipsis.glyphs.end());
				line.width = width + ellipsis.width;
			}

			void FillRect(
				std::vector<UInt32>& pixels,
				UInt32 width,
				UInt32 height,
				SInt32 left,
				SInt32 top,
				SInt32 right,
				SInt32 bottom,
				UInt32 color)
			{
				left = std::clamp<SInt32>(left, 0, static_cast<SInt32>(width));
				right = std::clamp<SInt32>(right, 0, static_cast<SInt32>(width));
				top = std::clamp<SInt32>(top, 0, static_cast<SInt32>(height));
				bottom = std::clamp<SInt32>(bottom, 0, static_cast<SInt32>(height));
				if (left >= right || top >= bottom)
					return;

				for (SInt32 y = top; y < bottom; ++y)
				{
					UInt32* row = pixels.data() + static_cast<size_t>(y) * width;
					std::fill(row + left, row + right, color);
				}
			}

			UInt8 BitmapCoverage(const FT_Bitmap& bitmap, UInt32 x, UInt32 y)
			{
				const SInt32 pitch = bitmap.pitch;
				const UInt8* row = pitch >= 0
					? bitmap.buffer + static_cast<size_t>(y) * pitch
					: bitmap.buffer + static_cast<size_t>(bitmap.rows - 1 - y) * static_cast<size_t>(-pitch);

				if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
					return row[x >> 3] & (0x80 >> (x & 7)) ? 255 : 0;
				if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
				{
					const UInt32 value = row[x];
					return bitmap.num_grays > 1
						? static_cast<UInt8>(std::min<UInt32>(255, value * 255 / (bitmap.num_grays - 1)))
						: static_cast<UInt8>(value ? 255 : 0);
				}

				return 0;
			}

			void BlendGlyphPixel(UInt32& destination, UInt32 sourceRgb, UInt8 coverage)
			{
				if (!coverage)
					return;

				auto blend = [&](UInt32 shift) -> UInt32
					{
						const UInt32 source = sourceRgb >> shift & 0xFF;
						const UInt32 target = destination >> shift & 0xFF;
						return (source * coverage + target * (255 - coverage) + 127) / 255;
					};

				destination = kOverlayAlpha << 24
					| blend(16) << 16
					| blend(8) << 8
					| blend(0);
			}

			void DrawLine(
				std::vector<UInt32>& pixels,
				UInt32 width,
				UInt32 height,
				const LineLayout& line,
				SInt32 lineTop)
			{
				const SInt32 textHeight = s_maxAscender + s_maxDescender;
				const SInt32 baseline = lineTop
					+ std::max<SInt32>(0, (static_cast<SInt32>(kOverlayLineHeight) - textHeight) / 2)
					+ s_maxAscender;
				const SInt32 clipLeft = static_cast<SInt32>(kOverlayPadding);
				const SInt32 clipRight = static_cast<SInt32>(width - kOverlayPadding);
				const SInt32 clipTop = lineTop;
				const SInt32 clipBottom = lineTop + static_cast<SInt32>(kOverlayLineHeight);
				const UInt32 textColor = line.highlighted ? kHighlightedTextColor : kTextColor;
				SInt32 penX = clipLeft;

				for (const GlyphPlacement& glyph : line.glyphs)
				{
					penX += glyph.kerning;
					if (FT_Load_Glyph(glyph.face, glyph.glyphIndex, kGlyphLoadFlags)
						|| FT_Render_Glyph(glyph.face->glyph, FT_RENDER_MODE_NORMAL))
					{
						penX += glyph.advance;
						continue;
					}

					const FT_GlyphSlot slot = glyph.face->glyph;
					const FT_Bitmap& bitmap = slot->bitmap;
					const SInt32 glyphLeft = penX + slot->bitmap_left;
					const SInt32 glyphTop = baseline - slot->bitmap_top;

					for (UInt32 y = 0; y < bitmap.rows; ++y)
					{
						const SInt32 destinationY = glyphTop + static_cast<SInt32>(y);
						if (destinationY < clipTop || destinationY >= clipBottom
							|| destinationY < 0 || destinationY >= static_cast<SInt32>(height))
						{
							continue;
						}

						for (UInt32 x = 0; x < bitmap.width; ++x)
						{
							const SInt32 destinationX = glyphLeft + static_cast<SInt32>(x);
							if (destinationX < clipLeft || destinationX >= clipRight)
								continue;

							UInt32& destination = pixels[static_cast<size_t>(destinationY) * width + destinationX];
							BlendGlyphPixel(destination, textColor, BitmapCoverage(bitmap, x, y));
						}
					}

					penX += glyph.advance;
				}
			}
		}

		bool InitializeCandidateOverlayRenderer()
		{
			if (s_rendererInitialized)
				return s_rendererAvailable;

			s_rendererInitialized = true;
			if (FT_Init_FreeType(&s_library))
			{
				gLog.FormattedMessage("tnvse_multibyte_input: FreeType initialization failed; using system IME candidate window");
				return false;
			}

			for (const std::wstring& path : GetFontPaths())
				LoadFontFile(path);

			s_rendererAvailable = !s_faces.empty();
			if (!s_rendererAvailable)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: FreeType could not load an IME overlay font; configure sMultibyteInputOverlayFontPath; using system IME candidate window");
				return false;
			}

			if (s_maxAscender <= 0)
				s_maxAscender = static_cast<SInt32>(kOverlayFontSize);
			gLog.FormattedMessage(
				"tnvse_multibyte_input: FreeType IME overlay initialized fonts=%u faces=%u",
				static_cast<UInt32>(s_fontFiles.size()),
				static_cast<UInt32>(s_faces.size()));
			return true;
		}

		bool IsCandidateOverlayRendererAvailable()
		{
			return s_rendererAvailable;
		}

		bool RasterizeCandidateOverlay(
			const std::vector<CandidateOverlayLine>& lines,
			std::vector<UInt32>& pixels,
			UInt32& width,
			UInt32& height)
		{
			pixels.clear();
			width = 0;
			height = 0;
			if (!s_rendererAvailable || lines.empty())
				return false;

			std::vector<LineLayout> layouts;
			layouts.reserve(lines.size());
			SInt32 maxLineWidth = 0;
			for (const CandidateOverlayLine& line : lines)
			{
				layouts.push_back(BuildLineLayout(line));
				maxLineWidth = std::max(maxLineWidth, layouts.back().width);
			}

			width = std::clamp<UInt32>(
				static_cast<UInt32>(std::max<SInt32>(0, maxLineWidth)) + kOverlayPadding * 2,
				kOverlayMinWidth,
				kOverlayMaxWidth);
			height = std::max<UInt32>(
				1,
				kOverlayPadding * 2 + static_cast<UInt32>(layouts.size()) * kOverlayLineHeight);

			CandidateOverlayLine ellipsisLine;
			ellipsisLine.text = HasGlyph(0x2026) ? L"\u2026" : L"...";
			const LineLayout ellipsis = BuildLineLayout(ellipsisLine);
			const SInt32 maxTextWidth = static_cast<SInt32>(width - kOverlayPadding * 2);
			for (LineLayout& line : layouts)
				TrimLineWithEllipsis(line, ellipsis, maxTextWidth);

			pixels.assign(static_cast<size_t>(width) * height, kBackgroundColor);
			FillRect(pixels, width, height, 0, 0, static_cast<SInt32>(width), 1, kBorderColor);
			FillRect(pixels, width, height, 0, static_cast<SInt32>(height) - 1, static_cast<SInt32>(width), static_cast<SInt32>(height), kBorderColor);
			FillRect(pixels, width, height, 0, 0, 1, static_cast<SInt32>(height), kBorderColor);
			FillRect(pixels, width, height, static_cast<SInt32>(width) - 1, 0, static_cast<SInt32>(width), static_cast<SInt32>(height), kBorderColor);

			for (size_t i = 0; i < layouts.size(); ++i)
			{
				const SInt32 lineTop = static_cast<SInt32>(kOverlayPadding + i * kOverlayLineHeight);
				if (layouts[i].highlighted)
				{
					FillRect(
						pixels,
						width,
						height,
						static_cast<SInt32>(kOverlayPadding) - 4,
						lineTop,
						static_cast<SInt32>(width - kOverlayPadding) + 4,
						lineTop + static_cast<SInt32>(kOverlayLineHeight),
						kHighlightColor);
				}
				DrawLine(pixels, width, height, layouts[i], lineTop);
			}

			return true;
		}

		void ShutdownCandidateOverlayRenderer()
		{
			for (const std::unique_ptr<MappedFontFile>& fontFile : s_fontFiles)
			{
				for (FT_Face face : fontFile->faces)
					FT_Done_Face(face);
				if (fontFile->data)
					UnmapViewOfFile(fontFile->data);
				if (fontFile->mapping)
					CloseHandle(fontFile->mapping);
				if (fontFile->file != INVALID_HANDLE_VALUE)
					CloseHandle(fontFile->file);
			}

			s_fontFiles.clear();
			s_faces.clear();
			if (s_library)
				FT_Done_FreeType(s_library);
			s_library = nullptr;
			s_rendererAvailable = false;
			s_rendererInitialized = false;
			s_maxAscender = 0;
			s_maxDescender = 0;
		}
	}
}
