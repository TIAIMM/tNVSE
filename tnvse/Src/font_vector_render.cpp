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
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook
{
	namespace
	{
		constexpr UInt32 kMaxChunkVertices = 65532;
		constexpr UInt32 kMaxChunkTriangles = 32766;
		constexpr UInt32 kGeometryFailureLogLimit = 64;
		constexpr float kLayerDepthStep = 0.01f;

		enum class VectorLayer : UInt8
		{
			Shadow = 0,
			Glow = 1,
			Outline = 2,
			Fill = 3,
		};

		struct PackedColor
		{
			std::array<UInt32, 4> channels = {};

			bool operator==(const PackedColor& other) const
			{
				return channels == other.channels;
			}
		};

		struct Chunk
		{
			std::vector<NiPoint3> vertices;
			std::vector<UInt16> indices;
		};

		struct ColorGroup
		{
			VectorLayer layer = VectorLayer::Fill;
			NiColorA color = { 1.0f, 1.0f, 1.0f, 1.0f };
			PackedColor packed;
			std::vector<Chunk> chunks;
		};

		NiTexturingProperty* s_whiteTextureProperty = nullptr;
		bool s_whiteTextureInitialized = false;
		bool s_whiteTextureAvailable = false;
		UInt32 s_geometryFailureLogCount = 0;
		std::mutex s_routeLogMutex;
		std::unordered_set<UInt64> s_loggedGlyphRoutes;
		std::array<bool, 4> s_loggedShapeDiagnostics = {};

		PackedColor PackColor(const NiColorA& color)
		{
			return { {
				std::bit_cast<UInt32>(color.r),
				std::bit_cast<UInt32>(color.g),
				std::bit_cast<UInt32>(color.b),
				std::bit_cast<UInt32>(color.a)
			} };
		}

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

		NiTriShape* MakeVectorTriShape(Font& font, UInt32 vertexCount, UInt32 triangleCount,
			const NiColorA& color, bool prepareObject)
		{
			if (!vertexCount || !triangleCount || !InitializeWhiteTexture())
				return nullptr;
			const UInt32 charCapacity = std::max((vertexCount + 3) / 4, (triangleCount + 1) / 2);
			if (!charCapacity || charCapacity > 16383)
				return nullptr;

			NiTriShape* shape = font.MakeTriShape(static_cast<int>(charCapacity), &color, false);
			if (!shape || !shape->GetModelData())
				return nullptr;

			// Font::MakeTriShape installs the original bitmap texture in both the
			// property state and the tile shader. Replace both after construction;
			// changing Font::pTextureData temporarily does not update the shape's
			// resolved property state reliably.
			shape->RemoveProperty(NiProperty::TEXTURING);
			shape->AddProperty(s_whiteTextureProperty);
			shape->UpdateProperties();
			NiShadeProperty* shade = shape->GetShadeProperty();
			NiTexture* whiteTexture = GetWhiteTexture();
			if (shade && shade->m_eShaderType == NiShadeProperty::PROP_Tile)
			{
				if (whiteTexture)
					ThisStdCall(0xBB7A10, shade, whiteTexture);
				*reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68) = color;
			}

			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		NiTriShape* CreateChunkShape(Font& font, const Chunk& chunk,
			VectorLayer layer, const NiColorA& color, bool prepareObject)
		{
			const UInt32 triangleCount = static_cast<UInt32>(chunk.indices.size() / 3);
			NiTriShape* shape = MakeVectorTriShape(font,
				static_cast<UInt32>(chunk.vertices.size()), triangleCount, color, false);
			if (!shape)
				return nullptr;

			NiTriShapeData* data = shape->GetModelData();
			const UInt32 capacityVertices = data->m_usVertices;
			const UInt32 capacityTriangles = data->m_usTriangles;
			const NiPoint3 paddingVertex = chunk.vertices.empty() ? NiPoint3{} : chunk.vertices.front();
			const float layerDepth = GetLayerDepthOffset(layer);
			for (UInt32 i = 0; i < capacityVertices; ++i)
			{
				data->m_pkVertex[i] = i < chunk.vertices.size() ? chunk.vertices[i] : paddingVertex;
				data->m_pkVertex[i].y += layerDepth;
				if (data->m_pkTexture)
					data->m_pkTexture[i] = NiPoint2(0.5f, 0.5f);
			}
			for (UInt32 i = 0; i < capacityTriangles * 3; ++i)
				data->m_pusTriList[i] = i < chunk.indices.size() ? chunk.indices[i] : 0;

			// The original bitmap font quads face -Y. libtess2 does not promise
			// that its output winding matches that Gamebryo UI convention, and
			// opposite-facing glyphs are removed by back-face culling.
			UInt32 flippedTriangles = 0;
			float firstWindingBefore = 0.0f;
			float firstWindingAfter = 0.0f;
			bool foundNonDegenerateTriangle = false;
			for (UInt32 triangle = 0; triangle < triangleCount; ++triangle)
			{
				UInt16* indices = &data->m_pusTriList[triangle * 3];
				if (indices[0] >= chunk.vertices.size()
					|| indices[1] >= chunk.vertices.size()
					|| indices[2] >= chunk.vertices.size())
				{
					continue;
				}

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
				const size_t layerIndex = static_cast<size_t>(layer);
				if (layerIndex < s_loggedShapeDiagnostics.size()
					&& !s_loggedShapeDiagnostics[layerIndex])
				{
					s_loggedShapeDiagnostics[layerIndex] = true;
					NiShadeProperty* shade = shape->GetShadeProperty();
					NiTexture* shaderTexture = nullptr;
					NiColorA shaderColor = {};
					if (shade && shade->m_eShaderType == NiShadeProperty::PROP_Tile)
					{
						shaderTexture = *reinterpret_cast<NiTexture**>(reinterpret_cast<UInt8*>(shade) + 0x60);
						shaderColor = *reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68);
					}
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: first vector shape layer=%s font=%u vertices=%u triangles=%u flipped=%u winding=%.4f->%.4f depth=%.3f propertyTexture=%p expectedProperty=%p shaderTexture=%p expectedTexture=%p requestedColor=(%.3f,%.3f,%.3f,%.3f) shaderColor=(%.3f,%.3f,%.3f,%.3f)",
						GetLayerName(layer), font.iFontNum,
						static_cast<UInt32>(chunk.vertices.size()), triangleCount,
						flippedTriangles, firstWindingBefore, firstWindingAfter, layerDepth,
						shape->GetTexturingProperty(), s_whiteTextureProperty,
						shaderTexture, GetWhiteTexture(),
						color.r, color.g, color.b, color.a,
						shaderColor.r, shaderColor.g, shaderColor.b, shaderColor.a);
				}
			}
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		ColorGroup& GetColorGroup(std::vector<ColorGroup>& groups, VectorLayer layer, const NiColorA& color)
		{
			const PackedColor packed = PackColor(color);
			for (ColorGroup& group : groups)
			{
				if (group.layer == layer && group.packed == packed)
					return group;
			}
			groups.push_back({ layer, color, packed, {} });
			return groups.back();
		}

		bool AppendMesh(ColorGroup& group, const vectorfont::GlyphMesh& mesh,
			const NiPoint3& pen, float offsetX, float offsetY)
		{
			const UInt32 meshVertices = static_cast<UInt32>(mesh.vertices.size());
			const UInt32 meshTriangles = static_cast<UInt32>(mesh.indices.size() / 3);
			if (!meshVertices || !meshTriangles)
				return true;
			if (meshVertices > kMaxChunkVertices || meshTriangles > kMaxChunkTriangles)
				return false;

			if (group.chunks.empty()
				|| group.chunks.back().vertices.size() + meshVertices > kMaxChunkVertices
				|| group.chunks.back().indices.size() / 3 + meshTriangles > kMaxChunkTriangles)
			{
				group.chunks.emplace_back();
			}

			Chunk& chunk = group.chunks.back();
			const UInt32 baseVertex = static_cast<UInt32>(chunk.vertices.size());
			chunk.vertices.reserve(chunk.vertices.size() + meshVertices);
			for (const vectorfont::MeshPoint& point : mesh.vertices)
			{
				chunk.vertices.emplace_back(
					pen.x + point.x + offsetX,
					pen.y,
					pen.z + point.y - offsetY);
			}
			chunk.indices.reserve(chunk.indices.size() + mesh.indices.size());
			for (UInt32 index : mesh.indices)
				chunk.indices.push_back(static_cast<UInt16>(baseVertex + index));
			return true;
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
		std::vector<ColorGroup> groups;

		Impl(Font* apFont, bool abPrepareObject)
			: font(apFont), prepareObject(abPrepareObject)
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

	VectorTextBuilder::VectorTextBuilder(Font* apFont, bool abPrepareObject)
		: m_impl(std::make_unique<Impl>(apFont, abPrepareObject))
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
					"tnvse_freetype_font: first vector glyph font=%u role=%s bytes=%u encoded=0x%04X codepoint=U+%04X",
					config.fontId,
					glyph.byteClass == VectorFontByteClass::DoubleByte ? "doubleByte" : "singleByte",
					glyph.byteLength, glyph.encodedCode, glyph.codePoint);
			}
		}
		std::shared_ptr<const vectorfont::GlyphMesh> fill =
			vectorfont::GetGlyphMesh(*m_impl->runtime, glyph, vectorfont::GlyphMeshType::Fill);
		if (!fill)
		{
			LogGeometryFailure(config.fontId, glyph.codePoint, "fill tessellation failed");
			return true;
		}

		if (config.shadow.enabled && !fill->vertices.empty())
		{
			ColorGroup& shadow = GetColorGroup(m_impl->groups, VectorLayer::Shadow,
				ResolveEffectColor(config.shadow, color));
			if (!AppendMesh(shadow, *fill, pen, config.shadow.x, config.shadow.y))
				LogGeometryFailure(config.fontId, glyph.codePoint, "shadow mesh exceeds chunk limits");
		}

		if (config.glow.enabled)
		{
			std::shared_ptr<const vectorfont::GlyphMesh> glow =
				vectorfont::GetGlyphMesh(*m_impl->runtime, glyph, vectorfont::GlyphMeshType::Glow);
			if (!glow)
			{
				LogGeometryFailure(config.fontId, glyph.codePoint, "glow tessellation failed");
			}
			else if (!glow->vertices.empty())
			{
				ColorGroup& glowGroup = GetColorGroup(m_impl->groups, VectorLayer::Glow,
					ResolveEffectColor(config.glow, color));
				if (!AppendMesh(glowGroup, *glow, pen, 0.0f, 0.0f))
					LogGeometryFailure(config.fontId, glyph.codePoint, "glow mesh exceeds chunk limits");
			}
		}

		if (config.outline.enabled)
		{
			std::shared_ptr<const vectorfont::GlyphMesh> outline =
				vectorfont::GetGlyphMesh(*m_impl->runtime, glyph, vectorfont::GlyphMeshType::Outline);
			if (!outline)
			{
				LogGeometryFailure(config.fontId, glyph.codePoint, "outline tessellation failed");
			}
			else if (!outline->vertices.empty())
			{
				ColorGroup& outlineGroup = GetColorGroup(m_impl->groups, VectorLayer::Outline,
					ResolveEffectColor(config.outline, color));
				if (!AppendMesh(outlineGroup, *outline, pen, 0.0f, 0.0f))
					LogGeometryFailure(config.fontId, glyph.codePoint, "outline mesh exceeds chunk limits");
			}
		}

		if (!fill->vertices.empty())
		{
			ColorGroup& fillGroup = GetColorGroup(m_impl->groups, VectorLayer::Fill,
				ResolveFillColor(config.fontColor, color));
			if (!AppendMesh(fillGroup, *fill, pen, 0.0f, 0.0f))
				LogGeometryFailure(config.fontId, glyph.codePoint, "fill mesh exceeds chunk limits");
		}
		return true;
	}

	NiNode* VectorTextBuilder::Finish()
	{
		if (!IsAvailable() || m_impl->finished)
			return nullptr;
		m_impl->finished = true;

		UInt32 childCount = 0;
		for (const ColorGroup& group : m_impl->groups)
			childCount += static_cast<UInt32>(group.chunks.size());
		NiNode* root = NiNode::Create(static_cast<UInt16>(std::min<UInt32>(childCount, 0xFFFF)));
		if (!root)
			return nullptr;

		for (UInt32 layer = static_cast<UInt32>(VectorLayer::Shadow);
			layer <= static_cast<UInt32>(VectorLayer::Fill); ++layer)
		{
			for (const ColorGroup& group : m_impl->groups)
			{
				if (static_cast<UInt32>(group.layer) != layer)
					continue;
				for (const Chunk& chunk : group.chunks)
				{
					NiTriShape* shape = CreateChunkShape(*m_impl->font, chunk,
						group.layer, group.color, m_impl->prepareObject);
					if (shape)
						root->AttachChild(shape, true);
				}
			}
		}
		return root;
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
				if (NiNode* node = builder->Finish())
					s_richTextContext->parent->AttachChild(node, true);
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
