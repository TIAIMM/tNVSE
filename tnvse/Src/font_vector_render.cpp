#include "font_vector_internal.h"

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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook
{
	namespace
	{
		constexpr UInt32 kMaxShapeVertices = 65532;
		constexpr UInt32 kMaxShapeTriangles = 32766;
		constexpr UInt32 kGeometryFailureLogLimit = 64;
		constexpr float kLayerDepthStep = 0.01f;

		enum class VectorLayer : UInt8
		{
			Shadow = 0,
			Glow = 1,
			Outline = 2,
			Fill = 3,
		};

		constexpr size_t kVectorLayerCount = 4;

		struct LayerGeometry
		{
			std::vector<NiPoint3> vertices;
			std::vector<NiColorA> colors;
			std::vector<UInt32> indices;
		};

		NiTexturingProperty* s_whiteTextureProperty = nullptr;
		bool s_whiteTextureInitialized = false;
		bool s_whiteTextureAvailable = false;
		UInt32 s_geometryFailureLogCount = 0;
		std::mutex s_routeLogMutex;
		std::unordered_set<UInt64> s_loggedGlyphRoutes;
		bool s_loggedMergedShapeDiagnostics = false;

		NiColorA ResolveFillColor(const vectorfont::FontColorStyle& style, const NiColorA* source)
		{
			if (!style.configured)
				return source ? *source : NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
			NiColorA result = style.color;
			result.a *= source ? source->a : 1.0f;
			return result;
		}

		NiColorA ResolveEffectColor(const vectorfont::EffectStyle& effect, const NiColorA* source)
		{
			NiColorA result = effect.color;
			result.a *= source ? source->a : 1.0f;
			return result;
		}

		const char* GetLayerName(VectorLayer layer)
		{
			switch (layer)
			{
			case VectorLayer::Shadow:
				return "shadow";
			case VectorLayer::Glow:
				return "glow";
			case VectorLayer::Outline:
				return "outline";
			case VectorLayer::Fill:
				return "font";
			default:
				return "unknown";
			}
		}

		float GetLayerDepthOffset(VectorLayer layer)
		{
			// Tile text faces -Y, so positive Y moves effect layers behind fill.
			return static_cast<float>(static_cast<UInt32>(VectorLayer::Fill)
				- static_cast<UInt32>(layer)) * kLayerDepthStep;
		}

		NiTexture* GetWhiteTexture()
		{
			if (!s_whiteTextureProperty || !s_whiteTextureProperty->m_kMaps.GetSize())
				return nullptr;
			NiTexturingProperty::Map* map = s_whiteTextureProperty->m_kMaps[0];
			return map ? map->m_spTexture : nullptr;
		}

		void LogGeometryFailure(UInt32 fontId, UInt32 codePoint, const char* reason)
		{
			if (s_geometryFailureLogCount >= kGeometryFailureLogLimit)
				return;
			++s_geometryFailureLogCount;
			gLog.FormattedMessage(
				"tnvse_freetype_font: skipped glyph geometry font=%u codepoint=U+%04X reason=%s",
				fontId, codePoint, reason);
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

		bool AppendMesh(LayerGeometry& layerGeometry, VectorLayer layer,
			const vectorfont::GlyphMesh& mesh, const NiPoint3& pen,
			const NiColorA& color, float offsetX, float offsetY)
		{
			const size_t meshVertices = mesh.vertices.size();
			const size_t meshTriangles = mesh.indices.size() / 3;
			if (!meshVertices || !meshTriangles)
				return true;
			if (layerGeometry.vertices.size() > UINT32_MAX - meshVertices)
				return false;

			const UInt32 baseVertex = static_cast<UInt32>(layerGeometry.vertices.size());
			for (UInt32 index : mesh.indices)
			{
				if (index >= meshVertices || baseVertex > UINT32_MAX - index)
					return false;
			}
			layerGeometry.vertices.reserve(layerGeometry.vertices.size() + meshVertices);
			layerGeometry.colors.reserve(layerGeometry.colors.size() + meshVertices);
			const float layerDepth = GetLayerDepthOffset(layer);
			for (const vectorfont::MeshPoint& point : mesh.vertices)
			{
				layerGeometry.vertices.emplace_back(
					pen.x + point.x + offsetX,
					pen.y + layerDepth,
					pen.z + point.y - offsetY);
				layerGeometry.colors.push_back(color);
			}

			layerGeometry.indices.reserve(layerGeometry.indices.size() + mesh.indices.size());
			for (UInt32 index : mesh.indices)
				layerGeometry.indices.push_back(baseVertex + index);
			return true;
		}

		bool FitsSingleShape(const std::array<LayerGeometry, kVectorLayerCount>& layers,
			const std::array<bool, kVectorLayerCount>& included,
			UInt32& vertexCount, UInt32& triangleCount)
		{
			size_t vertices = 0;
			size_t triangles = 0;
			for (size_t i = 0; i < layers.size(); ++i)
			{
				if (!included[i])
					continue;
				vertices += layers[i].vertices.size();
				triangles += layers[i].indices.size() / 3;
			}
			vertexCount = vertices <= UINT32_MAX ? static_cast<UInt32>(vertices) : UINT32_MAX;
			triangleCount = triangles <= UINT32_MAX ? static_cast<UInt32>(triangles) : UINT32_MAX;
			return vertices <= kMaxShapeVertices && triangles <= kMaxShapeTriangles;
		}

		NiTriShape* CreateMergedShape(Font& font,
			const std::array<LayerGeometry, kVectorLayerCount>& layers,
			const std::array<bool, kVectorLayerCount>& included,
			UInt32 vertexCount, UInt32 triangleCount, bool prepareObject)
		{
			if (!vertexCount || !triangleCount || !InitializeWhiteTexture())
				return CreateEmptyVectorShape(&font, prepareObject);

			const UInt32 charCapacity = std::max((vertexCount + 3) / 4, (triangleCount + 1) / 2);
			if (!charCapacity || charCapacity > 16383)
				return CreateEmptyVectorShape(&font, prepareObject);

			const UInt32 capacityVertices = charCapacity * 4;
			NiColorA* vertexColors = static_cast<NiColorA*>(
				MemoryManager_s_Instance->Allocate(sizeof(NiColorA) * capacityVertices));
			if (!vertexColors)
				return CreateEmptyVectorShape(&font, prepareObject);

			const NiColorA white = { 1.0f, 1.0f, 1.0f, 1.0f };
			NiTriShape* shape = font.MakeTriShape(static_cast<int>(charCapacity), &white, false);
			if (!shape || !shape->GetModelData())
			{
				MemoryManager_s_Instance->Deallocate(vertexColors);
				return CreateEmptyVectorShape(&font, prepareObject);
			}

			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);
			shape->RemoveProperty(NiProperty::TEXTURING);
			shape->AddProperty(s_whiteTextureProperty);
			shape->UpdateProperties();
			NiShadeProperty* shade = shape->GetShadeProperty();
			NiTexture* whiteTexture = GetWhiteTexture();
			if (shade && shade->m_eShaderType == NiShadeProperty::PROP_Tile)
			{
				if (whiteTexture)
					ThisStdCall(0xBB7A10, shade, whiteTexture);
				*reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68) = white;
			}

			NiTriShapeData* data = shape->GetModelData();
			data->m_pkColor = vertexColors;
			data->m_ucKeepFlags |= NiGeometryData::KEEP_COLOR;
			const NiColorA transparent = { 1.0f, 1.0f, 1.0f, 0.0f };
			for (UInt32 i = 0; i < data->m_usVertices; ++i)
			{
				data->m_pkVertex[i] = NiPoint3(0.0f, 0.0f, 0.0f);
				data->m_pkColor[i] = transparent;
				if (data->m_pkTexture)
					data->m_pkTexture[i] = NiPoint2(0.5f, 0.5f);
			}
			for (UInt32 i = 0; i < static_cast<UInt32>(data->m_usTriangles) * 3; ++i)
				data->m_pusTriList[i] = 0;

			UInt32 vertexCursor = 0;
			UInt32 indexCursor = 0;
			for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex)
			{
				if (!included[layerIndex])
					continue;
				const LayerGeometry& layer = layers[layerIndex];
				const UInt32 baseVertex = vertexCursor;
				for (size_t i = 0; i < layer.vertices.size(); ++i)
				{
					data->m_pkVertex[vertexCursor] = layer.vertices[i];
					data->m_pkColor[vertexCursor] = layer.colors[i];
					++vertexCursor;
				}
				for (UInt32 index : layer.indices)
					data->m_pusTriList[indexCursor++] = static_cast<UInt16>(baseVertex + index);
			}

			UInt32 flippedTriangles = 0;
			float firstWindingBefore = 0.0f;
			float firstWindingAfter = 0.0f;
			bool foundNonDegenerateTriangle = false;
			for (UInt32 triangle = 0; triangle < triangleCount; ++triangle)
			{
				UInt16* indices = &data->m_pusTriList[triangle * 3];
				const NiPoint3& v0 = data->m_pkVertex[indices[0]];
				const NiPoint3& v1 = data->m_pkVertex[indices[1]];
				const NiPoint3& v2 = data->m_pkVertex[indices[2]];
				const float winding = (v1.z - v0.z) * (v2.x - v0.x)
					- (v1.x - v0.x) * (v2.z - v0.z);
				if (!foundNonDegenerateTriangle && std::abs(winding) > 0.000001f)
				{
					firstWindingBefore = winding;
					foundNonDegenerateTriangle = true;
				}
				if (winding > 0.0f)
				{
					std::swap(indices[1], indices[2]);
					++flippedTriangles;
				}
				if (foundNonDegenerateTriangle && firstWindingAfter == 0.0f
					&& std::abs(winding) > 0.000001f)
				{
					firstWindingAfter = winding > 0.0f ? -winding : winding;
				}
			}

			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				std::lock_guard<std::mutex> lock(s_routeLogMutex);
				if (!s_loggedMergedShapeDiagnostics)
				{
					s_loggedMergedShapeDiagnostics = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first merged shape font=%u vertices=%u triangles=%u shadow=%d glow=%d outline=%d fill=%d flipped=%u winding=%.4f->%.4f colors=%p local=(%.3f,%.3f,%.3f)",
						font.iFontNum, vertexCount, triangleCount,
						included[static_cast<size_t>(VectorLayer::Shadow)] ? 1 : 0,
						included[static_cast<size_t>(VectorLayer::Glow)] ? 1 : 0,
						included[static_cast<size_t>(VectorLayer::Outline)] ? 1 : 0,
						included[static_cast<size_t>(VectorLayer::Fill)] ? 1 : 0,
						flippedTriangles,
						firstWindingBefore, firstWindingAfter, data->m_pkColor,
						shape->m_kLocal.m_Translate.x,
						shape->m_kLocal.m_Translate.y,
						shape->m_kLocal.m_Translate.z);
				}
			}
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		struct RichTextVectorContext
		{
			NiNode* parent = nullptr;
			std::unordered_map<Font*, std::unique_ptr<VectorTextBuilder>> builders;
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
		float rasterScale = 1.0f;
		std::array<LayerGeometry, kVectorLayerCount> layers;
		std::vector<vectorfont::AtlasGlyphInstance> glyphs;

		Impl(Font* apFont, bool abPrepareObject, float afRasterScale)
			: font(apFont), prepareObject(abPrepareObject),
			rasterScale(std::isfinite(afRasterScale) && afRasterScale >= 0.1f
				&& afRasterScale <= 10.0f ? afRasterScale : 1.0f)
		{
			if (!font || !IsFreeTypeFontActive(font) || !InitializeWhiteTexture())
				return;
			runtime = vectorfont::FindRuntimeFont(font->iFontNum);
			available = runtime != nullptr;
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

	VectorTextBuilder::VectorTextBuilder(Font* apFont, bool abPrepareObject, float afRasterScale)
		: m_impl(std::make_unique<Impl>(apFont, abPrepareObject, afRasterScale))
	{
	}

	VectorTextBuilder::~VectorTextBuilder() = default;

	bool VectorTextBuilder::IsAvailable() const
	{
		return m_impl && m_impl->available;
	}

	bool VectorTextBuilder::AddGlyph(const VectorEncodedGlyph& glyph,
		const NiPoint3& pen, const NiColorA* color)
	{
		if (!IsAvailable() || !glyph.metrics || !glyph.byteLength)
			return false;
		const vectorfont::FontConfig& config = vectorfont::GetRuntimeConfig(*m_impl->runtime);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
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
		const NiColorA sourceColor = color ? *color : NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
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

		if (NiTriShape* atlasShape = vectorfont::TryCreateGlyphAtlasShape(
			*m_impl->font, *m_impl->runtime, m_impl->glyphs,
			m_impl->rasterScale, m_impl->prepareObject))
		{
			return atlasShape;
		}

		// Build the old outline geometry only when atlas creation failed.
		const vectorfont::FontConfig& config = vectorfont::GetRuntimeConfig(*m_impl->runtime);
		for (const vectorfont::AtlasGlyphInstance& instance : m_impl->glyphs)
		{
			const NiColorA* color = &instance.color;
			std::shared_ptr<const vectorfont::GlyphMesh> fill = vectorfont::GetGlyphMesh(
				*m_impl->runtime, instance.glyph, vectorfont::GlyphMeshType::Fill);
			if (!fill)
			{
				LogGeometryFailure(config.fontId, instance.glyph.codePoint,
					"fill tessellation failed");
				continue;
			}
			if (config.shadow.enabled && !fill->vertices.empty())
			{
				AppendMesh(m_impl->layers[static_cast<size_t>(VectorLayer::Shadow)],
					VectorLayer::Shadow, *fill, instance.pen,
					ResolveEffectColor(config.shadow, color),
					config.shadow.x, config.shadow.y);
			}
			if (config.glow.enabled)
			{
				auto glow = vectorfont::GetGlyphMesh(*m_impl->runtime,
					instance.glyph, vectorfont::GlyphMeshType::Glow);
				if (glow && !glow->vertices.empty())
					AppendMesh(m_impl->layers[static_cast<size_t>(VectorLayer::Glow)],
						VectorLayer::Glow, *glow, instance.pen,
						ResolveEffectColor(config.glow, color), 0.0f, 0.0f);
			}
			if (config.outline.enabled)
			{
				auto outline = vectorfont::GetGlyphMesh(*m_impl->runtime,
					instance.glyph, vectorfont::GlyphMeshType::Outline);
				if (outline && !outline->vertices.empty())
					AppendMesh(m_impl->layers[static_cast<size_t>(VectorLayer::Outline)],
						VectorLayer::Outline, *outline, instance.pen,
						ResolveEffectColor(config.outline, color), 0.0f, 0.0f);
			}
			if (!fill->vertices.empty())
			{
				AppendMesh(m_impl->layers[static_cast<size_t>(VectorLayer::Fill)],
					VectorLayer::Fill, *fill, instance.pen,
					ResolveFillColor(config.fontColor, color), 0.0f, 0.0f);
			}
		}

		std::array<bool, kVectorLayerCount> included = {};
		for (size_t i = 0; i < included.size(); ++i)
			included[i] = !m_impl->layers[i].vertices.empty();

		UInt32 vertexCount = 0;
		UInt32 triangleCount = 0;
		std::array<bool, kVectorLayerCount> dropped = {};
		const std::array<VectorLayer, 3> degradationOrder = {
			VectorLayer::Glow,
			VectorLayer::Shadow,
			VectorLayer::Outline,
		};
		for (VectorLayer layer : degradationOrder)
		{
			if (FitsSingleShape(m_impl->layers, included, vertexCount, triangleCount))
				break;
			const size_t layerIndex = static_cast<size_t>(layer);
			if (included[layerIndex])
			{
				included[layerIndex] = false;
				dropped[layerIndex] = true;
			}
		}

		if (!FitsSingleShape(m_impl->layers, included, vertexCount, triangleCount))
		{
			if (s_geometryFailureLogCount < kGeometryFailureLogLimit)
			{
				++s_geometryFailureLogCount;
				gLog.FormattedMessage(
					"tnvse_freetype_font: merged fill exceeds single-shape limit font=%u vertices=%u triangles=%u; returning empty shape",
					m_impl->font->iFontNum, vertexCount, triangleCount);
			}
			return CreateEmptyVectorShape(m_impl->font, m_impl->prepareObject);
		}

		if (g_bEnableFreeTypeFontRenderingLog
			&& (dropped[static_cast<size_t>(VectorLayer::Glow)]
				|| dropped[static_cast<size_t>(VectorLayer::Shadow)]
				|| dropped[static_cast<size_t>(VectorLayer::Outline)]))
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: merged shape degraded font=%u dropGlow=%d dropShadow=%d dropOutline=%d vertices=%u triangles=%u",
				m_impl->font->iFontNum,
				dropped[static_cast<size_t>(VectorLayer::Glow)] ? 1 : 0,
				dropped[static_cast<size_t>(VectorLayer::Shadow)] ? 1 : 0,
				dropped[static_cast<size_t>(VectorLayer::Outline)] ? 1 : 0,
				vertexCount, triangleCount);
		}

		return CreateMergedShape(*m_impl->font, m_impl->layers, included,
			vertexCount, triangleCount, m_impl->prepareObject);
	}

	void BeginFreeTypeRichTextRender(NiNode* parent)
	{
		s_richTextContext = std::make_unique<RichTextVectorContext>();
		s_richTextContext->parent = parent;
	}

	void EndFreeTypeRichTextRender()
	{
		if (!s_richTextContext)
			return;
		if (s_richTextContext->parent)
		{
			for (auto& [font, builder] : s_richTextContext->builders)
			{
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
			return true;

		auto& builder = s_richTextContext->builders[font];
		if (!builder)
			builder = std::make_unique<VectorTextBuilder>(font, true);
		if (builder->IsAvailable())
			builder->AddGlyph(glyph, pen, color);
		return true;
	}
}
