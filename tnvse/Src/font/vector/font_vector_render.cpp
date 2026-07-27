#include "font_vector_internal.h"

#include "font_native_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiNode.hpp"
#include "NiPixelData.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook
{
	namespace
	{
		NiTexturingProperty* s_whiteTextureProperty = nullptr;
		bool s_whiteTextureInitialized = false;
		bool s_whiteTextureAvailable = false;
		std::mutex s_routeLogMutex;
		std::unordered_set<UInt64> s_loggedGlyphRoutes;
		UInt32 s_atlasEmptyLogCount = 0;
		UInt32 s_atlasFailureLogCount = 0;

		const char* GlyphAtlasBuildOutcomeName(
			vectorfont::GlyphAtlasBuildOutcome outcome)
		{
			switch (outcome)
			{
			case vectorfont::GlyphAtlasBuildOutcome::Created:
				return "created";
			case vectorfont::GlyphAtlasBuildOutcome::EmptyInput:
				return "empty-input";
			case vectorfont::GlyphAtlasBuildOutcome::NoDrawableShaderQuads:
				return "no-drawable-shader-quads";
			case vectorfont::GlyphAtlasBuildOutcome::NoDrawableCpuQuads:
				return "no-drawable-cpu-quads";
			case vectorfont::GlyphAtlasBuildOutcome::CpuMaskBuildFailure:
				return "cpu-mask-build";
			case vectorfont::GlyphAtlasBuildOutcome::QuadLimit:
				return "quad-limit";
			case vectorfont::GlyphAtlasBuildOutcome::AtlasOrShapeFailure:
				return "atlas-or-shape";
			default:
				return "unknown";
			}
		}

		const char* GlyphAtlasMaskFailureName(
			vectorfont::GlyphAtlasMaskFailure failure)
		{
			switch (failure)
			{
			case vectorfont::GlyphAtlasMaskFailure::Fill:
				return "fill";
			case vectorfont::GlyphAtlasMaskFailure::Shadow:
				return "shadow";
			case vectorfont::GlyphAtlasMaskFailure::Glow:
				return "glow";
			case vectorfont::GlyphAtlasMaskFailure::Outline:
				return "outline";
			default:
				return "none";
			}
		}

		bool InitializeWhiteTexture()
		{
			if (s_whiteTextureInitialized)
				return s_whiteTextureAvailable;
			s_whiteTextureInitialized = true;

			NiTexture::FormatPrefs formatPrefs;
			formatPrefs.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
			formatPrefs.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
			formatPrefs.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);

			NiPixelData* pixelData = static_cast<NiPixelData*>(NiMemObject::operator new(sizeof(NiPixelData)));
			if (!pixelData)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture pixel allocation failed");
				return false;
			}
			pixelData = ThisStdCall<NiPixelData*>(0xA7C190, pixelData, 1u, 1u,
				reinterpret_cast<const NiPixelFormat*>(0x11AA2A0), 1, 1);
			if (!pixelData || !pixelData->m_pucPixels || !pixelData->m_puiOffsetInBytes)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture pixel initialization failed");
				return false;
			}
			*reinterpret_cast<UInt32*>(&pixelData->m_pucPixels[*pixelData->m_puiOffsetInBytes]) = 0xFFFFFFFFu;
			pixelData->bNoConvert = 1;

			NiTexturingProperty* property = static_cast<NiTexturingProperty*>(
				NiMemObject::operator new(sizeof(NiTexturingProperty)));
			if (!property)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture property allocation failed");
				return false;
			}

			NiFixedString textureName;
			textureName.m_kHandle = static_cast<char*>(NiGlobalStringTable::AddString("tNVSE FreeType White"));
			property = ThisStdCall<NiTexturingProperty*>(0xA6ABB0,
				property, pixelData, &textureName, &formatPrefs);
			if (!property)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture property initialization failed");
				return false;
			}

			ThisStdCall(0x60AEB0, property, 1);
			property->IncRefCount();
			s_whiteTextureProperty = property;
			s_whiteTextureAvailable = s_whiteTextureProperty != nullptr;
			if (!s_whiteTextureAvailable)
				gLog.FormattedMessage("tnvse_freetype_font: failed to create 1x1 white text texture");
			return s_whiteTextureAvailable;
		}

		NiTriShape* CreateEmptyVectorShape(Font* font, bool prepareObject)
		{
			if (!font)
				return nullptr;
			const NiColorA transparent = { 1.0f, 1.0f, 1.0f, 0.0f };
			NiTriShape* shape = font->MakeTriShape(1, &transparent, false);
			if (!shape || !shape->GetModelData())
				return nullptr;

			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);
			NiTriShapeData* data = shape->GetModelData();
			for (UInt32 i = 0; i < data->m_usVertices; ++i)
			{
				data->m_pkVertex[i] = NiPoint3(0.0f, 0.0f, 0.0f);
				if (data->m_pkTexture)
					data->m_pkTexture[i] = NiPoint2(0.5f, 0.5f);
			}
			for (UInt32 i = 0; i < static_cast<UInt32>(data->m_usTriangles) * 3; ++i)
				data->m_pusTriList[i] = 0;
			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		struct RichTextVectorContext
		{
			static constexpr size_t kDirectFontSlots = 64;
			NiNode* parent = nullptr;
			float rasterScale = 1.0f;
			std::array<Font*, kDirectFontSlots> fonts = {};
			std::array<std::unique_ptr<VectorTextBuilder>,
				kDirectFontSlots> builders;
			std::unordered_map<Font*, std::unique_ptr<VectorTextBuilder>>
				fallbackBuilders;
		};

		thread_local std::unique_ptr<RichTextVectorContext> s_richTextContext;
	}

	struct VectorTextBuilder::Impl
	{
		Font* font = nullptr;
		vectorfont::RuntimeFont* runtime = nullptr;
		bool prepareObject = false;
		bool available = false;
		bool finished = false;
		bool suppressEffects = false;
		float rasterScale = 1.0f;
		NiColorA tileColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::vector<vectorfont::AtlasGlyphInstance> glyphs;

		Impl(Font* apFont, bool abPrepareObject, float afRasterScale,
			const NiColorA* apTileColor)
			: font(apFont), prepareObject(abPrepareObject),
			suppressEffects(IsFreeTypeEffectSuppressionActive()),
			rasterScale(std::isfinite(afRasterScale) && afRasterScale >= 0.1f
				&& afRasterScale <= 10.0f ? afRasterScale : 1.0f),
				tileColor(apTileColor ? *apTileColor
					: NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f })
		{
			if (!font)
				return;
			runtime = vectorfont::FindActiveRuntime(font);
			if (!runtime || !InitializeWhiteTexture())
				return;
			available = true;
		}
	};

	bool InitializeFreeTypeVectorRenderer()
	{
		return InitializeWhiteTexture();
	}

	NiTriShape* CreateEmptyFreeTypeTextShape(Font* font, bool prepareObject)
	{
		return CreateEmptyVectorShape(font, prepareObject);
	}

	VectorTextBuilder::VectorTextBuilder(Font* apFont, bool abPrepareObject,
		float afRasterScale, const NiColorA* apTileColor)
		: m_impl(std::make_unique<Impl>(apFont, abPrepareObject,
			afRasterScale, apTileColor))
	{
	}

	VectorTextBuilder::~VectorTextBuilder() = default;

	bool VectorTextBuilder::IsAvailable() const
	{
		return m_impl && m_impl->available;
	}

	void VectorTextBuilder::ReserveGlyphs(size_t auiCount)
	{
		if (m_impl && m_impl->available)
			m_impl->glyphs.reserve(auiCount);
	}

	bool VectorTextBuilder::AddGlyph(const VectorEncodedGlyph& glyph,
		const NiPoint3& pen, const NiColorA* color)
	{
		if (!IsAvailable() || !glyph.metrics || !glyph.byteLength)
			return false;
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			const vectorfont::FontConfig& config =
				vectorfont::GetRuntimeConfig(*m_impl->runtime);
			const UInt64 routeKey = (static_cast<UInt64>(config.fontId) << 8)
				| static_cast<UInt8>(glyph.byteClass);
			std::lock_guard<std::mutex> lock(s_routeLogMutex);
			if (s_loggedGlyphRoutes.insert(routeKey).second)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: first atlas glyph font=%u role=%s bytes=%u encoded=0x%04X codepoint=U+%04X",
					config.fontId,
					glyph.byteClass == VectorFontByteClass::DoubleByte ? "doubleByte" : "singleByte",
					glyph.byteLength, glyph.encodedCode, glyph.codePoint);
			}
		}
		const NiColorA sourceColor = color ? *color : m_impl->tileColor;
		m_impl->glyphs.push_back({ glyph, pen, sourceColor });
		return true;
	}

	NiTriShape* VectorTextBuilder::Finish()
	{
		if (!m_impl || m_impl->finished)
			return nullptr;
		m_impl->finished = true;
		if (!m_impl->font)
			return nullptr;
		if (!m_impl->available)
			return CreateEmptyVectorShape(m_impl->font, m_impl->prepareObject);

		vectorfont::GlyphAtlasBuildDiagnostics atlasDiagnostics;
		if (NiTriShape* atlasShape = vectorfont::TryCreateGlyphAtlasShape(
			*m_impl->font, *m_impl->runtime, m_impl->glyphs,
			m_impl->rasterScale, m_impl->prepareObject, m_impl->tileColor,
			m_impl->suppressEffects, &atlasDiagnostics))
		{
			return atlasShape;
		}
		NiTriShape* emptyShape = CreateEmptyVectorShape(
			m_impl->font, m_impl->prepareObject);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			std::lock_guard<std::mutex> lock(s_routeLogMutex);
			const bool actualFailure = !atlasDiagnostics.expectedEmpty || !emptyShape;
			UInt32& logCount = actualFailure
				? s_atlasFailureLogCount : s_atlasEmptyLogCount;
			const UInt32 maximumLogs = actualFailure ? 8 : 4;
			if (logCount < maximumLogs)
			{
				++logCount;
				gLog.FormattedMessage(
					actualFailure
						? "tnvse_freetype_native: submission-suppressed reason=atlas-shape-build phase=shape-build classification=%s outcome=%s expectedEmpty=%u font=%u fontObject=%p runtime=%p emptyShape=%p glyphs=%u missingMetrics=%u zeroByteLength=%u controls=%u spaces=%u firstCodepoint=U+%04X firstEncoded=0x%04X firstGlyph=%u firstBytes=%u firstRole=%s scale=%.3f prepare=%u suppressEffects=%u wantsShader=%u hasEffects=%u sdfFill=%u a8Renderer=%u requestedQuality=%u resolvedQuality=%u shaderBuilt=%u shaderQuads=%u shaderShapeAttempts=%u shaderShapeFailed=%u cpuBuilt=%u cpuQuads=%u cpuAttempts=%u cpuMaskFailure=%s degradedLayers=%u cpuShapeAttempts=%u nativeReady=%u nativeGeneration=%u thread=%u"
						: "tnvse_freetype_native: empty-shape reason=atlas-shape-build phase=shape-build classification=%s outcome=%s expectedEmpty=%u font=%u fontObject=%p runtime=%p emptyShape=%p glyphs=%u missingMetrics=%u zeroByteLength=%u controls=%u spaces=%u firstCodepoint=U+%04X firstEncoded=0x%04X firstGlyph=%u firstBytes=%u firstRole=%s scale=%.3f prepare=%u suppressEffects=%u wantsShader=%u hasEffects=%u sdfFill=%u a8Renderer=%u requestedQuality=%u resolvedQuality=%u shaderBuilt=%u shaderQuads=%u shaderShapeAttempts=%u shaderShapeFailed=%u cpuBuilt=%u cpuQuads=%u cpuAttempts=%u cpuMaskFailure=%s degradedLayers=%u cpuShapeAttempts=%u nativeReady=%u nativeGeneration=%u thread=%u",
						actualFailure ? "failure" : "expected-empty",
						GlyphAtlasBuildOutcomeName(atlasDiagnostics.outcome),
						atlasDiagnostics.expectedEmpty ? 1 : 0,
						m_impl->font->iFontNum, static_cast<void*>(m_impl->font),
						static_cast<void*>(m_impl->runtime), static_cast<void*>(emptyShape),
						atlasDiagnostics.inputGlyphCount,
						atlasDiagnostics.missingMetricsCount,
						atlasDiagnostics.zeroByteLengthCount,
						atlasDiagnostics.controlGlyphCount,
						atlasDiagnostics.spaceGlyphCount,
						atlasDiagnostics.firstCodePoint,
						atlasDiagnostics.firstEncodedCode,
						atlasDiagnostics.firstGlyphIndex,
						static_cast<UInt32>(atlasDiagnostics.firstByteLength),
						!atlasDiagnostics.inputGlyphCount ? "none"
							: atlasDiagnostics.firstByteClass
								== static_cast<UInt8>(VectorFontByteClass::DoubleByte)
									? "doubleByte" : "singleByte",
						m_impl->rasterScale, m_impl->prepareObject ? 1 : 0,
						m_impl->suppressEffects ? 1 : 0,
						atlasDiagnostics.wantsShaderPath ? 1 : 0,
						atlasDiagnostics.hasEffects ? 1 : 0,
						atlasDiagnostics.requestsSdfFill ? 1 : 0,
						atlasDiagnostics.a8RendererAvailable ? 1 : 0,
						static_cast<UInt32>(atlasDiagnostics.requestedQuality),
						static_cast<UInt32>(atlasDiagnostics.resolvedQuality),
						atlasDiagnostics.shaderQuadsBuilt ? 1 : 0,
						atlasDiagnostics.shaderQuadCount,
						atlasDiagnostics.shaderShapeAttempts,
						atlasDiagnostics.shaderAtlasOrShapeFailed ? 1 : 0,
						atlasDiagnostics.cpuQuadsBuilt ? 1 : 0,
						atlasDiagnostics.cpuQuadCount,
						atlasDiagnostics.cpuAttempts,
						GlyphAtlasMaskFailureName(atlasDiagnostics.cpuMaskFailure),
						atlasDiagnostics.degradedLayerCount,
						atlasDiagnostics.cpuShapeAttempts,
						vectorfont::IsNativeA8RendererAvailable() ? 1 : 0,
						vectorfont::GetNativeA8ShaderGeneration(), GetCurrentThreadId());
			}
			else if (logCount == maximumLogs)
			{
				++logCount;
				gLog.FormattedMessage(
					"tnvse_freetype_native: atlas-shape-build %s logs suppressed after %u entries",
					actualFailure ? "failure" : "expected-empty", maximumLogs);
			}
		}
		return emptyShape;
	}

	void BeginFreeTypeRichTextRender(NiNode* parent)
	{
		s_richTextContext = std::make_unique<RichTextVectorContext>();
		s_richTextContext->parent = parent;
		s_richTextContext->rasterScale = GetCanonicalFreeTypeRasterScale();
	}

	void EndFreeTypeRichTextRender()
	{
		if (!s_richTextContext)
			return;
		if (s_richTextContext->parent)
		{
			for (auto& builder : s_richTextContext->builders)
			{
				if (builder)
				{
					if (NiTriShape* shape = builder->Finish())
						s_richTextContext->parent->AttachChild(shape, true);
				}
			}
			for (auto& [font, builder] :
				s_richTextContext->fallbackBuilders)
			{
				if (builder)
				if (NiTriShape* shape = builder->Finish())
					s_richTextContext->parent->AttachChild(shape, true);
			}
		}
		s_richTextContext.reset();
	}

	bool AddFreeTypeRichTextGlyph(Font* font, const FontManager::CharData* character,
		const NiPoint3& pen, const NiColorA* color)
	{
		if (!s_richTextContext || !character || !IsFreeTypeFontActive(font)
			|| character->cChar == 1
			|| (character->xFilename.pString && character->xFilename.pString[0]))
		{
			return false;
		}

		char encoded[3] = { static_cast<char>(character->cChar), 0, 0 };
		UInt32 dbcsCode = 0;
		if (TryGetRichTextCharDbcs(character, dbcsCode))
		{
			encoded[0] = static_cast<char>((dbcsCode >> 8) & 0xFF);
			encoded[1] = static_cast<char>(dbcsCode & 0xFF);
		}

		VectorEncodedGlyph glyph;
		if (!DecodeFreeTypeGlyph(font, encoded, glyph))
			return false;

		std::unique_ptr<VectorTextBuilder>* builder = nullptr;
		const UInt32 fontId = font->iFontNum;
		if (fontId < RichTextVectorContext::kDirectFontSlots
			&& (!s_richTextContext->fonts[fontId]
				|| s_richTextContext->fonts[fontId] == font))
		{
			s_richTextContext->fonts[fontId] = font;
			builder = &s_richTextContext->builders[fontId];
		}
		else
		{
			builder = &s_richTextContext->fallbackBuilders[font];
		}
		if (!*builder)
			*builder = std::make_unique<VectorTextBuilder>(font, true,
				s_richTextContext->rasterScale, color);
		return (*builder)->IsAvailable()
			&& (*builder)->AddGlyph(glyph, pen, color);
	}
}
