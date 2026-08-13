#include "font_vector_internal.h"

#include "font_native_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "BSShaderProperty.hpp"
#include "MemoryManager.hpp"
#include "NiAlphaProperty.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiMemory.hpp"
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
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace fonthook
{
	namespace implementation::font_vector_render {}
	using namespace implementation::font_vector_render;

	namespace implementation::font_vector_render
	{
		inline constexpr UInt32 kBSScissorTriShapeSize = 0xD4;

		struct BSScissorTriShapeView : NiTriShape
		{
			UInt32 scissorTail[4];
		};

		// CommonLib does not expose the retail TileShaderProperty type. Keep this
		// construction-only view tied to the FalloutNV.exe layout used by the
		// native renderer's independently checked live-property view.
		struct TileShaderPropertyConstructionView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode;
			bool byte90;
			bool rotates;
			bool hasVertexColors;
			bool noTexture;
			BSStringT<char> texturePath;
			RECT scissorRect;
			bool useScissorTest;
		};

		static_assert(sizeof(NiTriShape) == 0xC4);
		static_assert(sizeof(BSScissorTriShapeView)
			== kBSScissorTriShapeSize);
		static_assert(sizeof(TileShaderPropertyConstructionView) == 0xB0);
		static_assert(offsetof(TileShaderPropertyConstructionView, overlayColor)
			== 0x68);

		// FalloutNV.exe 1.4.0.525 retail text-shape primitives. Keep each target
		// beside its exact calling convention so this factory remains auditable
		// without routing through Font::MakeTriShape.
		using BSScissorTriShapeConstructorFn = BSScissorTriShapeView* (__thiscall*)(
			BSScissorTriShapeView*, UInt16, NiPoint3*, NiPoint3*, NiColorA*,
			NiPoint2*, UInt16, UInt32, UInt16, UInt16*,
			SInt32, SInt32, SInt32, SInt32);
		inline constexpr UInt32 kBSScissorTriShapeConstructorAddress = 0xC5CF30;

		using NiGeometryDataSetConsistencyFn = void (__thiscall*)(
			NiGeometryData*, NiGeometryData::Consistency);
		inline constexpr UInt32 kNiGeometryDataSetConsistencyAddress = 0xA67050;

		using TileShaderPropertyConstructorFn =
			TileShaderPropertyConstructionView* (__thiscall*)(
				TileShaderPropertyConstructionView*, bool);
		inline constexpr UInt32 kTileShaderPropertyConstructorAddress = 0xBB7C30;

		using TileShaderPropertySetTextureFn = void (__thiscall*)(
			TileShaderPropertyConstructionView*, NiTexture*);
		inline constexpr UInt32 kTileShaderPropertySetTextureAddress = 0xBB7A10;

		using AttachDefaultTextAlphaPropertyFn = void (__cdecl*)(NiTriShape*);
		inline constexpr UInt32 kAttachDefaultTextAlphaPropertyAddress = 0x706D50;

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

			NiPixelData* pixelStorage = static_cast<NiPixelData*>(
				NiMemObject::operator new(sizeof(NiPixelData)));
			if (!pixelStorage)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture pixel allocation failed");
				return false;
			}
			// The engine entry is NiPixelData::NiPixelData (void), not a
			// nullable creation function. Validate the constructed fields below.
			ThisStdCall<void>(0xA7C190, pixelStorage, 1u, 1u,
				&NiPixelFormat_RGBA32, 1, 1);
			NiPixelData* pixelData = pixelStorage;
			NiPixelDataPtr pixelDataGuard = pixelData;
			if (!pixelData || !pixelData->m_pucPixels || !pixelData->m_puiOffsetInBytes)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture pixel initialization failed");
				return false;
			}
			*reinterpret_cast<UInt32*>(&pixelData->m_pucPixels[*pixelData->m_puiOffsetInBytes]) = 0xFFFFFFFFu;
			pixelData->bNoConvert = 1;

			NiTexturingProperty* propertyStorage = static_cast<NiTexturingProperty*>(
				NiMemObject::operator new(sizeof(NiTexturingProperty)));
			if (!propertyStorage)
			{
				gLog.FormattedMessage("tnvse_freetype_font: white texture property allocation failed");
				return false;
			}

			NiFixedString textureName;
			textureName.m_kHandle = static_cast<char*>(NiGlobalStringTable::AddString("tNVSE FreeType White"));
			ThisStdCall<void>(0xA6ABB0, propertyStorage, pixelData,
				&textureName, &formatPrefs);
			NiTexturingProperty* property = propertyStorage;
			const auto deleteProperty = [](NiTexturingProperty* value)
			{
				if (value)
					value->DeleteThis();
			};
			std::unique_ptr<NiTexturingProperty, decltype(deleteProperty)>
				propertyGuard(property, deleteProperty);
			if (!property->m_kMaps.GetSize()
				|| !property->m_kMaps[0]
				|| !property->m_kMaps[0]->m_spTexture)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: white texture property initialization failed");
				return false;
			}

			ThisStdCall<void>(0x60AEB0, property, 1);
			property->IncRefCount();
			s_whiteTextureProperty = property;
			propertyGuard.release();
			s_whiteTextureAvailable = s_whiteTextureProperty != nullptr;
			if (!s_whiteTextureAvailable)
				gLog.FormattedMessage("tnvse_freetype_font: failed to create 1x1 white text texture");
			return s_whiteTextureAvailable;
		}

		NiTexture* GetWhiteTexture()
		{
			if (!InitializeWhiteTexture() || !s_whiteTextureProperty
				|| !s_whiteTextureProperty->m_kMaps.GetSize()
				|| !s_whiteTextureProperty->m_kMaps[0])
			{
				return nullptr;
			}
			return s_whiteTextureProperty->m_kMaps[0]->m_spTexture;
		}

		NiTriShape* CreateEmptyVectorShape(Font* font, bool prepareObject)
		{
			if (!font)
				return nullptr;
			const NiColorA transparent = { 1.0f, 1.0f, 1.0f, 0.0f };
			NiTriShape* shape = CreateFreeTypePlaceholderTextShape(
				1, transparent, false);
			NiTriShapeData* data = shape ? shape->GetModelData() : nullptr;
			if (!data || data->m_usVertices < 4 || data->m_usTriangles < 2
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				if (shape)
					shape->DeleteThis();
				return nullptr;
			}

			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);
			for (UInt32 i = 0; i < data->m_usVertices; ++i)
			{
				data->m_pkVertex[i] = NiPoint3(0.0f, 0.0f, 0.0f);
				if (data->m_pkTexture)
					data->m_pkTexture[i] = NiPoint2(0.5f, 0.5f);
			}
			for (UInt32 i = 0; i < static_cast<UInt32>(data->m_usTriangles) * 3; ++i)
				data->m_pusTriList[i] = 0;
			ThisStdCall<void>(
				0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (prepareObject)
			{
				// This transparent fallback has no drawable payload. During LoadingMenu
				// prewarm presentation it must not re-enter renderer precache after the
				// native text route has already failed safely.
				if (IsFreeTypeNoPrecacheRouteActive())
					shape->PrepareObject(false, true);
				else
					shape->PrepareObject();
			}
			return shape;
		}

		struct RichTextVectorContext
		{
			static constexpr size_t kDirectFontSlots = 64;
			NiNode* parent = nullptr;
			float rasterScale = 1.0f;
			bool suppressEffects = false;
			std::array<Font*, kDirectFontSlots> fonts = {};
			std::array<std::optional<VectorTextBuilder>,
				kDirectFontSlots> builders;
			std::unordered_map<Font*, std::unique_ptr<VectorTextBuilder>>
				fallbackBuilders;
		};

		thread_local std::optional<RichTextVectorContext>
			s_richTextContext;
	}

	NiTriShape* CreateFreeTypeTextShape(UInt32 quadCount,
		const NiColorA& tileColor, bool prepareObject,
		NiTexturingProperty* textureProperty, NiTexture* texture)
	{
		constexpr UInt32 kMaximumQuadCount =
			std::numeric_limits<UInt16>::max() / 4u;
		if (!quadCount || quadCount > kMaximumQuadCount
			|| !textureProperty || !texture)
		{
			return nullptr;
		}

		const UInt32 vertexCount = quadCount * 4u;
		const UInt32 triangleCount = quadCount * 2u;
		const UInt32 indexCount = quadCount * 6u;
		MemoryManager* memory = MemoryManager::GetSingleton();
		NiPoint3* vertices = static_cast<NiPoint3*>(
			memory->Allocate(sizeof(NiPoint3) * vertexCount));
		NiPoint2* textureCoordinates = static_cast<NiPoint2*>(
			memory->Allocate(sizeof(NiPoint2) * vertexCount));
		UInt16* indices = NiAlloc<UInt16>(indexCount);
		if (!vertices || !textureCoordinates || !indices)
		{
			memory->Deallocate(vertices);
			memory->Deallocate(textureCoordinates);
			NiFree(indices);
			return nullptr;
		}

		void* shapeStorage =
			NiMemObject::operator new(kBSScissorTriShapeSize);
		if (!shapeStorage)
		{
			memory->Deallocate(vertices);
			memory->Deallocate(textureCoordinates);
			NiFree(indices);
			return nullptr;
		}
		auto* shape = reinterpret_cast<BSScissorTriShapeConstructorFn>(
			kBSScissorTriShapeConstructorAddress)(
			static_cast<BSScissorTriShapeView*>(shapeStorage),
			static_cast<UInt16>(vertexCount), vertices,
			nullptr, nullptr, textureCoordinates, 1u, 0u,
			static_cast<UInt16>(triangleCount), indices,
			0, 0, 0, 0);
		NiTriShapeData* data = shape ? shape->GetModelData() : nullptr;
		if (!shape || !data)
		{
			// The retail constructor transfers these three arrays only when its
			// NiTriShapeData allocation succeeds. A data-less shape owns none of
			// them, so release them explicitly after destroying the shape shell.
			if (shape)
				shape->DeleteThis();
			else
				NiMemObject::operator delete(shapeStorage,
					kBSScissorTriShapeSize);
			memory->Deallocate(vertices);
			memory->Deallocate(textureCoordinates);
			NiFree(indices);
			return nullptr;
		}

		data->bIsReadingData = false;
		data->bUnk3B = true;
		shape->SetName(NiFixedString("Interface: Text"));
		reinterpret_cast<NiGeometryDataSetConsistencyFn>(
			kNiGeometryDataSetConsistencyAddress)(
			data, NiGeometryData::STATIC);

		void* tileStorage = NiMemObject::operator new(
			sizeof(TileShaderPropertyConstructionView));
		if (!tileStorage)
		{
			shape->DeleteThis();
			return nullptr;
		}
		auto* tile = reinterpret_cast<TileShaderPropertyConstructorFn>(
			kTileShaderPropertyConstructorAddress)(
			static_cast<TileShaderPropertyConstructionView*>(tileStorage), false);
		if (!tile)
		{
			NiMemObject::operator delete(tileStorage,
				sizeof(TileShaderPropertyConstructionView));
			shape->DeleteThis();
			return nullptr;
		}
		shape->AddProperty(tile);
		tile->overlayColor = tileColor;
		if (prepareObject)
			shape->PrepareObject();
		reinterpret_cast<TileShaderPropertySetTextureFn>(
			kTileShaderPropertySetTextureAddress)(tile, texture);
		shape->UpdateProperties();
		reinterpret_cast<AttachDefaultTextAlphaPropertyFn>(
			kAttachDefaultTextAlphaPropertyAddress)(shape);

		// Preserve the old final property order: the retail Tile and shared Alpha
		// properties precede the FreeType atlas property.
		shape->AddProperty(textureProperty);
		shape->UpdateProperties();
		return shape;
	}

	NiTriShape* CreateFreeTypePlaceholderTextShape(UInt32 quadCount,
		const NiColorA& tileColor, bool prepareObject)
	{
		NiTexture* texture = GetWhiteTexture();
		return texture ? CreateFreeTypeTextShape(quadCount, tileColor,
			prepareObject, s_whiteTextureProperty, texture) : nullptr;
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

		bool ConvertDirectGlyphsToGeneric()
		{
			if (!font)
				return false;
			glyphs.clear();
			glyphs.reserve(directGlyphs.size() + 1u);
			for (const vectorfont::DirectGlyphCommand& command : directGlyphs)
			{
				char replay[3] = {};
				if (command.byteLength == 2)
				{
					replay[0] = static_cast<char>(command.encodedCode >> 8);
					replay[1] = static_cast<char>(command.encodedCode & 0xFF);
				}
				else
				{
					replay[0] = static_cast<char>(command.encodedCode & 0xFF);
				}
				VectorEncodedGlyph replayGlyph;
				if (!DecodeFreeTypeGlyph(font, replay, replayGlyph))
				{
					sealedBatchInvalid = true;
					glyphs.clear();
					sealedProfile.reset();
					return false;
				}
				glyphs.push_back({ replayGlyph, command.pen,
					command.sourceColor });
			}
			directGlyphs.clear();
			sealedProfile.reset();
			sealedBatchInvalid = false;
			return true;
		}

		Impl(Font* apFont, bool abPrepareObject, float afRasterScale,
			const NiColorA* apTileColor, bool abSuppressEffects)
			: font(apFont), prepareObject(abPrepareObject),
			suppressEffects(abSuppressEffects
				|| IsFreeTypeEffectSuppressionActive()),
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

	VectorTextBuilder::VectorTextBuilder(Font* apFont, bool abPrepareObject,
		float afRasterScale, const NiColorA* apTileColor,
		bool abSuppressEffects)
		: m_impl(std::make_unique<Impl>(apFont, abPrepareObject,
			afRasterScale, apTileColor, abSuppressEffects))
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
				vectorfont::InvalidateSealedDirectFontProfileIfCurrent(
					*m_impl->runtime, m_impl->sealedProfile);
				if (!m_impl->ConvertDirectGlyphsToGeneric())
					return false;
				m_impl->glyphs.push_back({ glyph, pen, sourceColor });
				return true;
			}
			vectorfont::DirectGlyphCommand command;
			command.pen = pen;
			command.sourceColor = sourceColor;
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
			if (lookup != vectorfont::SealedDirectGlyphLookup::Resolved)
			{
				if (lookup == vectorfont::SealedDirectGlyphLookup::Invalid)
				{
					vectorfont::InvalidateSealedDirectFontProfileIfCurrent(
						*m_impl->runtime, m_impl->sealedProfile);
				}
				if (!m_impl->ConvertDirectGlyphsToGeneric())
					return false;
				if (!DecodeFreeTypeGlyph(
					m_impl->font, encodedText, glyph))
				{
					return false;
				}
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

		vectorfont::GlyphAtlasBuildDiagnostics atlasDiagnosticsStorage;
		vectorfont::GlyphAtlasBuildDiagnostics* diagnostics =
			&atlasDiagnosticsStorage;
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
				vectorfont::InvalidateSealedDirectFontProfileIfCurrent(
					*m_impl->runtime, m_impl->sealedProfile);
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
		if (!atlasShape && m_impl->sealedProfile
			&& !m_impl->sealedBatchInvalid
			&& diagnostics->outcome
				== vectorfont::GlyphAtlasBuildOutcome::AtlasOrShapeFailure
			&& m_impl->ConvertDirectGlyphsToGeneric())
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
						? "tnvse_freetype_native: submission-suppressed reason=atlas-shape-build phase=shape-build classification=%s outcome=%s expectedEmpty=%u font=%u fontObject=%p runtime=%p emptyShape=%p glyphs=%u missingMetrics=%u zeroByteLength=%u controls=%u spaces=%u firstCodepoint=U+%04X firstEncoded=0x%04X firstGlyph=%u firstBytes=%u firstRole=%s scale=%.3f prepare=%u suppressEffects=%u wantsShader=%u hasEffects=%u sdfFill=%u nativeRenderer=%u requestedQuality=%u resolvedQuality=%u shaderBuilt=%u shaderQuads=%u shaderShapeAttempts=%u shaderShapeFailed=%u cpuBuilt=%u cpuQuads=%u cpuAttempts=%u cpuMaskFailure=%s degradedLayers=%u cpuShapeAttempts=%u nativeReady=%u nativeGeneration=%u thread=%u"
						: "tnvse_freetype_native: empty-shape reason=atlas-shape-build phase=shape-build classification=%s outcome=%s expectedEmpty=%u font=%u fontObject=%p runtime=%p emptyShape=%p glyphs=%u missingMetrics=%u zeroByteLength=%u controls=%u spaces=%u firstCodepoint=U+%04X firstEncoded=0x%04X firstGlyph=%u firstBytes=%u firstRole=%s scale=%.3f prepare=%u suppressEffects=%u wantsShader=%u hasEffects=%u sdfFill=%u nativeRenderer=%u requestedQuality=%u resolvedQuality=%u shaderBuilt=%u shaderQuads=%u shaderShapeAttempts=%u shaderShapeFailed=%u cpuBuilt=%u cpuQuads=%u cpuAttempts=%u cpuMaskFailure=%s degradedLayers=%u cpuShapeAttempts=%u nativeReady=%u nativeGeneration=%u thread=%u",
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
						atlasDiagnostics.nativeRendererAvailable ? 1 : 0,
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
						vectorfont::IsNativeFontRendererAvailable() ? 1 : 0,
						vectorfont::GetNativeFontShaderGeneration(), GetCurrentThreadId());
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
		s_richTextContext->suppressEffects =
			g_bDisableFreeTypeRichTextEffects;
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
					s_richTextContext->rasterScale, color,
					s_richTextContext->suppressEffects);
			return builder->IsAvailable()
				&& builder->AddEncodedGlyph(
					encoded, pen, color);
		}
		std::unique_ptr<VectorTextBuilder>& builder =
			s_richTextContext->fallbackBuilders[font];
		if (!builder)
			builder = std::make_unique<VectorTextBuilder>(font, true,
				s_richTextContext->rasterScale, color,
				s_richTextContext->suppressEffects);
		return builder->IsAvailable()
			&& builder->AddEncodedGlyph(
				encoded, pen, color);
	}
}
