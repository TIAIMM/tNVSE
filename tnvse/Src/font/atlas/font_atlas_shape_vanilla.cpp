#include "font_atlas_shape_detail.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiDX9TextureData.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace fonthook::vectorfont
{
		const char* DirectShapeFailureStageName(
			DirectShapeFailureStage stage) noexcept
		{
			switch (stage)
			{
			case DirectShapeFailureStage::None:
				return "none";
			case DirectShapeFailureStage::CachedUnsupportedMaskType:
				return "cached-unsupported-mask-type";
			case DirectShapeFailureStage::CachedAtlasListEmpty:
				return "cached-atlas-list-empty";
			case DirectShapeFailureStage::CachedAtlasPageLimitExceeded:
				return "cached-atlas-page-limit-exceeded";
			case DirectShapeFailureStage::CachedGlyphBatchSizeMismatch:
				return "cached-glyph-batch-size-mismatch";
			case DirectShapeFailureStage::CachedDrawableGlyphCountZero:
				return "cached-drawable-glyph-count-zero";
			case DirectShapeFailureStage::CachedDrawableGlyphLimitExceeded:
				return "cached-drawable-glyph-limit-exceeded";
			case DirectShapeFailureStage::
				CachedArgbPagePrefixInitializationFailed:
				return "cached-argb-page-prefix-init-failed";
			case DirectShapeFailureStage::CachedArgbGlyphSourceMissing:
				return "cached-argb-glyph-source-missing";
			case DirectShapeFailureStage::CachedArgbAtlasPageOutOfRange:
				return "cached-argb-atlas-page-out-of-range";
			case DirectShapeFailureStage::CachedArgbPagePrefixOutOfRange:
				return "cached-argb-page-prefix-out-of-range";
			case DirectShapeFailureStage::CachedArgbPageShapeCreationFailed:
				return "cached-argb-page-shape-creation-failed";
			case DirectShapeFailureStage::CachedArgbPageShapeMissing:
				return "cached-argb-page-shape-missing";
			case DirectShapeFailureStage::CachedShaderEffectConfigurationFailed:
				return "cached-shader-effect-config-failed";
			case DirectShapeFailureStage::CachedGlyphRoleOutOfRange:
				return "cached-glyph-role-out-of-range";
			case DirectShapeFailureStage::CachedRasterProfileResolutionFailed:
				return "cached-raster-profile-resolution-failed";
			case DirectShapeFailureStage::
				CachedQuadCountPrefixInitializationFailed:
				return "cached-quad-count-prefix-init-failed";
			case DirectShapeFailureStage::CachedGlyphSourceMissing:
				return "cached-glyph-source-missing";
			case DirectShapeFailureStage::CachedGlyphAtlasPageOutOfRange:
				return "cached-glyph-atlas-page-out-of-range";
			case DirectShapeFailureStage::CachedGlyphPageLimitExceeded:
				return "cached-glyph-page-limit-exceeded";
			case DirectShapeFailureStage::CachedPhysicalQuadLimitExceeded:
				return "cached-physical-quad-limit-exceeded";
			case DirectShapeFailureStage::CachedPhysicalQuadCountZero:
				return "cached-physical-quad-count-zero";
			case DirectShapeFailureStage::
				CachedPageProfilePrefixInitializationFailed:
				return "cached-page-profile-prefix-init-failed";
			case DirectShapeFailureStage::CachedRasterProfileMissing:
				return "cached-raster-profile-missing";
			case DirectShapeFailureStage::CachedPageRasterProfileMixed:
				return "cached-page-raster-profile-mixed";
			case DirectShapeFailureStage::CachedShadowQuadWriteFailed:
				return "cached-shadow-quad-write-failed";
			case DirectShapeFailureStage::CachedBodyQuadWriteFailed:
				return "cached-body-quad-write-failed";
			case DirectShapeFailureStage::CachedQuadRangeCoverageFailed:
				return "cached-quad-range-coverage-failed";
			case DirectShapeFailureStage::CachedColorContractMissing:
				return "cached-color-contract-missing";
			case DirectShapeFailureStage::CachedDrawRangesEmpty:
				return "cached-draw-ranges-empty";
			case DirectShapeFailureStage::NativeQuadCountZero:
				return "native-quad-count-zero";
			case DirectShapeFailureStage::NativeVertexCountTooSmall:
				return "native-vertex-count-too-small";
			case DirectShapeFailureStage::NativeEffectPagePopulationFailed:
				return "native-effect-page-population-failed";
			case DirectShapeFailureStage::NativeVertexBoundFailed:
				return "native-vertex-bound-failed";
			case DirectShapeFailureStage::NativePayloadBuildFailed:
				return "native-payload-build-failed";
			case DirectShapeFailureStage::NativePayloadVertexCountTooSmall:
				return "native-payload-vertex-count-too-small";
			case DirectShapeFailureStage::NativePayloadPacketsEmpty:
				return "native-payload-packets-empty";
			case DirectShapeFailureStage::NativeFacadeAtlasPageOutOfRange:
				return "native-facade-atlas-page-out-of-range";
			case DirectShapeFailureStage::NativePacketShellCreationFailed:
				return "native-packet-shell-creation-failed";
			case DirectShapeFailureStage::NativeShapePreparationFailed:
				return "native-shape-preparation-failed";
			default:
				return "unknown";
			}
		}

		bool IsVanillaLayoutPayloadEligible(
			const NativeFontPayloadTemplate& payload,
			const NativeFontPacketTemplate*& packet,
			NativeFontVanillaLayoutKind& layoutKind)
		{
			packet = nullptr;
			layoutKind = NativeFontVanillaLayoutKind::None;
			if (!HasNativeFontPayloadValidationSeal(payload)
				|| !UsesNativeFontVanillaLayout(
					payload.validationSeal.vanillaLayoutKind)
				|| payload.compositePackets.size() != 1)
			{
				return false;
			}
			const NativeFontPacketTemplate& candidate =
				payload.compositePackets.front();
			// BuildNativeFontPayloadTemplate is the sole publisher and records this
			// immutable witness while validating the same vertices.  Count the full
			// eligibility traversal that no longer runs here.
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					VanillaLayoutCertifiedVertexScanAvoided,
				candidate.vertexCount);
			layoutKind = payload.validationSeal.vanillaLayoutKind;
			packet = &candidate;
			return true;
		}

		NiTriShape* TryCreateVanillaLayoutShape(Font& font,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const NativeFontPayloadTemplatePtr& payload, UInt32 glyphCount,
			const NativeFontEffectShapeConfig& effects,
			const NativeFontShapeColorContract& colorContract,
			const NiColorA& tileColor, const NiPoint3& origin,
			bool prepareObject)
		{
			// Vanilla-layout owns an explicit renderer PrecacheGeometry request.
			// LoadingMenu progress text is built while that menu owns renderer/UI
			// locks, so use the direct native facade for this narrow route instead.
			if (IsFreeTypeNoPrecacheRouteActive()
				|| !g_bEnableFreeTypeFontVanillaLayout)
				return nullptr;

			const NativeFontPacketTemplate* packet = nullptr;
			NativeFontVanillaLayoutKind layoutKind =
				NativeFontVanillaLayoutKind::None;
			if (!payload || atlases.size() != 1 || !atlases.front()
				|| !IsVanillaLayoutPayloadEligible(
					*payload, packet, layoutKind)
				|| !packet
				|| !UsesNativeFontVanillaLayout(layoutKind)
				|| !IsVanillaLayoutEnabled(packet->distanceFieldMethod))
			{
				return nullptr;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutEligible);
			if (packet->compositeShiftedShadow)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutShiftedEligible);
			}
			const auto recordFallback = []()
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutFallback);
			};
			if (!prepareObject)
			{
				recordFallback();
				return nullptr;
			}
			const UInt32 targetQuadCount = packet->vertexCount / 4u;
			if (!targetQuadCount || targetQuadCount != glyphCount
				|| packet->vertexCount > std::numeric_limits<UInt16>::max())
			{
				recordFallback();
				return nullptr;
			}

			NiTriShape* shape = CreateFreeTypeTextShape(targetQuadCount,
				tileColor, false, atlases.front()->property,
				GetAtlasTexture(*atlases.front()));
			if (!shape || !shape->GetModelData())
			{
				if (shape)
					shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			NiTriShapeData* data = shape->GetModelData();
			if (data->m_pkBuffData || data->m_usVertices < packet->vertexCount
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkTexture)
				data->m_pkTexture = NiAlloc<NiPoint2>(data->m_usVertices);
			if (!data->m_pkColor || !data->m_pkTexture)
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}

			static constexpr UInt16 kCanonicalQuad[6] =
				{ 0, 2, 1, 0, 3, 2 };
			for (UInt32 index = 0; index < packet->vertexCount; ++index)
			{
				const NativeFontGpuVertex& vertex = payload->gpuVertices[
					static_cast<size_t>(packet->firstVertex) + index];
				data->m_pkVertex[index] = NiPoint3(
					vertex.x + origin.x, vertex.y + origin.y,
					vertex.z + origin.z);
				data->m_pkColor[index] = UnpackNativeBaseColor(vertex.color);
				data->m_pkTexture[index] = NiPoint2(vertex.u, vertex.v);
			}
			for (UInt32 quad = 0; quad < targetQuadCount; ++quad)
			{
				for (UInt32 ordinal = 0; ordinal < 6; ++ordinal)
				{
					data->m_pusTriList[quad * 6u + ordinal] =
						static_cast<UInt16>(
							quad * 4u + kCanonicalQuad[ordinal]);
				}
			}
			// Retail FalloutNV exposes one UV-present bit and one m_pkTexture
			// source. The compact TEXCOORD1/2 or parameterized TEXCOORD1/2/3
			// stream is installed only by the certified direct VB upload after the
			// queued native pack has completed and retired this UV source.
			data->m_usDataFlags |= NiGeometryData::TEXTURE_SET_MASK;
			data->m_kBound = payload->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;

			// Establish vanilla properties and bounds, but suppress its geometry
			// precache: that route selects the vanilla TileShader's 20-byte layout.
			shape->PrepareObject(false, true);
			if (!PrepareNativeFontVanillaLayoutShape(font, shape, font.iFontNum,
				glyphCount, payload->quadCount, layoutKind,
				&effects, &colorContract,
				payload, origin))
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			TileShader* targetShader = ResolveNativeFontPacketShader(
				*packet, shape, false, layoutKind);
			if (!targetShader || !RequestNativeFontVanillaLayoutShapePrecache(
				shape, targetShader))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutPrecacheUnavailable);
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPrecacheAccepted);
			data->m_kBound = payload->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (shape->m_pWorldBound)
				shape->UpdateWorldBound();
			RecordFreeTypePerf(FreeTypePerfCounter::VanillaLayoutCreated);
			if (packet->compositeShiftedShadow)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutShiftedCreated);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::VanillaLayoutVertex,
				packet->vertexCount);
			return shape;
		}

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
			std::vector<NativeFontCompositeSpan>&& compositeSpans)
		{
			failureStage = DirectShapeFailureStage::None;
			auto fail = [&](DirectShapeFailureStage stage) -> NiTriShape*
			{
				failureStage = stage;
				return nullptr;
			};
			if (!quadCount)
				return fail(DirectShapeFailureStage::NativeQuadCountZero);
			if (vertices.size() < quadCount * 4u)
			{
				return fail(
					DirectShapeFailureStage::NativeVertexCountTooSmall);
			}
			if (!PopulateDirectAtlasEffectPages(atlases, effects))
			{
				return fail(DirectShapeFailureStage::
					NativeEffectPagePopulationFailed);
			}
			NiBound bound;
			if (!BuildDirectVertexBound(
				vertices.size(), boundMinimum, boundMaximum, bound))
			{
				return fail(DirectShapeFailureStage::NativeVertexBoundFailed);
			}
			NativeFontPayloadTemplatePtr payload =
				BuildNativeFontPayloadTemplate(std::move(vertices),
					quadCount, glyphCount, colorContract, effects, bound,
					std::move(compositeSpans));
			if (!payload)
				return fail(DirectShapeFailureStage::NativePayloadBuildFailed);
			if (payload->gpuVertices.size() < 4)
			{
				return fail(DirectShapeFailureStage::
					NativePayloadVertexCountTooSmall);
			}
			if (payload->packets.empty())
				return fail(DirectShapeFailureStage::NativePayloadPacketsEmpty);
			if (NiTriShape* vanillaLayout = TryCreateVanillaLayoutShape(
				font, atlases, payload, glyphCount, effects, colorContract,
				tileColor, origin, prepareObject))
			{
				return vanillaLayout;
			}

			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeCandidate);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadePayloadPacket,
				static_cast<UInt64>(payload->packets.size()));
			RecordFreeTypePerf(payload->packets.size() == 1
				? FreeTypePerfCounter::SingletonFacadeSinglePacketArtifact
				: FreeTypePerfCounter::SingletonFacadeMultiPacketArtifact);
			const NativeFontPacketTemplate& facadePacket =
				payload->packets.front();
			if (facadePacket.atlasPage >= atlases.size())
			{
				return fail(DirectShapeFailureStage::
					NativeFacadeAtlasPageOutOfRange);
			}
			NiTriShape* shape = CreateDirectNativePacketShell(
				atlases[facadePacket.atlasPage], *payload, facadePacket,
				facadeColor, tileColor, origin, false);
			if (!shape)
			{
				return fail(DirectShapeFailureStage::
					NativePacketShellCreationFailed);
			}
			if (!PrepareNativeFontSingletonFacadeShape(font, shape,
				font.iFontNum, glyphCount, quadCount, &effects,
				&colorContract, payload, origin))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeFallback);
				if (!PrepareNativeFontAtlasShape(font, shape, font.iFontNum,
					glyphCount, quadCount, &effects, &colorContract,
					payload, origin))
				{
					shape->DeleteThis();
					return fail(DirectShapeFailureStage::
						NativeShapePreparationFailed);
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeCreated);
			if (prepareObject)
			{
				if (IsFreeTypeNoPrecacheRouteActive())
					shape->PrepareObject(false, true);
				else
					shape->PrepareObject();
			}
			NiTriShapeData* data = shape->GetModelData();
			data->m_kBound = bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shape;
		}
}
