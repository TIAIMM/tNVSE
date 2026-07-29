#include "font_vector_internal.h"

#include "font_native_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiNode.hpp"
#include "NiPixelData.hpp"
#include "NiPoint4.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
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

		// Font::MakeTriShape returns a BSScissorTriShape carrying a
		// TileShaderProperty. CommonLib does not expose those concrete types, so
		// keep this retail-layout view local and guarded by ABI assertions.
		struct StockTileShaderPropertyView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			bool byte90 = false;
			bool rotates = false;
			bool hasVertexColors = false;
			bool noTexture = false;
			BSStringT<char> texturePath;
			RECT scissorRect = {};
			bool useScissorTest = false;
		};

		inline constexpr UInt32 kStockScissorTailOffset = 0xC4;
		inline constexpr UInt32 kStockScissorTailSize = 0x10;
		static_assert(sizeof(NiTriShape) == kStockScissorTailOffset);
		static_assert(sizeof(StockTileShaderPropertyView) == 0xB0);
		static_assert(offsetof(
			StockTileShaderPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(
			StockTileShaderPropertyView, tileAlpha) == 0x78);

		StockTileShaderPropertyView* GetStockTileProperty(
			NiTriShape* shape)
		{
			NiShadeProperty* property =
				shape ? shape->GetShadeProperty() : nullptr;
			return property
				&& property->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<StockTileShaderPropertyView*>(property)
				: nullptr;
		}

		void CopyStockTileDynamicState(
			const StockTileShaderPropertyView& source,
			StockTileShaderPropertyView& destination)
		{
			destination.m_usFlags = source.m_usFlags;
			destination.ulFlags[0] = source.ulFlags[0];
			destination.ulFlags[1] = source.ulFlags[1];
			destination.fAlpha = source.fAlpha;
			destination.fFadeAlpha = source.fFadeAlpha;
			destination.fEnvMapScale = source.fEnvMapScale;
			destination.fLODFade = source.fLODFade;
			destination.fDepthBias = source.fDepthBias;
			destination.uiShaderIndex = source.uiShaderIndex;
			if (destination.alphaTexture.m_pObject
				!= source.alphaTexture.m_pObject)
			{
				destination.alphaTexture = source.alphaTexture;
			}
			destination.overlayColor = source.overlayColor;
			destination.tileAlpha = source.tileAlpha;
			destination.textureTransform = source.textureTransform;
			destination.clampMode = source.clampMode;
			destination.byte90 = source.byte90;
			destination.rotates = source.rotates;
			destination.hasVertexColors = source.hasVertexColors;
			destination.noTexture = source.noTexture;
			destination.scissorRect = source.scissorRect;
			destination.useScissorTest = source.useScissorTest;
			// sourceTexture and texturePath deliberately remain page-specific.
		}

		bool SynchronizeStockPageShapeState(
			const NiTriShape& primary, NiTriShape& pageShape)
		{
			const StockTileShaderPropertyView* sourceTile =
				GetStockTileProperty(const_cast<NiTriShape*>(&primary));
			StockTileShaderPropertyView* pageTile =
				GetStockTileProperty(&pageShape);
			if (!sourceTile || !pageTile || !pageTile->sourceTexture)
				return false;

			pageShape.m_kLocal = primary.m_kLocal;
			pageShape.m_kWorld = primary.m_kWorld;
			pageShape.m_uiFlags = primary.m_uiFlags;
			std::memcpy(
				reinterpret_cast<UInt8*>(&pageShape)
					+ kStockScissorTailOffset,
				reinterpret_cast<const UInt8*>(&primary)
					+ kStockScissorTailOffset,
				kStockScissorTailSize);

			pageShape.m_kProperties.m_spAlphaProperty =
				primary.m_kProperties.m_spAlphaProperty;
			pageShape.m_kProperties.m_spCullingProperty =
				primary.m_kProperties.m_spCullingProperty;
			pageShape.m_kProperties.m_spMaterialProperty =
				primary.m_kProperties.m_spMaterialProperty;
			pageShape.m_kProperties.m_spStencilProperty =
				primary.m_kProperties.m_spStencilProperty;
			pageShape.m_kProperties.m_spUnknownProperty =
				primary.m_kProperties.m_spUnknownProperty;
			// The Tile shade property and texturing property must remain unique:
			// they carry this physical page's atlas texture.
			CopyStockTileDynamicState(*sourceTile, *pageTile);
			if (primary.m_pWorldBound && pageShape.m_pWorldBound)
				*pageShape.m_pWorldBound = *primary.m_pWorldBound;
			else if (pageShape.m_pWorldBound)
				pageShape.UpdateWorldBound();
			return true;
		}

		UInt32 PackDirectCommandColor(const NiColorA& color)
		{
			auto channel = [](float value)
			{
				if (!std::isfinite(value))
					value = 1.0f;
				return static_cast<UInt32>(
					std::clamp(value, 0.0f, 1.0f)
					* 255.0f + 0.5f);
			};
			return (channel(color.a) << 24)
				| (channel(color.r) << 16)
				| (channel(color.g) << 8)
				| channel(color.b);
		}

		NiColorA UnpackDirectCommandColor(UInt32 color)
		{
			constexpr float inverse = 1.0f / 255.0f;
			return {
				static_cast<float>((color >> 16) & 0xFFu) * inverse,
				static_cast<float>((color >> 8) & 0xFFu) * inverse,
				static_cast<float>(color & 0xFFu) * inverse,
				static_cast<float>((color >> 24) & 0xFFu) * inverse
			};
		}

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

		struct StockPageShapeBatch
		{
			NiTriShape* primary = nullptr;
			std::vector<NiTriShapePtr> additionalShapes;
		};

		struct StockPageShapeCapture
		{
			std::vector<StockPageShapeBatch> batches;
		};

		struct RichTextVectorContext
		{
			static constexpr size_t kDirectFontSlots = 64;
			NiNode* parent = nullptr;
			float rasterScale = 1.0f;
			std::array<Font*, kDirectFontSlots> fonts = {};
			std::array<std::optional<VectorTextBuilder>,
				kDirectFontSlots> builders;
			std::unordered_map<Font*, std::unique_ptr<VectorTextBuilder>>
				fallbackBuilders;
			std::vector<StockPageShapeBatch> stockPageBatches;
		};

		thread_local std::optional<RichTextVectorContext>
			s_richTextContext;
		thread_local std::vector<StockPageShapeCapture>
			s_stockPageShapeCaptures;

		void AttachStockPageShapeBatches(
			std::vector<StockPageShapeBatch>& batches,
			NiNode* fallbackParent)
		{
			for (StockPageShapeBatch& batch : batches)
			{
				NiNode* parent = batch.primary
					? batch.primary->m_pkParent : nullptr;
				if (!parent)
					parent = fallbackParent;
				if (!parent || !batch.primary)
					continue;

				UInt32 insertionIndex = parent->GetArrayCount();
				bool foundPrimary = false;
				for (UInt32 index = 0;
					index < parent->GetArrayCount(); ++index)
				{
					if (parent->GetAt(index) == batch.primary)
					{
						insertionIndex = index + 1u;
						foundPrimary = true;
						break;
					}
				}

				// Font::CreateText/MakeString can return only one shape. Once the
				// game has attached that ABI primary, insert every packet sibling
				// immediately after it. Inserting at the actual child index avoids
				// AttachChild(firstAvail) filling an older hole and keeps multiple
				// captured text groups contiguous without changing the relative
				// order of unrelated children.
				for (NiTriShapePtr& pageShape : batch.additionalShapes)
				{
					if (!pageShape)
						continue;
					pageShape->m_kLocal = batch.primary->m_kLocal;
					pageShape->m_uiFlags = batch.primary->m_uiFlags;
					if (foundPrimary)
						parent->InsertChildAt(insertionIndex++, pageShape);
					else
						parent->AttachChild(pageShape, false);
					SynchronizeStockPageShapeState(
						*batch.primary, *pageShape);
				}
			}
			batches.clear();
		}

		void AttachStockPageShapeBatchForPrimary(
			std::vector<StockPageShapeBatch>& batches,
			NiTriShape* primary, NiNode* fallbackParent)
		{
			if (!primary)
				return;
			const auto found = std::find_if(
				batches.begin(), batches.end(),
				[primary](const StockPageShapeBatch& batch)
				{
					return batch.primary == primary;
				});
			if (found == batches.end())
				return;
			std::vector<StockPageShapeBatch> selected;
			selected.push_back(std::move(*found));
			batches.erase(found);
			AttachStockPageShapeBatches(selected, fallbackParent);
		}
	}

	struct VectorTextBuilder::Impl
	{
		Font* font = nullptr;
		vectorfont::RuntimeFont* runtime = nullptr;
		bool prepareObject = false;
		bool available = false;
		bool finished = false;
		bool suppressEffects = false;
		bool sealedBatchInvalid = false;
		float rasterScale = 1.0f;
		NiColorA tileColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::shared_ptr<const vectorfont::SealedDirectFontProfile>
			sealedProfile;
		std::vector<vectorfont::DirectGlyphCommand> directGlyphs;
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
			sealedProfile =
				vectorfont::AcquireSealedDirectFontProfile(
					*runtime, rasterScale);
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

	void BeginFreeTypeStockPageShapeCapture()
	{
		s_stockPageShapeCaptures.emplace_back();
	}

	void EndFreeTypeStockPageShapeCapture(NiNode* fallbackParent)
	{
		if (s_stockPageShapeCaptures.empty())
			return;
		StockPageShapeCapture capture =
			std::move(s_stockPageShapeCaptures.back());
		s_stockPageShapeCaptures.pop_back();
		AttachStockPageShapeBatches(capture.batches, fallbackParent);
	}

	bool CanUseFreeTypeStockPageShapes()
	{
		return s_richTextContext.has_value()
			|| !s_stockPageShapeCaptures.empty();
	}

	bool RegisterFreeTypeStockPageShapes(NiTriShape* primaryShape,
		const std::vector<NiTriShape*>& additionalShapes)
	{
		if (!primaryShape || additionalShapes.empty())
			return false;
		if (!s_richTextContext && s_stockPageShapeCaptures.empty())
			return false;

		StockPageShapeBatch batch;
		batch.primary = primaryShape;
		batch.additionalShapes.reserve(additionalShapes.size());
		for (NiTriShape* shape : additionalShapes)
		{
			if (!shape || shape == primaryShape)
				return false;
		}
		for (NiTriShape* shape : additionalShapes)
		{
			batch.additionalShapes.emplace_back(shape);
		}
		if (batch.additionalShapes.empty())
			return false;

		if (s_richTextContext)
		{
			s_richTextContext->stockPageBatches.push_back(
				std::move(batch));
			return true;
		}
		if (!s_stockPageShapeCaptures.empty())
		{
			s_stockPageShapeCaptures.back().batches.push_back(
				std::move(batch));
			return true;
		}
		return false;
	}

	bool SynchronizeFreeTypeStockPageShapeState(
		const NiTriShape& primaryShape, NiTriShape& pageShape)
	{
		return SynchronizeStockPageShapeState(primaryShape, pageShape);
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

	bool VectorTextBuilder::UsesSealedDirectProfile() const
	{
		return IsAvailable() && m_impl->sealedProfile != nullptr;
	}

	void VectorTextBuilder::ReserveGlyphs(size_t auiCount)
	{
		if (m_impl && m_impl->available)
		{
			if (m_impl->sealedProfile)
				m_impl->directGlyphs.reserve(auiCount);
			else
				m_impl->glyphs.reserve(auiCount);
		}
	}

	bool VectorTextBuilder::AddGlyph(const VectorEncodedGlyph& glyph,
		const NiPoint3& pen, const NiColorA* color)
	{
		if (!IsAvailable() || !HasVectorGlyphMetrics(glyph)
			|| !glyph.byteLength)
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
		if (m_impl->sealedProfile)
		{
			if (!glyph.hasDirectMetrics
				|| glyph.directSlot
					== std::numeric_limits<UInt16>::max()
				|| static_cast<size_t>(glyph.byteClass) >= 2)
			{
				m_impl->sealedBatchInvalid = true;
				return false;
			}
			vectorfont::DirectGlyphCommand command;
			command.pen = pen;
			command.packedColor =
				PackDirectCommandColor(sourceColor);
			command.directSlot = glyph.directSlot;
			command.encodedCode =
				static_cast<UInt16>(glyph.encodedCode);
			command.byteClass =
				static_cast<UInt8>(glyph.byteClass);
			command.byteLength = glyph.byteLength;
			m_impl->directGlyphs.push_back(command);
		}
		else
		{
			m_impl->glyphs.push_back({ glyph, pen, sourceColor });
		}
		return true;
	}

	bool VectorTextBuilder::AddEncodedGlyph(
		const char* encodedText, const NiPoint3& pen,
		const NiColorA* color, VectorEncodedGlyph* decodedGlyph)
	{
		if (!IsAvailable() || !encodedText || !*encodedText)
			return false;
		VectorEncodedGlyph glyph;
		if (m_impl->sealedProfile)
		{
			const vectorfont::SealedDirectGlyphLookup lookup =
				vectorfont::DecodeSealedDirectGlyph(
					*m_impl->sealedProfile,
					encodedText, glyph);
			if (lookup
				== vectorfont::SealedDirectGlyphLookup::Unavailable)
			{
				m_impl->glyphs.reserve(
					m_impl->directGlyphs.size() + 1u);
				for (const vectorfont::DirectGlyphCommand& command :
					m_impl->directGlyphs)
				{
					char replay[3] = {};
					if (command.byteLength == 2)
					{
						replay[0] = static_cast<char>(
							command.encodedCode >> 8);
						replay[1] = static_cast<char>(
							command.encodedCode & 0xFF);
					}
					else
						replay[0] = static_cast<char>(
							command.encodedCode & 0xFF);
					VectorEncodedGlyph replayGlyph;
					if (!DecodeFreeTypeGlyph(
						m_impl->font, replay, replayGlyph))
					{
						m_impl->sealedBatchInvalid = true;
						m_impl->glyphs.clear();
						m_impl->sealedProfile.reset();
						return false;
					}
					m_impl->glyphs.push_back({
						replayGlyph, command.pen,
						UnpackDirectCommandColor(
							command.packedColor) });
				}
				m_impl->directGlyphs.clear();
				m_impl->sealedProfile.reset();
				m_impl->sealedBatchInvalid = false;
				if (!DecodeFreeTypeGlyph(
					m_impl->font, encodedText, glyph))
				{
					return false;
				}
			}
			else if (lookup
				!= vectorfont::SealedDirectGlyphLookup::Resolved)
			{
				m_impl->sealedBatchInvalid = true;
				vectorfont::InvalidateSealedDirectFontProfile(
					*m_impl->runtime);
				return false;
			}
		}
		else if (!DecodeFreeTypeGlyph(
			m_impl->font, encodedText, glyph))
		{
			return false;
		}
		if (decodedGlyph)
			*decodedGlyph = glyph;
		return AddGlyph(glyph, pen, color);
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

		std::optional<vectorfont::GlyphAtlasBuildDiagnostics>
			atlasDiagnosticsStorage;
		if (g_bEnableFreeTypeFontRenderingLog)
			atlasDiagnosticsStorage.emplace();
		vectorfont::GlyphAtlasBuildDiagnostics* diagnostics =
			atlasDiagnosticsStorage
				? &*atlasDiagnosticsStorage : nullptr;
		NiTriShape* atlasShape = nullptr;
		if (m_impl->sealedProfile)
		{
			if (!m_impl->sealedBatchInvalid)
			{
				atlasShape =
					vectorfont::TryCreateSealedGlyphAtlasShape(
						*m_impl->font, *m_impl->runtime,
						m_impl->sealedProfile,
						m_impl->directGlyphs,
						m_impl->rasterScale,
						m_impl->prepareObject,
						m_impl->tileColor,
						m_impl->suppressEffects,
						diagnostics);
			}
			else
			{
				vectorfont::InvalidateSealedDirectFontProfile(
					*m_impl->runtime);
				if (diagnostics)
				{
					diagnostics->inputGlyphCount =
						static_cast<UInt32>(
							m_impl->directGlyphs.size());
					diagnostics->outcome =
						vectorfont::GlyphAtlasBuildOutcome::
							AtlasOrShapeFailure;
				}
			}
		}
		else
		{
			atlasShape = vectorfont::TryCreateGlyphAtlasShape(
				*m_impl->font, *m_impl->runtime, m_impl->glyphs,
				m_impl->rasterScale, m_impl->prepareObject,
				m_impl->tileColor, m_impl->suppressEffects,
				diagnostics);
		}
		if (atlasShape)
		{
			return atlasShape;
		}
		NiTriShape* emptyShape = CreateEmptyVectorShape(
			m_impl->font, m_impl->prepareObject);
		if (g_bEnableFreeTypeFontRenderingLog && diagnostics)
		{
			const vectorfont::GlyphAtlasBuildDiagnostics&
				atlasDiagnostics = *diagnostics;
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
		s_richTextContext.emplace();
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
					{
						s_richTextContext->parent->AttachChild(shape, true);
						AttachStockPageShapeBatchForPrimary(
							s_richTextContext->stockPageBatches,
							shape, s_richTextContext->parent);
					}
				}
			}
			for (auto& [font, builder] :
				s_richTextContext->fallbackBuilders)
			{
				if (builder)
					if (NiTriShape* shape = builder->Finish())
					{
						s_richTextContext->parent->AttachChild(shape, true);
						AttachStockPageShapeBatchForPrimary(
							s_richTextContext->stockPageBatches,
							shape, s_richTextContext->parent);
					}
			}
			AttachStockPageShapeBatches(
				s_richTextContext->stockPageBatches,
				s_richTextContext->parent);
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

		const UInt32 fontId = font->iFontNum;
		if (fontId < RichTextVectorContext::kDirectFontSlots
			&& (!s_richTextContext->fonts[fontId]
				|| s_richTextContext->fonts[fontId] == font))
		{
			s_richTextContext->fonts[fontId] = font;
			std::optional<VectorTextBuilder>& builder =
				s_richTextContext->builders[fontId];
			if (!builder)
				builder.emplace(font, true,
					s_richTextContext->rasterScale, color);
			return builder->IsAvailable()
				&& builder->AddEncodedGlyph(
					encoded, pen, color);
		}
		std::unique_ptr<VectorTextBuilder>& builder =
			s_richTextContext->fallbackBuilders[font];
		if (!builder)
			builder = std::make_unique<VectorTextBuilder>(font, true,
				s_richTextContext->rasterScale, color);
		return builder->IsAvailable()
			&& builder->AddEncodedGlyph(
				encoded, pen, color);
	}
}
