#pragma once

#include "font_atlas_internal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace fonthook::vectorfont
{
	struct NiTriShapeDeleter
	{
		void operator()(NiTriShape* shape) const
		{
			if (shape)
				shape->DeleteThis();
		}
	};

	using NiTriShapeOwner = std::unique_ptr<NiTriShape, NiTriShapeDeleter>;

	template <size_t LayerCount>
	UInt32 CountUsedDirectAtlasPages(
		const std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, LayerCount>& counts,
		size_t availablePageCount)
	{
		const size_t pageLimit = std::min(
			availablePageCount,
			static_cast<size_t>(kMaximumAtlasSnapshotPages));
		UInt32 usedPageCount = 0;
		for (size_t page = 0; page < pageLimit; ++page)
		{
			for (size_t layer = 0; layer < LayerCount; ++layer)
			{
				if (!counts[layer][page])
					continue;
				++usedPageCount;
				break;
			}
		}
		return usedPageCount;
	}

	template <class Value, size_t Capacity>
	bool InitializeDirectPagePrefix(
		std::array<Value, Capacity>& values, size_t availablePageCount,
		const Value& value, UInt64& initializationBytesAvoided)
	{
		if (!availablePageCount || availablePageCount > Capacity)
			return false;
		std::fill_n(values.begin(), availablePageCount, value);
		initializationBytesAvoided +=
			(Capacity - availablePageCount) * sizeof(Value);
		return true;
	}

	template <size_t RangeCount>
	bool InitializeDirectQuadCountPrefix(
		std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, RangeCount>& counts,
		size_t availablePageCount, UInt64& initializationBytesAvoided)
	{
		if (!availablePageCount
			|| availablePageCount > kMaximumAtlasSnapshotPages)
		{
			return false;
		}
		for (auto& range : counts)
			std::fill_n(range.begin(), availablePageCount, 0u);
		initializationBytesAvoided += RangeCount
			* (kMaximumAtlasSnapshotPages - availablePageCount)
			* sizeof(UInt32);
		return true;
	}

	template <size_t RangeCount>
	bool DirectQuadRangesCompletelyWritten(
		const std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, RangeCount>& counts,
		const std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, RangeCount>& offsets,
		const std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, RangeCount>& cursors,
		size_t availablePageCount, UInt32 expectedQuadCount)
	{
		const size_t pageLimit = std::min(
			availablePageCount,
			static_cast<size_t>(kMaximumAtlasSnapshotPages));
		UInt64 contiguousEnd = 0;
		for (size_t range = 0; range < RangeCount; ++range)
		{
			for (size_t page = 0; page < pageLimit; ++page)
			{
				if (offsets[range][page] != contiguousEnd)
					return false;
				contiguousEnd += counts[range][page];
				if (contiguousEnd > expectedQuadCount
					|| cursors[range][page] != contiguousEnd)
				{
					return false;
				}
			}
		}
		return contiguousEnd == expectedQuadCount;
	}

	struct CompositeGlyphQuadSource
	{
		UInt32 firstVertex = std::numeric_limits<UInt32>::max();
		UInt16 atlasPage = std::numeric_limits<UInt16>::max();
		UInt8 layerMask = 0;
	};

	struct CompositeConstructionProfile
	{
		float uniformSdfSpread = 0.0f;
		float uniformDistanceParameterScale = 0.0f;
		UInt8 staticLayerMask = 0;
	};

	NiColorA SanitizeColor(const NiColorA& color);
	UInt32 PackNativeBaseColor(const NiColorA& color);
	float SanitizeNativeUvBound(float value);
	NiColorA ResolveBaseColor(const NiColorA& source, const NiColorA& tile);
	NiColorA ResolveFillLayerColor(const FontColorStyle& style);
	NiColorA ResolveEffectLayerColor(const EffectStyle& effect,
		const FontColorStyle& fillStyle);
	NiColorA ComposeQuadColor(const PendingQuad& quad);
	bool EffectUsesLiveTileRgb(const EffectStyle& effect);
	void BuildNativeFontDrawRanges(const std::vector<PendingQuad>& quads,
		NativeFontEffectShapeConfig& config);
	void BuildBakedArgbFallback(const std::vector<PendingQuad>& source,
		const NiColorA& tileColor, std::vector<PendingQuad>& quads);
	bool ConfigureShaderEffectBuild(const FontConfig& config,
		float rasterScale, EffectQuality quality, bool suppressEffects,
		ShaderEffectBuild& build);

	bool WriteDirectQuadGeometry(const AtlasSnapshotPlacement& source,
		const NiPoint3& pen, const NiPoint3& origin,
		float offsetX, float offsetY, float rasterScale,
		float baselineOffset, float sourceToLogicalScale, bool usesSdf,
		NiPoint3* outputPositions, NiPoint2* outputTexture,
		NiPoint3& boundMinimum, NiPoint3& boundMaximum);
	bool WriteDirectQuadVertices(const AtlasSnapshotPlacement& source,
		const NiPoint3& pen, const NiPoint3& origin,
		float offsetX, float offsetY, float rasterScale,
		float baselineOffset, float sourceToLogicalScale, bool usesSdf,
		UInt32 packedColor, UInt8 layerMask,
		NativeFontGpuVertex* output, NiPoint3& boundMinimum,
		NiPoint3& boundMaximum);
	bool WriteVanillaDirectQuadGeometry(const FontLetter& letter,
		const NiPoint3& pen, const NiPoint3& origin,
		float baselineOffset, NiPoint3* outputPositions,
		NiPoint2* outputTexture, NiPoint3& boundMinimum,
		NiPoint3& boundMaximum);
	bool WriteVanillaDirectQuadVertices(const FontLetter& letter,
		const NiPoint3& pen, const NiPoint3& origin,
		float baselineOffset, UInt32 packedColor, UInt8 layerMask,
		NativeFontGpuVertex* output, NiPoint3& boundMinimum,
		NiPoint3& boundMaximum);
	bool IsValidCompositeConstructionProfile(
		const CompositeConstructionProfile& profile);
	bool VertexMatchesCompositeConstructionProfile(
		const NativeFontGpuVertex& vertex,
		const NativeFontGpuVertex& quadFirst,
		const CompositeConstructionProfile& profile);
	bool AppendOneGlyphCompositeQuads(
		std::vector<NativeFontGpuVertex>& vertices,
		const std::vector<CompositeGlyphQuadSource>& sources,
		UInt32 pageCount, const NativeFontEffectShapeConfig& effects,
		NiPoint3& boundMinimum, NiPoint3& boundMaximum,
		std::vector<NativeFontCompositeSpan>& spans,
		const CompositeConstructionProfile* constructionProfile);
	void ExtendColorContract(NativeFontShapeColorContract& contract,
		bool& initialized, const NiColorA& source);
	bool BuildDirectVertexBound(size_t vertexCount,
		const NiPoint3& minimum, const NiPoint3& maximum, NiBound& bound);
	bool PopulateDirectAtlasEffectPages(
		const std::vector<std::shared_ptr<AtlasResource>>& atlases,
		NativeFontEffectShapeConfig& effects);
	NiTriShape* CreateDirectNativePacketShell(
		const std::shared_ptr<AtlasResource>& atlas,
		const NativeFontPayloadTemplate& payload,
		const NativeFontPacketTemplate& packet,
		const NiColorA& facadeColor, const NiColorA& tileColor,
		const NiPoint3& origin, bool prepareObject);
	bool IsVanillaLayoutPayloadEligible(
		const NativeFontPayloadTemplate& payload,
		const NativeFontPacketTemplate*& packet,
		NativeFontVanillaLayoutKind& layoutKind);
	NiTriShape* TryCreateVanillaLayoutShape(Font& font,
		const std::vector<std::shared_ptr<AtlasResource>>& atlases,
		const NativeFontPayloadTemplatePtr& payload, UInt32 glyphCount,
		const NativeFontEffectShapeConfig& effects,
		const NativeFontShapeColorContract& colorContract,
		const NiColorA& tileColor, const NiPoint3& origin,
		bool prepareObject);
	NiTriShape* CreateDirectNativeShape(Font& font,
		const std::vector<std::shared_ptr<AtlasResource>>& atlases,
		std::vector<NativeFontGpuVertex>&& vertices,
		UInt32 glyphCount, UInt32 quadCount,
		NativeFontEffectShapeConfig& effects,
		const NativeFontShapeColorContract& colorContract,
		const NiColorA& facadeColor, const NiColorA& tileColor,
		const NiPoint3& origin, const NiPoint3& boundMinimum,
		const NiPoint3& boundMaximum, bool prepareObject,
		DirectShapeFailureStage& failureStage,
		std::vector<NativeFontCompositeSpan>&& compositeSpans = {});
	NiColorA UnpackNativeBaseColor(UInt32 color);
	const NiPoint3& GetDirectGlyphPen(const AtlasGlyphInstance& glyph);
	const NiPoint3& GetDirectGlyphPen(const DirectGlyphCommand& glyph);
	NiColorA GetDirectGlyphSourceColor(const AtlasGlyphInstance& glyph);
	NiColorA GetDirectGlyphSourceColor(const DirectGlyphCommand& glyph);
	VectorFontByteClass GetDirectGlyphByteClass(
		const AtlasGlyphInstance& glyph);
	VectorFontByteClass GetDirectGlyphByteClass(
		const DirectGlyphCommand& glyph);
	float GetDirectGlyphBaselineOffset(RuntimeFont& runtime,
		const DirectAtlasGlyphBatch& batch,
		const AtlasGlyphInstance& glyph);
	float GetDirectGlyphBaselineOffset(RuntimeFont& runtime,
		const DirectAtlasGlyphBatch& batch,
		const DirectGlyphCommand& glyph);
	NativeFontPayloadTemplatePtr BuildNativeTextArtifact(Font& font,
		const std::vector<PendingQuad>& quads,
		const std::vector<std::shared_ptr<AtlasResource>>& atlases,
		const NiPoint3& origin, const NativeFontEffectShapeConfig& effects);
	NiTriShape* CreateAtlasShape(Font& font,
		const std::vector<PendingQuad>& quads,
		const std::vector<std::shared_ptr<AtlasResource>>& atlases,
		bool prepareObject, const NiColorA& tileColor,
		bool useNativeFontShader,
		const NativeFontEffectShapeConfig* effectConfig,
		const NiPoint3& origin);
}
