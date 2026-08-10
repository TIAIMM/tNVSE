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
		bool WriteDirectQuadGeometry(const AtlasSnapshotPlacement& source,
			const NiPoint3& pen, const NiPoint3& origin,
			float offsetX, float offsetY, float rasterScale,
			float baselineOffset, float sourceToLogicalScale, bool usesSdf,
			NiPoint3* outputPositions, NiPoint2* outputTexture,
			NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!outputPositions || !outputTexture || !source.cacheId
				|| !source.rect.width || !source.rect.height
				|| !std::isfinite(rasterScale) || rasterScale <= 0.0f
				|| !std::isfinite(sourceToLogicalScale)
				|| sourceToLogicalScale <= 0.0f)
			{
				return false;
			}
			const float sourcePixelToLogical =
				sourceToLogicalScale / rasterScale;
			const float logicalX = pen.x - origin.x + offsetX;
			const float logicalZ = pen.z - origin.z
				+ baselineOffset - offsetY;
			const float bitmapLeft = static_cast<float>(source.left);
			const float bitmapTop = static_cast<float>(source.top);
			const float x0 = usesSdf
				? logicalX + bitmapLeft * sourcePixelToLogical
				: std::round(logicalX * rasterScale + bitmapLeft)
					/ rasterScale;
			const float z0 = usesSdf
				? logicalZ + bitmapTop * sourcePixelToLogical
				: std::round(logicalZ * rasterScale + bitmapTop)
					/ rasterScale;
			const float pixelScale = usesSdf
				? sourcePixelToLogical : 1.0f / rasterScale;
			const float x1 = x0
				+ static_cast<float>(source.rect.width) * pixelScale;
			const float z1 = z0
				- static_cast<float>(source.rect.height) * pixelScale;
			const float depth = pen.y - origin.y;
			const std::array<NiPoint3, 4> positions = {{
				NiPoint3(x0, depth, z0), NiPoint3(x1, depth, z0),
				NiPoint3(x1, depth, z1), NiPoint3(x0, depth, z1)
			}};
			const std::array<NiPoint2, 4> texture = {{
				NiPoint2(source.glyphPlacement.u0, source.glyphPlacement.v0),
				NiPoint2(source.glyphPlacement.u1, source.glyphPlacement.v0),
				NiPoint2(source.glyphPlacement.u1, source.glyphPlacement.v1),
				NiPoint2(source.glyphPlacement.u0, source.glyphPlacement.v1)
			}};
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NiPoint3& position = positions[ordinal];
				const NiPoint2& uv = texture[ordinal];
				if (!std::isfinite(position.x)
					|| !std::isfinite(position.y)
					|| !std::isfinite(position.z)
					|| !std::isfinite(uv.x) || !std::isfinite(uv.y))
				{
					return false;
				}
				outputPositions[ordinal] = position;
				outputTexture[ordinal] = uv;
				boundMinimum.x = std::min(boundMinimum.x, position.x);
				boundMinimum.y = std::min(boundMinimum.y, position.y);
				boundMinimum.z = std::min(boundMinimum.z, position.z);
				boundMaximum.x = std::max(boundMaximum.x, position.x);
				boundMaximum.y = std::max(boundMaximum.y, position.y);
				boundMaximum.z = std::max(boundMaximum.z, position.z);
			}
			return true;
		}

		bool WriteDirectQuadVertices(const AtlasSnapshotPlacement& source,
			const NiPoint3& pen, const NiPoint3& origin,
			float offsetX, float offsetY, float rasterScale,
			float baselineOffset, float sourceToLogicalScale, bool usesSdf,
			UInt32 packedColor, UInt8 layerMask,
			NativeFontGpuVertex* output, NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!output)
				return false;
			std::array<NiPoint3, 4> positions;
			std::array<NiPoint2, 4> texture;
			if (!WriteDirectQuadGeometry(source, pen, origin,
				offsetX, offsetY, rasterScale, baselineOffset,
				sourceToLogicalScale, usesSdf, positions.data(),
				texture.data(), boundMinimum, boundMaximum))
			{
				return false;
			}
			const float inverseSourceToLogicalScale =
				1.0f / sourceToLogicalScale;
			const float glyphU0 =
				SanitizeNativeUvBound(source.glyphPlacement.u0);
			const float glyphV0 =
				SanitizeNativeUvBound(source.glyphPlacement.v0);
			const float glyphU1 =
				SanitizeNativeUvBound(source.glyphPlacement.u1);
			const float glyphV1 =
				SanitizeNativeUvBound(source.glyphPlacement.v1);
			if (usesSdf
				&& (!std::isfinite(inverseSourceToLogicalScale)
					|| inverseSourceToLogicalScale < 1.0f
					|| !layerMask || layerMask > 15u
					|| glyphU0 > glyphU1 || glyphV0 > glyphV1))
			{
				return false;
			}
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NiPoint3& position = positions[ordinal];
				const NiPoint2& uv = texture[ordinal];
				output[ordinal] = {
					position.x, position.y, position.z, uv.x, uv.y,
					packedColor, static_cast<float>(source.sdfSpread),
					inverseSourceToLogicalScale,
					static_cast<float>(layerMask),
					glyphU0, glyphV0, glyphU1, glyphV1
				};
			}
			return true;
		}

		bool WriteVanillaDirectQuadGeometry(const FontLetter& letter,
			const NiPoint3& pen, const NiPoint3& origin,
			float baselineOffset, NiPoint3* outputPositions,
			NiPoint2* outputTexture, NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!outputPositions || !outputTexture
				|| letter.iTextureIndex < 0
				|| !std::isfinite(letter.fWidth)
				|| !std::isfinite(letter.fHeight)
				|| !std::isfinite(letter.fLeadingEdge)
				|| !std::isfinite(letter.fTopEdge))
			{
				return false;
			}
			const float x0 = pen.x - origin.x
				+ letter.fLeadingEdge;
			const float z0 = pen.z - origin.z
				+ baselineOffset + letter.fTopEdge;
			const float x1 = x0 + letter.fWidth;
			const float z1 = z0 - letter.fHeight;
			const float depth = pen.y - origin.y;
			const std::array<NiPoint3, 4> positions = {{
				NiPoint3(x0, depth, z0), NiPoint3(x1, depth, z0),
				NiPoint3(x1, depth, z1), NiPoint3(x0, depth, z1)
			}};
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const UVMap& uv = letter.pMapping[ordinal];
				const NiPoint3& position = positions[ordinal];
				if (!std::isfinite(position.x)
					|| !std::isfinite(position.y)
					|| !std::isfinite(position.z)
					|| !std::isfinite(uv.fU)
					|| !std::isfinite(uv.fV))
				{
					return false;
				}
				outputPositions[ordinal] = position;
				outputTexture[ordinal] = NiPoint2(uv.fU, uv.fV);
				boundMinimum.x =
					std::min(boundMinimum.x, position.x);
				boundMinimum.y =
					std::min(boundMinimum.y, position.y);
				boundMinimum.z =
					std::min(boundMinimum.z, position.z);
				boundMaximum.x =
					std::max(boundMaximum.x, position.x);
				boundMaximum.y =
					std::max(boundMaximum.y, position.y);
				boundMaximum.z =
					std::max(boundMaximum.z, position.z);
			}
			return true;
		}

		bool WriteVanillaDirectQuadVertices(const FontLetter& letter,
			const NiPoint3& pen, const NiPoint3& origin,
			float baselineOffset, UInt32 packedColor, UInt8 layerMask,
			NativeFontGpuVertex* output, NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!output)
				return false;
			std::array<NiPoint3, 4> positions;
			std::array<NiPoint2, 4> texture;
			if (!WriteVanillaDirectQuadGeometry(letter, pen, origin,
				baselineOffset, positions.data(), texture.data(),
				boundMinimum, boundMaximum))
			{
				return false;
			}
			float glyphU0 = std::numeric_limits<float>::max();
			float glyphV0 = std::numeric_limits<float>::max();
			float glyphU1 = std::numeric_limits<float>::lowest();
			float glyphV1 = std::numeric_limits<float>::lowest();
			for (const NiPoint2& uv : texture)
			{
				glyphU0 = std::min(glyphU0, uv.x);
				glyphV0 = std::min(glyphV0, uv.y);
				glyphU1 = std::max(glyphU1, uv.x);
				glyphV1 = std::max(glyphV1, uv.y);
			}
			glyphU0 = SanitizeNativeUvBound(glyphU0);
			glyphV0 = SanitizeNativeUvBound(glyphV0);
			glyphU1 = SanitizeNativeUvBound(glyphU1);
			glyphV1 = SanitizeNativeUvBound(glyphV1);
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NiPoint3& position = positions[ordinal];
				const NiPoint2& uv = texture[ordinal];
				output[ordinal] = {
					position.x, position.y, position.z,
					uv.x, uv.y, packedColor, 0.0f, 1.0f,
					static_cast<float>(layerMask),
					glyphU0, glyphV0, glyphU1, glyphV1
				};
			}
			return true;
		}
		bool IsValidCompositeConstructionProfile(
			const CompositeConstructionProfile& profile)
		{
			return profile.staticLayerMask >= 1u
				&& profile.staticLayerMask <= 15u
				&& std::isfinite(profile.uniformSdfSpread)
				&& profile.uniformSdfSpread > 0.0f
				&& std::isfinite(profile.uniformDistanceParameterScale)
				&& profile.uniformDistanceParameterScale >= 1.0f;
		}

		bool VertexMatchesCompositeConstructionProfile(
			const NativeFontGpuVertex& vertex,
			const NativeFontGpuVertex& quadFirst,
			const CompositeConstructionProfile& profile)
		{
			return std::isfinite(vertex.x) && std::isfinite(vertex.y)
				&& std::isfinite(vertex.z) && std::isfinite(vertex.u)
				&& std::isfinite(vertex.v)
				&& std::isfinite(vertex.sdfSpread)
				&& std::isfinite(vertex.distanceParameterScale)
				&& std::isfinite(vertex.glyphU0)
				&& std::isfinite(vertex.glyphV0)
				&& std::isfinite(vertex.glyphU1)
				&& std::isfinite(vertex.glyphV1)
				&& vertex.glyphU0 <= vertex.glyphU1
				&& vertex.glyphV0 <= vertex.glyphV1
				&& vertex.layerMask
					== static_cast<float>(profile.staticLayerMask)
				&& vertex.sdfSpread == profile.uniformSdfSpread
				&& vertex.distanceParameterScale
					== profile.uniformDistanceParameterScale
				&& vertex.glyphU0 == quadFirst.glyphU0
				&& vertex.glyphV0 == quadFirst.glyphV0
				&& vertex.glyphU1 == quadFirst.glyphU1
				&& vertex.glyphV1 == quadFirst.glyphV1;
		}

		bool AppendOneGlyphCompositeQuads(
			std::vector<NativeFontGpuVertex>& vertices,
			const std::vector<CompositeGlyphQuadSource>& sources,
			UInt32 pageCount, const NativeFontEffectShapeConfig& effects,
			NiPoint3& boundMinimum, NiPoint3& boundMaximum,
			std::vector<NativeFontCompositeSpan>& spans,
			const CompositeConstructionProfile* constructionProfile)
		{
			spans.clear();
			if (!effects.shaderEffects || sources.empty() || !pageCount)
				return false;
			const UInt32 sourceVertexCount =
				static_cast<UInt32>(vertices.size());
			const bool shiftedShadow =
				(effects.shadowOffsetX != 0.0f
					|| effects.shadowOffsetY != 0.0f);
			if (!std::isfinite(effects.shadowOffsetX)
				|| !std::isfinite(effects.shadowOffsetY))
			{
				return false;
			}
			bool constructionProfileValid = constructionProfile
				&& pageCount == 1u
				&& IsValidCompositeConstructionProfile(*constructionProfile);
			bool canAliasSource = pageCount == 1 && !shiftedShadow
				&& sources.size() * 4u == vertices.size();
			UInt64 fusedAliasProofVisits = 0;
			for (size_t sourceIndex = 0; sourceIndex < sources.size();
				++sourceIndex)
			{
				const CompositeGlyphQuadSource& source = sources[sourceIndex];
				const UInt64 bodyEnd =
					static_cast<UInt64>(source.firstVertex) + 4u;
				if (!source.layerMask || source.atlasPage >= pageCount
					|| bodyEnd > sourceVertexCount)
				{
					return false;
				}
				if (constructionProfileValid
					&& (source.atlasPage != 0u
						|| source.layerMask
							!= constructionProfile->staticLayerMask))
				{
					constructionProfileValid = false;
				}
				const NativeFontGpuVertex& topLeft =
					vertices[source.firstVertex];
				const NativeFontGpuVertex& topRight =
					vertices[source.firstVertex + 1u];
				const NativeFontGpuVertex& bottomRight =
					vertices[source.firstVertex + 2u];
				if (!(topRight.x > topLeft.x)
					|| !(topLeft.z > bottomRight.z))
				{
					return false;
				}
				if (canAliasSource)
				{
					++fusedAliasProofVisits;
					canAliasSource = source.atlasPage == 0
						&& source.firstVertex == sourceIndex * 4u
						&& source.layerMask != 0;
				}
			}
			RecordFreeTypePerf(FreeTypePerfCounter::
				TextArtifactCompositeAliasProofScanSaved,
				fusedAliasProofVisits);

			// The common single-page/no-offset case already has exactly one body
			// quad per glyph. Reuse it without growing the 32-bit text artifact.
			if (canAliasSource)
			{
				spans.push_back({
					0, sourceVertexCount, 0, true
				});
				return true;
			}

			const UInt64 totalVertexCount =
				static_cast<UInt64>(vertices.size())
				+ static_cast<UInt64>(sources.size()) * 4u;
			if (totalVertexCount
				> static_cast<UInt64>(kNativeFontMaximumQuads) * 4u)
			{
				return false;
			}
			vertices.reserve(static_cast<size_t>(totalVertexCount));
			for (UInt16 page = 0; page < pageCount; ++page)
			{
				const UInt32 firstVertex =
					static_cast<UInt32>(vertices.size());
				for (const CompositeGlyphQuadSource& source : sources)
				{
					if (source.atlasPage != page)
						continue;
					std::array<NativeFontGpuVertex, 4> quad = {{
						vertices[source.firstVertex + 0],
						vertices[source.firstVertex + 1],
						vertices[source.firstVertex + 2],
						vertices[source.firstVertex + 3]
					}};
					const float x0 = quad[0].x;
					const float x1 = quad[1].x;
					const float z0 = quad[0].z;
					const float z1 = quad[2].z;
					const float width = x1 - x0;
					const float height = z0 - z1;

					const bool hasShadow = (source.layerMask & 1u) != 0;
					const float shadowX = hasShadow
						? effects.shadowOffsetX : 0.0f;
					const float shadowZ = hasShadow
						? -effects.shadowOffsetY : 0.0f;
					const float unionX0 = std::min(x0, x0 + shadowX);
					const float unionX1 = std::max(x1, x1 + shadowX);
					const float unionZ0 = std::max(z0, z0 + shadowZ);
					const float unionZ1 = std::min(z1, z1 + shadowZ);
					const float uPerLogical =
						(quad[1].u - quad[0].u) / width;
					const float vPerLogical =
						(quad[2].v - quad[1].v) / height;
					const float unionU0 =
						quad[0].u + (unionX0 - x0) * uPerLogical;
					const float unionU1 =
						quad[0].u + (unionX1 - x0) * uPerLogical;
					const float unionV0 =
						quad[0].v + (z0 - unionZ0) * vPerLogical;
					const float unionV1 =
						quad[0].v + (z0 - unionZ1) * vPerLogical;
					const std::array<NiPoint3, 4> positions = {{
						NiPoint3(unionX0, quad[0].y, unionZ0),
						NiPoint3(unionX1, quad[1].y, unionZ0),
						NiPoint3(unionX1, quad[2].y, unionZ1),
						NiPoint3(unionX0, quad[3].y, unionZ1)
					}};
					const std::array<NiPoint2, 4> texture = {{
						NiPoint2(unionU0, unionV0),
						NiPoint2(unionU1, unionV0),
						NiPoint2(unionU1, unionV1),
						NiPoint2(unionU0, unionV1)
					}};
					for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
					{
						quad[ordinal].x = positions[ordinal].x;
						quad[ordinal].y = positions[ordinal].y;
						quad[ordinal].z = positions[ordinal].z;
						quad[ordinal].u = texture[ordinal].x;
						quad[ordinal].v = texture[ordinal].y;
						quad[ordinal].layerMask =
							static_cast<float>(source.layerMask);
						if (constructionProfileValid
							&& !VertexMatchesCompositeConstructionProfile(
								quad[ordinal], quad[0], *constructionProfile))
						{
							constructionProfileValid = false;
						}
						boundMinimum.x = std::min(
							boundMinimum.x, quad[ordinal].x);
						boundMinimum.y = std::min(
							boundMinimum.y, quad[ordinal].y);
						boundMinimum.z = std::min(
							boundMinimum.z, quad[ordinal].z);
						boundMaximum.x = std::max(
							boundMaximum.x, quad[ordinal].x);
						boundMaximum.y = std::max(
							boundMaximum.y, quad[ordinal].y);
						boundMaximum.z = std::max(
							boundMaximum.z, quad[ordinal].z);
						vertices.push_back(quad[ordinal]);
					}
				}
				const UInt32 vertexCount =
					static_cast<UInt32>(vertices.size()) - firstVertex;
				if (vertexCount)
				{
					NativeFontCompositeSpan span;
					span.firstVertex = firstVertex;
					span.vertexCount = vertexCount;
					span.atlasPage = page;
					span.fused = pageCount == 1;
					const UInt64 expectedVertexCount =
						static_cast<UInt64>(sources.size()) * 4u;
					if (constructionProfileValid && page == 0u
						&& firstVertex == sourceVertexCount
						&& vertexCount == expectedVertexCount
						&& static_cast<UInt64>(vertices.size())
							== static_cast<UInt64>(sourceVertexCount)
								+ expectedVertexCount)
					{
						span.constructionWitness.uniformSdfSpread =
							constructionProfile->uniformSdfSpread;
						span.constructionWitness.
							uniformDistanceParameterScale =
							constructionProfile->
								uniformDistanceParameterScale;
						span.constructionWitness.staticLayerMask =
							constructionProfile->staticLayerMask;
						span.constructionWitness.complete = true;
						span.constructionWitness.
							registrationVerticesValid = true;
						span.constructionWitness.
							vanillaLayoutVerticesValid = true;
					}
					spans.push_back(span);
				}
			}
			return !spans.empty();
		}

		void ExtendColorContract(NativeFontShapeColorContract& contract,
			bool& initialized, const NiColorA& source)
		{
			const NiColorA color = SanitizeColor(source);
			if (!initialized)
			{
				contract.minimumModifier = color;
				contract.maximumModifier = color;
				initialized = true;
				return;
			}
			contract.minimumModifier.r =
				std::min(contract.minimumModifier.r, color.r);
			contract.minimumModifier.g =
				std::min(contract.minimumModifier.g, color.g);
			contract.minimumModifier.b =
				std::min(contract.minimumModifier.b, color.b);
			contract.minimumModifier.a =
				std::min(contract.minimumModifier.a, color.a);
			contract.maximumModifier.r =
				std::max(contract.maximumModifier.r, color.r);
			contract.maximumModifier.g =
				std::max(contract.maximumModifier.g, color.g);
			contract.maximumModifier.b =
				std::max(contract.maximumModifier.b, color.b);
			contract.maximumModifier.a =
				std::max(contract.maximumModifier.a, color.a);
		}

		bool BuildDirectVertexBound(
			size_t vertexCount,
			const NiPoint3& minimum, const NiPoint3& maximum,
			NiBound& bound)
		{
			if (!vertexCount
				|| !std::isfinite(minimum.x)
				|| !std::isfinite(minimum.y)
				|| !std::isfinite(minimum.z)
				|| !std::isfinite(maximum.x)
				|| !std::isfinite(maximum.y)
				|| !std::isfinite(maximum.z))
			{
				return false;
			}
			bound.m_kCenter = NiPoint3(
				(minimum.x + maximum.x) * 0.5f,
				(minimum.y + maximum.y) * 0.5f,
				(minimum.z + maximum.z) * 0.5f);
			const float dx = (maximum.x - minimum.x) * 0.5f;
			const float dy = (maximum.y - minimum.y) * 0.5f;
			const float dz = (maximum.z - minimum.z) * 0.5f;
			const float radiusSquared =
				dx * dx + dy * dy + dz * dz;
			bound.m_fRadius = std::sqrt(radiusSquared);
			return std::isfinite(bound.m_fRadius);
		}

		bool PopulateDirectAtlasEffectPages(
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			NativeFontEffectShapeConfig& effects)
		{
			if (atlases.empty())
				return false;
			effects.atlasProperties.clear();
			effects.atlasTextures.clear();
			effects.atlasInverseSizes.clear();
			effects.atlasProperties.reserve(atlases.size());
			effects.atlasTextures.reserve(atlases.size());
			effects.atlasInverseSizes.reserve(atlases.size());
			for (const std::shared_ptr<AtlasResource>& atlas : atlases)
			{
				if (!atlas || !atlas->property || !atlas->width
					|| !atlas->height)
				{
					return false;
				}
				NiTexture* texture = GetAtlasTexture(*atlas);
				if (!texture)
					return false;
				effects.atlasProperties.push_back(atlas->property);
				effects.atlasTextures.push_back(texture);
				effects.atlasInverseSizes.push_back(NiPoint2(
					1.0f / static_cast<float>(atlas->width),
					1.0f / static_cast<float>(atlas->height)));
			}
			effects.inverseAtlasWidth = effects.atlasInverseSizes[0].x;
			effects.inverseAtlasHeight = effects.atlasInverseSizes[0].y;
			return true;
		}

		NiTriShape* CreateDirectNativePacketShell(
			const std::shared_ptr<AtlasResource>& atlas,
			const NativeFontPayloadTemplate& payload,
			const NativeFontPacketTemplate& packet,
			const NiColorA& facadeColor, const NiColorA& tileColor,
			const NiPoint3& origin, bool prepareObject)
		{
			const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
				+ packet.vertexCount;
			if (!atlas || !packet.vertexCount
				|| vertexEnd > payload.gpuVertices.size())
			{
				return nullptr;
			}

			NiTriShape* shape = CreateFreeTypeTextShape(1, tileColor, false,
				atlas->property, GetAtlasTexture(*atlas));
			if (!shape || !shape->GetModelData())
			{
				if (shape)
					shape->DeleteThis();
				return nullptr;
			}
			NiTriShapeData* data = shape->GetModelData();
			if (data->m_usVertices < 4 || !data->m_pkVertex
				|| !data->m_pkTexture || !data->m_pusTriList)
			{
				shape->DeleteThis();
				return nullptr;
			}
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkColor)
			{
				shape->DeleteThis();
				return nullptr;
			}

			static constexpr UInt16 kFacadeQuad[6] =
				{ 0, 2, 1, 0, 3, 2 };
			const NiColorA safeFacadeColor = SanitizeColor(facadeColor);
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NativeFontGpuVertex& vertex =
					payload.gpuVertices[packet.firstVertex + ordinal];
				data->m_pkVertex[ordinal] = NiPoint3(
					vertex.x + origin.x,
					vertex.y + origin.y,
					vertex.z + origin.z);
				data->m_pkTexture[ordinal] = NiPoint2(vertex.u, vertex.v);
				data->m_pkColor[ordinal] = safeFacadeColor;
			}
			std::copy(std::begin(kFacadeQuad), std::end(kFacadeQuad),
				data->m_pusTriList);
			data->m_kBound = payload.bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject)
				shape->PrepareObject();
			data->m_kBound = payload.bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shape;
		}
}
