#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiCullingProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
#include "NiPoint4.hpp"
#include "NiStencilProperty.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kScissorTailOffset = 0xC4;
		inline constexpr UInt32 kScissorTailSize = 0x10;
		inline constexpr UInt32 kCanonicalArrayCount = 1;
		// Conservative render-thread cost model. A source draw includes the native
		// packet/proxy/TileShader submission that a follower avoids. Merge cost
		// covers candidate bookkeeping, packet inspection, two CPU vertex passes,
		// the 60-byte write, and one shared DISCARD lock/unlock per traversal.
		// These are intentionally fixed internal estimates rather than an INI ABI:
		// their purpose is to reject CPU-negative short-text batches, not predict
		// wall-clock frame time exactly. No speculative GPU saving is credited,
		// because the observed inventory regression is render-thread bound.
		inline constexpr UInt64 kEstimatedSavedDrawNanoseconds = 2048;
		inline constexpr UInt64 kEstimatedBatchCandidateNanoseconds = 2048;
		inline constexpr UInt64 kEstimatedBatchPacketNanoseconds = 128;
		inline constexpr UInt64 kEstimatedBatchVertexNanoseconds = 64;
		// The rejected inventory traversals measured a 16.384 us histogram
		// ceiling before any upload. Charge half of that as unavoidable
		// descriptor/gate work in addition to the existing DISCARD cost so a
		// candidate must repay both preparing and publishing a batch.
		inline constexpr UInt64 kEstimatedBatchDecisionNanoseconds = 8192;
		inline constexpr UInt64 kEstimatedBatchFrameUploadNanoseconds = 8192;

		struct TileShaderPropertyView : BSShaderProperty
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

		static_assert(sizeof(NiTriShape) == kScissorTailOffset);
		static_assert(sizeof(TileShaderPropertyView) == 0xB0);
		static_assert(offsetof(TileShaderPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(TileShaderPropertyView, tileAlpha) == 0x78);

		struct CrossFacadePacketRef
		{
			NiTriShape* facade = nullptr;
			const A8ShapeMetadata* metadata = nullptr;
			NativeA8ShapePayload* payload = nullptr;
			const NativeA8PacketTemplate* packet = nullptr;
			TileShader* batchShader = nullptr;
			const void* d3dTexture = nullptr;
			NiTransform effectiveWorld;
			NiColorA tileColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			UInt32 packetIndex = 0;
		};

		struct CrossFacadeCandidate
		{
			size_t segmentIndex = 0;
			size_t firstPacket = 0;
			size_t packetCount = 0;
			UInt32 vertexCount = 0;
			UInt32 facadeCount = 0;
			UInt32 baseVertex = 0;
			UInt64 estimatedSavedNanoseconds = 0;
			UInt64 estimatedMergeNanoseconds = 0;
			NiBound bound;
			bool selected = false;
		};

		enum class CrossFacadeCommandType : UInt8
		{
			Ordinary,
			Batch
		};

		struct CrossFacadeCommand
		{
			CrossFacadeCommandType type =
				CrossFacadeCommandType::Ordinary;
			size_t packetOrCandidate = 0;
		};

		struct CrossFacadeSegment
		{
			size_t firstItem = 0;
			size_t itemCount = 0;
			size_t firstFacade = 0;
			size_t facadeCount = 0;
			size_t firstPacket = 0;
			size_t packetCount = 0;
			size_t firstCommand = 0;
			size_t commandCount = 0;
			bool executable = false;
			bool completed = false;
		};

		struct CrossFacadeDispatchEntry
		{
			NiTriShape* facade = nullptr;
			size_t segmentIndex = 0;
			bool leader = false;
		};

		struct CrossFacadeFrameState
		{
			std::vector<CrossFacadePacketRef> packets;
			std::vector<CrossFacadeCandidate> candidates;
			std::vector<CrossFacadeSegment> segments;
			std::vector<NiTriShape*> facades;
			std::vector<CrossFacadeCommand> commands;
			std::vector<CrossFacadeDispatchEntry> dispatchEntries;
			std::vector<UInt32> dispatchLookup;
			std::vector<UInt32> occurrenceLookup;
			std::vector<UInt8> duplicateItems;
			std::vector<size_t> candidateOrder;
			std::vector<SInt32> candidateAtPacket;
			CpuMemoryLease cpuMemory;
			UInt32 generation = 0;
			UInt32 dispatchDepth = 0;
			UInt32 suspendDepth = 0;
			bool prepared = false;
		};

		struct CrossFacadeGpuState
		{
			std::mutex mutex;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			UInt32 generation = 0;
			UInt32 faultGeneration = 0;
			UInt32 vertexCapacity = 0;
			UInt32 indexBytes = 0;
			bool loggedReady = false;
		};

		thread_local CrossFacadeFrameState s_frame;

		CrossFacadeGpuState& GpuState()
		{
			static CrossFacadeGpuState* state = new CrossFacadeGpuState();
			return *state;
		}

		size_t HashPointer(const void* pointer)
		{
			size_t value = reinterpret_cast<size_t>(pointer) >> 4;
			value ^= value >> 16;
			value *= static_cast<size_t>(0x45D9F3Bu);
			value ^= value >> 16;
			return value;
		}

		size_t LookupCapacity(size_t expected)
		{
			size_t capacity = 8;
			const size_t required = std::max<size_t>(8, expected * 2u);
			while (capacity < required)
				capacity <<= 1;
			return capacity;
		}

		void PrepareLookup(std::vector<UInt32>& lookup, size_t expected)
		{
			const size_t capacity = LookupCapacity(expected);
			if (lookup.size() != capacity)
				lookup.assign(capacity, 0);
			else
				std::fill(lookup.begin(), lookup.end(), 0);
		}

		size_t EstimateFrameBytes(const CrossFacadeFrameState& frame)
		{
			return frame.packets.capacity() * sizeof(CrossFacadePacketRef)
				+ frame.candidates.capacity() * sizeof(CrossFacadeCandidate)
				+ frame.segments.capacity() * sizeof(CrossFacadeSegment)
				+ frame.facades.capacity() * sizeof(NiTriShape*)
				+ frame.commands.capacity() * sizeof(CrossFacadeCommand)
				+ frame.dispatchEntries.capacity()
					* sizeof(CrossFacadeDispatchEntry)
				+ frame.dispatchLookup.capacity() * sizeof(UInt32)
				+ frame.occurrenceLookup.capacity() * sizeof(UInt32)
				+ frame.duplicateItems.capacity() * sizeof(UInt8)
				+ frame.candidateOrder.capacity() * sizeof(size_t)
				+ frame.candidateAtPacket.capacity() * sizeof(SInt32);
		}

		void RefreshFrameMemory(CrossFacadeFrameState& frame)
		{
			frame.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				EstimateFrameBytes(frame));
		}

		void ClearFrame(CrossFacadeFrameState& frame, bool releaseMemory)
		{
			frame.prepared = false;
			frame.generation = 0;
			frame.dispatchDepth = 0;
			frame.suspendDepth = 0;
			frame.packets.clear();
			frame.candidates.clear();
			frame.segments.clear();
			frame.facades.clear();
			frame.commands.clear();
			frame.dispatchEntries.clear();
			frame.candidateOrder.clear();
			frame.candidateAtPacket.clear();
			frame.duplicateItems.clear();
			if (releaseMemory || IsCpuMemoryBudgetExceeded())
			{
				std::vector<CrossFacadePacketRef>().swap(frame.packets);
				std::vector<CrossFacadeCandidate>().swap(frame.candidates);
				std::vector<CrossFacadeSegment>().swap(frame.segments);
				std::vector<NiTriShape*>().swap(frame.facades);
				std::vector<CrossFacadeCommand>().swap(frame.commands);
				std::vector<CrossFacadeDispatchEntry>().swap(
					frame.dispatchEntries);
				std::vector<UInt32>().swap(frame.dispatchLookup);
				std::vector<UInt32>().swap(frame.occurrenceLookup);
				std::vector<UInt8>().swap(frame.duplicateItems);
				std::vector<size_t>().swap(frame.candidateOrder);
				std::vector<SInt32>().swap(frame.candidateAtPacket);
				frame.cpuMemory.Release();
			}
			else
			{
				RefreshFrameMemory(frame);
			}
		}

		const TileShaderPropertyView* GetTileProperty(
			const NiTriShape* shape)
		{
			NiShadeProperty* property =
				shape ? shape->GetShadeProperty() : nullptr;
			return property
				&& property->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<const TileShaderPropertyView*>(property)
				: nullptr;
		}

		bool EqualBytes(const void* left, const void* right, size_t bytes)
		{
			return std::memcmp(left, right, bytes) == 0;
		}

		bool EqualOptionalCulling(const NiCullingProperty* left,
			const NiCullingProperty* right)
		{
			return (!left || !right)
				? left == right : left->m_usFlags == right->m_usFlags;
		}

		bool EqualOptionalStencil(const NiStencilProperty* left,
			const NiStencilProperty* right)
		{
			return (!left || !right) ? left == right
				: EqualBytes(&left->m_usFlags, &right->m_usFlags,
						sizeof(left->m_usFlags))
					&& left->m_uiRef == right->m_uiRef
					&& left->m_uiMask == right->m_uiMask;
		}

		bool EqualOptionalMaterialWithoutAlpha(
			const NiMaterialProperty* left,
			const NiMaterialProperty* right)
		{
			if (!left || !right)
				return left == right;
			return left->m_iIndex == right->m_iIndex
				&& EqualBytes(&left->m_spec, &right->m_spec,
					sizeof(left->m_spec))
				&& EqualBytes(&left->m_emit, &right->m_emit,
					sizeof(left->m_emit))
				&& left->m_pExternalEmittance == right->m_pExternalEmittance
				&& EqualBytes(&left->m_fShine, &right->m_fShine,
					sizeof(left->m_fShine))
				&& EqualBytes(&left->m_fEmitMult, &right->m_fEmitMult,
					sizeof(left->m_fEmitMult))
				&& left->m_uiRevID == right->m_uiRevID;
		}

		bool EqualLinearTransform(const NiTransform& left,
			const NiTransform& right)
		{
			return EqualBytes(&left.m_Rotate, &right.m_Rotate,
					sizeof(left.m_Rotate))
				&& EqualBytes(&left.m_fScale, &right.m_fScale,
					sizeof(left.m_fScale));
		}

		bool IsFiniteTransform(const NiTransform& value)
		{
			if (!std::isfinite(value.m_fScale)
				|| std::abs(value.m_fScale) < 0.000001f
				|| !std::isfinite(value.m_Translate.x)
				|| !std::isfinite(value.m_Translate.y)
				|| !std::isfinite(value.m_Translate.z))
			{
				return false;
			}
			for (const auto& row : value.m_Rotate.m_pEntry)
			{
				for (float entry : row)
				{
					if (!std::isfinite(entry))
						return false;
				}
			}
			return true;
		}

		bool EqualFacadeFixedState(const NiTriShape* left,
			const NiTriShape* right)
		{
			if (!left || !right
				|| !EqualBytes(&left->m_uiFlags, &right->m_uiFlags,
					sizeof(left->m_uiFlags))
				|| !EqualLinearTransform(left->m_kLocal, right->m_kLocal)
				|| !EqualLinearTransform(left->m_kWorld, right->m_kWorld)
				|| !IsFiniteTransform(left->m_kWorld)
				|| !IsFiniteTransform(right->m_kWorld)
				|| !EqualBytes(
					reinterpret_cast<const UInt8*>(left)
						+ kScissorTailOffset,
					reinterpret_cast<const UInt8*>(right)
						+ kScissorTailOffset,
					kScissorTailSize))
			{
				return false;
			}

			const NiAlphaProperty* leftAlpha = left->GetAlphaProperty();
			const NiAlphaProperty* rightAlpha = right->GetAlphaProperty();
			if (!leftAlpha || !rightAlpha
				|| !EqualBytes(&leftAlpha->m_usFlags,
					&rightAlpha->m_usFlags,
					sizeof(leftAlpha->m_usFlags))
				|| leftAlpha->m_ucAlphaTestRef
					!= rightAlpha->m_ucAlphaTestRef)
			{
				return false;
			}
			if (!EqualOptionalCulling(
					left->m_kProperties.m_spCullingProperty.m_pObject,
					right->m_kProperties.m_spCullingProperty.m_pObject)
				|| !EqualOptionalStencil(
					left->m_kProperties.m_spStencilProperty.m_pObject,
					right->m_kProperties.m_spStencilProperty.m_pObject)
				|| !EqualOptionalMaterialWithoutAlpha(
					left->m_kProperties.m_spMaterialProperty.m_pObject,
					right->m_kProperties.m_spMaterialProperty.m_pObject)
				|| left->m_kProperties.m_spUnknownProperty.m_pObject
					!= right->m_kProperties.m_spUnknownProperty.m_pObject)
			{
				return false;
			}

			const TileShaderPropertyView* leftTile =
				GetTileProperty(left);
			const TileShaderPropertyView* rightTile =
				GetTileProperty(right);
			if (!leftTile || !rightTile)
				return false;
			return EqualBytes(&leftTile->m_usFlags,
					&rightTile->m_usFlags, sizeof(leftTile->m_usFlags))
				&& leftTile->ulFlags[0] == rightTile->ulFlags[0]
				&& leftTile->ulFlags[1] == rightTile->ulFlags[1]
				&& EqualBytes(&leftTile->fAlpha, &rightTile->fAlpha,
					sizeof(leftTile->fAlpha))
				&& EqualBytes(&leftTile->fFadeAlpha,
					&rightTile->fFadeAlpha,
					sizeof(leftTile->fFadeAlpha))
				&& EqualBytes(&leftTile->fEnvMapScale,
					&rightTile->fEnvMapScale,
					sizeof(leftTile->fEnvMapScale))
				&& EqualBytes(&leftTile->fLODFade,
					&rightTile->fLODFade,
					sizeof(leftTile->fLODFade))
				&& EqualBytes(&leftTile->fDepthBias,
					&rightTile->fDepthBias,
					sizeof(leftTile->fDepthBias))
				&& leftTile->uiShaderIndex == rightTile->uiShaderIndex
				&& leftTile->alphaTexture.m_pObject
					== rightTile->alphaTexture.m_pObject
				&& EqualBytes(&leftTile->textureTransform,
					&rightTile->textureTransform,
					sizeof(leftTile->textureTransform))
				&& leftTile->clampMode == rightTile->clampMode
				&& leftTile->byte90 == rightTile->byte90
				&& leftTile->rotates == rightTile->rotates
				&& leftTile->noTexture == rightTile->noTexture
				&& leftTile->useScissorTest
					== rightTile->useScissorTest
				&& EqualRect(&leftTile->scissorRect,
					&rightTile->scissorRect);
		}

		bool ResolveTileColor(const NiTriShape* facade, NiColorA& color)
		{
			const TileShaderPropertyView* tile =
				GetTileProperty(facade);
			if (!tile)
				return false;
			const NiMaterialProperty* material =
				facade->m_kProperties.m_spMaterialProperty.m_pObject;
			const float materialAlpha = material
				? material->m_fAlpha : 1.0f;
			color = {
				tile->overlayColor.r,
				tile->overlayColor.g,
				tile->overlayColor.b,
				tile->tileAlpha * materialAlpha
			};
			return std::isfinite(color.r) && std::isfinite(color.g)
				&& std::isfinite(color.b) && std::isfinite(color.a);
		}

		NiTransform EffectiveWorld(const NiTriShape* facade,
			const NiPoint3& origin)
		{
			NiTransform result = facade->m_kWorld;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				result.m_Translate = facade->m_kWorld * origin;
			return result;
		}

		bool ValidFrameItem(const NativeA8CrossFacadeFrameItem& item,
			bool duplicate)
		{
			if (duplicate || item.barrier || !item.facade || !item.metadata
				|| !item.payload
				|| item.preflightResult != NativeA8FallbackReason::None
				|| !item.payload->buildComplete
				|| !item.payload->payloadTemplate
				|| item.generation != item.payload->preparedGeneration)
			{
				return false;
			}
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(*item.payload->payloadTemplate,
					item.payload->useCompositePackets);
			return !packets.empty()
				&& packets.size() == item.payload->packetShaders.size();
		}

		struct CrossFacadeOptimisticCost
		{
			UInt64 savedNanoseconds = 0;
			UInt64 mergeNanoseconds = 0;
			UInt64 candidateCount = 0;
		};

		enum class CrossFacadeOptimisticGate : UInt8
		{
			NoCandidate,
			CostRejected,
			Pass
		};

		void AddOptimisticChunk(CrossFacadeOptimisticCost& cost,
			UInt32 packetCount, UInt32 facadeCount, UInt32 vertexCount)
		{
			if (packetCount < 2 || facadeCount < 2 || !vertexCount)
				return;
			const UInt64 saved =
				static_cast<UInt64>(packetCount - 1u)
					* kEstimatedSavedDrawNanoseconds;
			const UInt64 merge =
				kEstimatedBatchCandidateNanoseconds
				+ static_cast<UInt64>(packetCount)
					* kEstimatedBatchPacketNanoseconds
				+ static_cast<UInt64>(vertexCount)
					* kEstimatedBatchVertexNanoseconds;
			if (saved <= merge)
				return;
			cost.savedNanoseconds += saved;
			cost.mergeNanoseconds += merge;
			++cost.candidateCount;
		}

		bool EstimateOptimisticSegment(
			const std::vector<NativeA8CrossFacadeFrameItem>& items,
			size_t firstItem, size_t itemCount,
			CrossFacadeOptimisticCost& cost)
		{
			UInt32 chunkPackets = 0;
			UInt32 chunkFacades = 0;
			UInt32 chunkVertices = 0;
			NiTriShape* lastFacade = nullptr;
			const size_t endItem = firstItem + itemCount;
			for (size_t itemIndex = firstItem;
				itemIndex < endItem; ++itemIndex)
			{
				const NativeA8CrossFacadeFrameItem& item =
					items[itemIndex];
				const NativeA8PayloadTemplate& artifact =
					*item.payload->payloadTemplate;
				const std::vector<NativeA8PacketTemplate>& packets =
					GetNativeA8Packets(artifact,
						item.payload->useCompositePackets);
				for (const NativeA8PacketTemplate& packet : packets)
				{
					const UInt64 endVertex =
						static_cast<UInt64>(packet.firstVertex)
							+ packet.vertexCount;
					if (!packet.vertexCount
						|| (packet.vertexCount & 3u)
						|| endVertex > artifact.gpuVertices.size()
						|| packet.atlasPage
							>= item.payload
								->preflightAtlasTextures.size())
					{
						return false;
					}
					if (chunkPackets && packet.vertexCount
						> kNativeA8MaximumQuads * 4u
							- chunkVertices)
					{
						AddOptimisticChunk(cost, chunkPackets,
							chunkFacades, chunkVertices);
						chunkPackets = 0;
						chunkFacades = 0;
						chunkVertices = 0;
						lastFacade = nullptr;
					}
					if (packet.vertexCount
						> kNativeA8MaximumQuads * 4u)
					{
						lastFacade = nullptr;
						continue;
					}
					if (item.facade != lastFacade)
					{
						++chunkFacades;
						lastFacade = item.facade;
					}
					++chunkPackets;
					chunkVertices += packet.vertexCount;
				}
			}
			AddOptimisticChunk(cost, chunkPackets,
				chunkFacades, chunkVertices);
			return true;
		}

		CrossFacadeOptimisticGate BuildOptimisticSegments(
			const std::vector<NativeA8CrossFacadeFrameItem>& items,
			CrossFacadeFrameState& frame)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCostPrecheck);
			CrossFacadeOptimisticCost total;
			size_t cursor = 0;
			while (cursor < items.size())
			{
				if (!ValidFrameItem(items[cursor],
						frame.duplicateItems[cursor] != 0))
				{
					++cursor;
					continue;
				}
				const size_t firstItem = cursor;
				const NiTriShape* leaderFacade = items[cursor].facade;
				while (cursor < items.size()
					&& ValidFrameItem(items[cursor],
						frame.duplicateItems[cursor] != 0)
					&& EqualFacadeFixedState(
						leaderFacade, items[cursor].facade))
				{
					++cursor;
				}
				if (cursor < items.size()
					&& ValidFrameItem(items[cursor],
						frame.duplicateItems[cursor] != 0))
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeStateFallback);
				}
				const size_t itemCount = cursor - firstItem;
				if (itemCount < 2)
					continue;

				CrossFacadeOptimisticCost segmentCost;
				if (!EstimateOptimisticSegment(items, firstItem,
						itemCount, segmentCost))
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CrossFacadeResourceFallback);
					continue;
				}
				if (!segmentCost.candidateCount)
					continue;
				CrossFacadeSegment segment;
				segment.firstItem = firstItem;
				segment.itemCount = itemCount;
				frame.segments.push_back(segment);
				total.savedNanoseconds +=
					segmentCost.savedNanoseconds;
				total.mergeNanoseconds +=
					segmentCost.mergeNanoseconds;
				total.candidateCount +=
					segmentCost.candidateCount;
			}

			const UInt64 fixedMergeNanoseconds =
				kEstimatedBatchDecisionNanoseconds
					+ kEstimatedBatchFrameUploadNanoseconds;
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CrossFacadePrecheckEstimatedSavedNanoseconds,
				total.savedNanoseconds);
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CrossFacadePrecheckEstimatedMergeNanoseconds,
				total.mergeNanoseconds + fixedMergeNanoseconds);
			if (!total.candidateCount)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CrossFacadeCostPrecheckReject);
				return CrossFacadeOptimisticGate::NoCandidate;
			}
			if (total.savedNanoseconds
				<= total.mergeNanoseconds + fixedMergeNanoseconds)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CrossFacadeCostPrecheckReject);
				RecordFreeTypePerf(
					FreeTypePerfCounter::CrossFacadeCostFallback,
					total.candidateCount);
				frame.segments.clear();
				return CrossFacadeOptimisticGate::CostRejected;
			}
			return CrossFacadeOptimisticGate::Pass;
		}

		bool BuildPacketRef(const NativeA8CrossFacadeFrameItem& item,
			UInt32 packetIndex, CrossFacadePacketRef& output)
		{
			const NativeA8PayloadTemplate& artifact =
				*item.payload->payloadTemplate;
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(artifact,
					item.payload->useCompositePackets);
			if (packetIndex >= packets.size())
				return false;
			const NativeA8PacketTemplate& packet = packets[packetIndex];
			const UInt64 end = static_cast<UInt64>(packet.firstVertex)
				+ packet.vertexCount;
			if (!packet.vertexCount || (packet.vertexCount & 3u)
				|| end > artifact.gpuVertices.size()
				|| packet.atlasPage
					>= item.payload->preflightAtlasTextures.size())
			{
				return false;
			}
			output.facade = item.facade;
			output.metadata = item.metadata;
			output.payload = item.payload;
			output.packet = &packet;
			output.packetIndex = packetIndex;
			output.d3dTexture =
				item.payload->preflightAtlasTextures[packet.atlasPage];
			output.effectiveWorld = EffectiveWorld(item.facade,
				item.payload->geometryOrigin);
			if (!output.d3dTexture || !IsFiniteTransform(output.effectiveWorld)
				|| !ResolveTileColor(item.facade, output.tileColor))
			{
				return false;
			}
			output.batchShader = ResolveNativeA8CrossFacadePacketShader(
				packet, item.facade,
				item.payload->preflightScaledFillSampling);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadePacketMaterialized);
			return true;
		}

		bool SameBatchTarget(const CrossFacadePacketRef& left,
			const CrossFacadePacketRef& right)
		{
			return left.batchShader && left.batchShader == right.batchShader
				&& left.d3dTexture && left.d3dTexture == right.d3dTexture;
		}

		UInt32 CountDistinctFacades(
			const std::vector<CrossFacadePacketRef>& packets,
			size_t first, size_t count)
		{
			UInt32 result = 0;
			NiTriShape* previous = nullptr;
			for (size_t index = 0; index < count; ++index)
			{
				NiTriShape* facade = packets[first + index].facade;
				if (facade != previous)
				{
					++result;
					previous = facade;
				}
			}
			return result;
		}

		void AddCandidate(CrossFacadeFrameState& frame,
			size_t segmentIndex, size_t firstPacket, size_t packetCount,
			UInt32 vertexCount)
		{
			const UInt32 facadeCount = CountDistinctFacades(
				frame.packets, firstPacket, packetCount);
			if (packetCount < 2 || facadeCount < 2 || !vertexCount)
				return;
			CrossFacadeCandidate candidate;
			candidate.segmentIndex = segmentIndex;
			candidate.firstPacket = firstPacket;
			candidate.packetCount = packetCount;
			candidate.vertexCount = vertexCount;
			candidate.facadeCount = facadeCount;
			RecordFreeTypePerf(FreeTypePerfCounter::CrossFacadeCandidate);
			candidate.estimatedSavedNanoseconds =
				static_cast<UInt64>(packetCount - 1u)
					* kEstimatedSavedDrawNanoseconds;
			candidate.estimatedMergeNanoseconds =
				kEstimatedBatchCandidateNanoseconds
				+ static_cast<UInt64>(packetCount)
					* kEstimatedBatchPacketNanoseconds
				+ static_cast<UInt64>(vertexCount)
					* kEstimatedBatchVertexNanoseconds;
			if (candidate.estimatedSavedNanoseconds
				<= candidate.estimatedMergeNanoseconds)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CrossFacadeCostFallback);
				return;
			}
			frame.candidates.push_back(candidate);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCostEligible);
		}

		void BuildCandidatesForSegment(CrossFacadeFrameState& frame,
			size_t segmentIndex)
		{
			const CrossFacadeSegment& segment =
				frame.segments[segmentIndex];
			const size_t end = segment.firstPacket + segment.packetCount;
			size_t cursor = segment.firstPacket;
			while (cursor < end)
			{
				if (!frame.packets[cursor].batchShader)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeResourceFallback);
					++cursor;
					continue;
				}
				size_t targetEnd = cursor + 1u;
				while (targetEnd < end
					&& SameBatchTarget(frame.packets[cursor],
						frame.packets[targetEnd]))
				{
					++targetEnd;
				}
				if (targetEnd < end)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeStateFallback);
				}

				size_t chunkFirst = cursor;
				size_t chunkCount = 0;
				UInt32 chunkVertices = 0;
				for (size_t index = cursor; index < targetEnd; ++index)
				{
					const UInt32 vertices =
						frame.packets[index].packet->vertexCount;
					if (chunkCount && vertices
						> kNativeA8MaximumQuads * 4u - chunkVertices)
					{
						AddCandidate(frame, segmentIndex, chunkFirst,
							chunkCount, chunkVertices);
						chunkFirst = index;
						chunkCount = 0;
						chunkVertices = 0;
					}
					if (vertices > kNativeA8MaximumQuads * 4u)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CrossFacadeCapacityFallback);
						chunkFirst = index + 1u;
						continue;
					}
					chunkVertices += vertices;
					++chunkCount;
				}
				AddCandidate(frame, segmentIndex, chunkFirst,
					chunkCount, chunkVertices);
				cursor = targetEnd;
			}
		}

		void ReleaseGpuLocked(CrossFacadeGpuState& state)
		{
			if (state.vertexBuffer)
				state.vertexBuffer->Release();
			if (state.indexBuffer)
				state.indexBuffer->Release();
			state.renderer = nullptr;
			state.device = nullptr;
			state.vertexBuffer = nullptr;
			state.indexBuffer = nullptr;
			state.declaration = nullptr;
			state.generation = 0;
			state.vertexCapacity = 0;
			state.indexBytes = 0;
		}

		void FaultCrossFacadeGeneration(UInt32 generation)
		{
			if (!generation
				|| !IsNativeA8ShaderGenerationCurrent(generation))
				return;
			CrossFacadeGpuState& state = GpuState();
			std::lock_guard<std::mutex> lock(state.mutex);
			if (state.generation && state.generation != generation)
				return;
			ReleaseGpuLocked(state);
			state.faultGeneration = generation;
		}

		bool PopulateIndexBuffer(IDirect3DIndexBuffer9* buffer,
			UInt32 quadCapacity)
		{
			if (!buffer || !quadCapacity)
				return false;
			void* memory = nullptr;
			const UINT bytes = quadCapacity * 6u * sizeof(UInt16);
			HRESULT result = buffer->Lock(0, bytes, &memory, 0);
			if (FAILED(result) || !memory)
				return false;
			auto* indices = static_cast<UInt16*>(memory);
			for (UInt32 quad = 0; quad < quadCapacity; ++quad)
			{
				const UInt16 base =
					static_cast<UInt16>(quad * 4u);
				const UInt32 output = quad * 6u;
				indices[output + 0] = static_cast<UInt16>(base + 0u);
				indices[output + 1] = static_cast<UInt16>(base + 2u);
				indices[output + 2] = static_cast<UInt16>(base + 1u);
				indices[output + 3] = static_cast<UInt16>(base + 0u);
				indices[output + 4] = static_cast<UInt16>(base + 3u);
				indices[output + 5] = static_cast<UInt16>(base + 2u);
			}
			result = buffer->Unlock();
			return SUCCEEDED(result);
		}

		bool EnsureGpuLocked(CrossFacadeGpuState& state,
			UInt32 generation)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device =
				renderer ? renderer->GetD3DDevice() : nullptr;
			IDirect3DVertexDeclaration9* declaration =
				GetNativeA8CrossFacadeD3DDeclaration(generation);
			if (!renderer || !device || !declaration
				|| !IsNativeA8ShaderGenerationCurrent(generation))
			{
				if (generation
					&& IsNativeA8ShaderGenerationCurrent(generation))
				{
					state.faultGeneration = generation;
				}
				return false;
			}
			if (state.renderer == renderer && state.device == device
				&& state.generation == generation
				&& state.declaration == declaration
				&& state.vertexBuffer && state.indexBuffer)
			{
				return true;
			}
			if (state.faultGeneration == generation)
				return false;
			if (state.faultGeneration
				&& state.faultGeneration != generation)
				state.faultGeneration = 0;
			if (state.generation != generation)
				state.loggedReady = false;
			ReleaseGpuLocked(state);

			const UInt64 capLimit = static_cast<UInt64>(
				renderer->m_kD3DCaps9.MaxVertexIndex) + 1u;
			UInt64 desired = std::min<UInt64>(
				kNativeA8CrossFacadeVertexCapacity, capLimit);
			desired &= ~static_cast<UInt64>(3u);
			if (desired < 8u
				|| desired > std::numeric_limits<UInt32>::max())
			{
				state.faultGeneration = generation;
				return false;
			}
			const UInt32 vertexCapacity = static_cast<UInt32>(desired);
			const UInt32 quadCapacity = std::min<UInt32>(
				kNativeA8MaximumQuads, vertexCapacity / 4u);
			const UInt32 indexBytes =
				quadCapacity * 6u * sizeof(UInt16);

			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			HRESULT result = device->CreateVertexBuffer(
				vertexCapacity * sizeof(NativeA8CrossFacadeGpuVertex),
				D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
				D3DPOOL_DEFAULT, &vertexBuffer, nullptr);
			if (SUCCEEDED(result) && vertexBuffer)
			{
				result = device->CreateIndexBuffer(indexBytes,
					D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
					D3DPOOL_DEFAULT, &indexBuffer, nullptr);
			}
			if (FAILED(result) || !vertexBuffer || !indexBuffer
				|| !PopulateIndexBuffer(indexBuffer, quadCapacity))
			{
				if (indexBuffer)
					indexBuffer->Release();
				if (vertexBuffer)
					vertexBuffer->Release();
				state.faultGeneration = generation;
				return false;
			}

			state.renderer = renderer;
			state.device = device;
			state.vertexBuffer = vertexBuffer;
			state.indexBuffer = indexBuffer;
			state.declaration = declaration;
			state.generation = generation;
			state.vertexCapacity = vertexCapacity;
			state.indexBytes = indexBytes;
			if (g_bEnableFreeTypeFontRenderingLog && !state.loggedReady)
			{
				state.loggedReady = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: cross-facade geometry ready generation=%u vertexCapacity=%u vertexStride=%u vertexBytes=%u canonicalIndexBytes=%u costDrawNs=%llu costCandidateNs=%llu costPacketNs=%llu costVertexNs=%llu costDecisionNs=%llu costFrameNs=%llu",
					generation, vertexCapacity,
					static_cast<UInt32>(
						sizeof(NativeA8CrossFacadeGpuVertex)),
					vertexCapacity * static_cast<UInt32>(
						sizeof(NativeA8CrossFacadeGpuVertex)),
					indexBytes,
					kEstimatedSavedDrawNanoseconds,
					kEstimatedBatchCandidateNanoseconds,
					kEstimatedBatchPacketNanoseconds,
					kEstimatedBatchVertexNanoseconds,
					kEstimatedBatchDecisionNanoseconds,
					kEstimatedBatchFrameUploadNanoseconds);
			}
			return true;
		}

		void MarkDuplicateItems(
			const std::vector<NativeA8CrossFacadeFrameItem>& items,
			CrossFacadeFrameState& frame)
		{
			frame.duplicateItems.assign(items.size(), 0);
			PrepareLookup(frame.occurrenceLookup, items.size());
			const size_t mask = frame.occurrenceLookup.size() - 1u;
			for (size_t index = 0; index < items.size(); ++index)
			{
				NiTriShape* facade = items[index].facade;
				if (!facade || items[index].barrier)
					continue;
				size_t slot = HashPointer(facade) & mask;
				for (;;)
				{
					const UInt32 stored =
						frame.occurrenceLookup[slot];
					if (!stored)
					{
						frame.occurrenceLookup[slot] =
							static_cast<UInt32>(index + 1u);
						break;
					}
					const size_t previous =
						static_cast<size_t>(stored - 1u);
					if (previous < items.size()
						&& items[previous].facade == facade)
					{
						frame.duplicateItems[previous] = 1;
						frame.duplicateItems[index] = 1;
						break;
					}
					slot = (slot + 1u) & mask;
				}
			}
		}

		void MaterializeSegments(
			const std::vector<NativeA8CrossFacadeFrameItem>& items,
			CrossFacadeFrameState& frame)
		{
			for (size_t segmentIndex = 0;
				segmentIndex < frame.segments.size(); ++segmentIndex)
			{
				CrossFacadeSegment& segment =
					frame.segments[segmentIndex];
				if (!segment.itemCount
					|| segment.firstItem >= items.size()
					|| segment.itemCount
						> items.size() - segment.firstItem)
				{
					continue;
				}
				segment.firstFacade = frame.facades.size();
				segment.firstPacket = frame.packets.size();
				bool complete = true;
				const size_t endItem =
					segment.firstItem + segment.itemCount;
				for (size_t itemIndex = segment.firstItem;
					itemIndex < endItem; ++itemIndex)
				{
					const NativeA8CrossFacadeFrameItem& item =
						items[itemIndex];
					frame.facades.push_back(item.facade);
					const std::vector<NativeA8PacketTemplate>& packets =
						GetNativeA8Packets(
							*item.payload->payloadTemplate,
							item.payload->useCompositePackets);
					for (size_t packetIndex = 0;
						packetIndex < packets.size(); ++packetIndex)
					{
						CrossFacadePacketRef packet;
						if (!BuildPacketRef(item,
							static_cast<UInt32>(packetIndex), packet))
						{
							complete = false;
							break;
						}
						frame.packets.push_back(packet);
					}
					if (!complete)
						break;
				}
				if (!complete)
				{
					frame.facades.resize(segment.firstFacade);
					frame.packets.resize(segment.firstPacket);
					segment.facadeCount = 0;
					segment.packetCount = 0;
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeResourceFallback);
					continue;
				}
				segment.facadeCount =
					frame.facades.size() - segment.firstFacade;
				segment.packetCount =
					frame.packets.size() - segment.firstPacket;
				if (segment.facadeCount < 2 || segment.packetCount < 2)
				{
					frame.facades.resize(segment.firstFacade);
					frame.packets.resize(segment.firstPacket);
					segment.facadeCount = 0;
					segment.packetCount = 0;
					continue;
				}
				BuildCandidatesForSegment(frame, segmentIndex);
			}
		}

		bool CandidateNetDensityGreater(
			const CrossFacadeCandidate& left,
			const CrossFacadeCandidate& right)
		{
			const UInt64 leftNet = left.estimatedSavedNanoseconds
				- left.estimatedMergeNanoseconds;
			const UInt64 rightNet = right.estimatedSavedNanoseconds
				- right.estimatedMergeNanoseconds;
			const UInt64 leftScore =
				leftNet * right.vertexCount;
			const UInt64 rightScore =
				rightNet * left.vertexCount;
			if (leftScore != rightScore)
				return leftScore > rightScore;
			if (left.packetCount != right.packetCount)
				return left.packetCount > right.packetCount;
			return left.firstPacket < right.firstPacket;
		}

		bool HasProfitableCandidateSet(CrossFacadeFrameState& frame)
		{
			UInt64 estimatedSavedNanoseconds = 0;
			UInt64 estimatedMergeNanoseconds =
				kEstimatedBatchDecisionNanoseconds
					+ kEstimatedBatchFrameUploadNanoseconds;
			for (const CrossFacadeCandidate& candidate : frame.candidates)
			{
				estimatedSavedNanoseconds +=
					candidate.estimatedSavedNanoseconds;
				estimatedMergeNanoseconds +=
					candidate.estimatedMergeNanoseconds;
			}
			if (estimatedSavedNanoseconds > estimatedMergeNanoseconds)
				return true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCostFallback,
				static_cast<UInt64>(frame.candidates.size()));
			return false;
		}

		UInt32 SelectCandidates(CrossFacadeFrameState& frame,
			UInt32 capacity)
		{
			frame.candidateOrder.resize(frame.candidates.size());
			std::iota(frame.candidateOrder.begin(),
				frame.candidateOrder.end(), size_t{ 0 });
			UInt64 allVertices = 0;
			for (const CrossFacadeCandidate& candidate : frame.candidates)
				allVertices += candidate.vertexCount;
			if (allVertices <= capacity)
			{
				for (CrossFacadeCandidate& candidate : frame.candidates)
					candidate.selected = true;
				return static_cast<UInt32>(allVertices);
			}
			std::stable_sort(frame.candidateOrder.begin(),
				frame.candidateOrder.end(),
				[&](size_t left, size_t right)
				{
					return CandidateNetDensityGreater(
						frame.candidates[left],
						frame.candidates[right]);
				});
			UInt32 selectedVertices = 0;
			UInt64 estimatedSavedNanoseconds = 0;
			UInt64 estimatedMergeNanoseconds =
				kEstimatedBatchDecisionNanoseconds
					+ kEstimatedBatchFrameUploadNanoseconds;
			UInt64 selectedCandidates = 0;
			for (size_t index : frame.candidateOrder)
			{
				CrossFacadeCandidate& candidate =
					frame.candidates[index];
				if (candidate.vertexCount <= capacity - selectedVertices)
				{
					candidate.selected = true;
					selectedVertices += candidate.vertexCount;
					estimatedSavedNanoseconds +=
						candidate.estimatedSavedNanoseconds;
					estimatedMergeNanoseconds +=
						candidate.estimatedMergeNanoseconds;
					++selectedCandidates;
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeCapacityFallback);
				}
			}
			if (!selectedCandidates || estimatedSavedNanoseconds
				<= estimatedMergeNanoseconds)
			{
				for (CrossFacadeCandidate& candidate : frame.candidates)
				{
					if (!candidate.selected)
						continue;
					candidate.selected = false;
					RecordFreeTypePerf(
						FreeTypePerfCounter::CrossFacadeCostFallback);
				}
				return 0;
			}
			return selectedVertices;
		}

		void RecordSelectedCandidateCost(
			const CrossFacadeFrameState& frame)
		{
			UInt64 selectedCandidates = 0;
			UInt64 estimatedSavedNanoseconds = 0;
			UInt64 estimatedMergeNanoseconds =
				kEstimatedBatchDecisionNanoseconds
					+ kEstimatedBatchFrameUploadNanoseconds;
			for (const CrossFacadeCandidate& candidate : frame.candidates)
			{
				if (!candidate.selected)
					continue;
				++selectedCandidates;
				estimatedSavedNanoseconds +=
					candidate.estimatedSavedNanoseconds;
				estimatedMergeNanoseconds +=
					candidate.estimatedMergeNanoseconds;
			}
			if (!selectedCandidates)
				return;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCostSelected,
				selectedCandidates);
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CrossFacadeEstimatedSavedNanoseconds,
				estimatedSavedNanoseconds);
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CrossFacadeEstimatedMergeNanoseconds,
				estimatedMergeNanoseconds);
		}

		bool IsLiveTileRgb(const NativeA8PacketTemplate& packet,
			const NativeA8GpuVertex& vertex)
		{
			if (packet.shaderClass == NativeA8ShaderClass::Argb)
				return true;
			if (packet.shaderClass == NativeA8ShaderClass::Coverage)
				return vertex.layerMask >= 0.5f;
			return packet.usesLiveTileRgb;
		}

		bool FillCandidateVertices(CrossFacadeFrameState& frame,
			CrossFacadeCandidate& candidate,
			NativeA8CrossFacadeGpuVertex* destination)
		{
			if (!destination || !candidate.packetCount)
				return false;
			const CrossFacadePacketRef& first =
				frame.packets[candidate.firstPacket];
			NiTransform leader = first.effectiveWorld;
			if (!IsFiniteTransform(leader))
				return false;
			NiTransform leaderInverse = leader.GetInverse();
			if (!IsFiniteTransform(leaderInverse))
				return false;

			NiPoint3 minimum(
				std::numeric_limits<float>::max());
			NiPoint3 maximum(
				-std::numeric_limits<float>::max());
			UInt32 outputIndex = 0;
			for (size_t sourceIndex = 0;
				sourceIndex < candidate.packetCount; ++sourceIndex)
			{
				const CrossFacadePacketRef& source =
					frame.packets[candidate.firstPacket + sourceIndex];
				const NativeA8PayloadTemplate& artifact =
					*source.payload->payloadTemplate;
				const UInt32 firstVertex = source.packet->firstVertex;
				const UInt32 endVertex =
					firstVertex + source.packet->vertexCount;
				for (UInt32 vertexIndex = firstVertex;
					vertexIndex < endVertex; ++vertexIndex)
				{
					const NativeA8GpuVertex& input =
						artifact.gpuVertices[vertexIndex];
					const NiPoint3 local(input.x, input.y, input.z);
					const NiPoint3 adjusted =
						leaderInverse
							* (source.effectiveWorld * local);
					if (!std::isfinite(adjusted.x)
						|| !std::isfinite(adjusted.y)
						|| !std::isfinite(adjusted.z))
					{
						return false;
					}
					NativeA8CrossFacadeGpuVertex& output =
						destination[outputIndex++];
					output.source = input;
					output.source.x = adjusted.x;
					output.source.y = adjusted.y;
					output.source.z = adjusted.z;
					const bool liveRgb =
						IsLiveTileRgb(*source.packet, input);
					output.tileR = liveRgb
						? source.tileColor.r : 1.0f;
					output.tileG = liveRgb
						? source.tileColor.g : 1.0f;
					output.tileB = liveRgb
						? source.tileColor.b : 1.0f;
					output.tileA = source.tileColor.a;
					minimum.x = std::min(minimum.x, adjusted.x);
					minimum.y = std::min(minimum.y, adjusted.y);
					minimum.z = std::min(minimum.z, adjusted.z);
					maximum.x = std::max(maximum.x, adjusted.x);
					maximum.y = std::max(maximum.y, adjusted.y);
					maximum.z = std::max(maximum.z, adjusted.z);
				}
			}
			if (outputIndex != candidate.vertexCount)
				return false;
			candidate.bound.m_kCenter =
				(minimum + maximum) * 0.5f;
			float radiusSquared = 0.0f;
			for (UInt32 index = 0; index < outputIndex; ++index)
			{
				const NativeA8GpuVertex& vertex =
					destination[index].source;
				const NiPoint3 delta(vertex.x - candidate.bound.m_kCenter.x,
					vertex.y - candidate.bound.m_kCenter.y,
					vertex.z - candidate.bound.m_kCenter.z);
				radiusSquared = std::max(
					radiusSquared, delta.SqrLength());
			}
			candidate.bound.m_fRadius = std::sqrt(radiusSquared);
			return std::isfinite(candidate.bound.m_fRadius);
		}

		bool UploadSelectedCandidates(CrossFacadeFrameState& frame,
			CrossFacadeGpuState& gpu, UInt32 totalVertices)
		{
			if (!totalVertices || !gpu.vertexBuffer
				|| totalVertices > gpu.vertexCapacity)
			{
				return false;
			}
			frame.candidateOrder.erase(
				std::remove_if(frame.candidateOrder.begin(),
					frame.candidateOrder.end(),
					[&](size_t index)
					{
						return index >= frame.candidates.size()
							|| !frame.candidates[index].selected;
					}),
				frame.candidateOrder.end());
			const auto packetOrder =
				[&](size_t left, size_t right)
				{
					return frame.candidates[left].firstPacket
						< frame.candidates[right].firstPacket;
				};
			if (!std::is_sorted(frame.candidateOrder.begin(),
					frame.candidateOrder.end(), packetOrder))
			{
				std::sort(frame.candidateOrder.begin(),
					frame.candidateOrder.end(), packetOrder);
			}

			const UINT byteCount = totalVertices
				* sizeof(NativeA8CrossFacadeGpuVertex);
			void* memory = nullptr;
			HRESULT result = gpu.vertexBuffer->Lock(
				0, byteCount, &memory, D3DLOCK_DISCARD);
			if (FAILED(result) || !memory)
				return false;
			auto* destination =
				static_cast<NativeA8CrossFacadeGpuVertex*>(memory);
			UInt32 baseVertex = 0;
			bool complete = true;
			for (size_t index : frame.candidateOrder)
			{
				CrossFacadeCandidate& candidate =
					frame.candidates[index];
				candidate.baseVertex = baseVertex;
				if (!FillCandidateVertices(frame, candidate,
						destination + baseVertex))
				{
					complete = false;
					break;
				}
				baseVertex += candidate.vertexCount;
			}
			result = gpu.vertexBuffer->Unlock();
			if (!complete || baseVertex != totalVertices
				|| FAILED(result))
			{
				return false;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeVertex, totalVertices);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeUploadBytes, byteCount);
			return true;
		}

		void InsertDispatchEntry(CrossFacadeFrameState& frame,
			const CrossFacadeDispatchEntry& entry)
		{
			const size_t mask = frame.dispatchLookup.size() - 1u;
			size_t slot = HashPointer(entry.facade) & mask;
			while (frame.dispatchLookup[slot])
				slot = (slot + 1u) & mask;
			frame.dispatchEntries.push_back(entry);
			frame.dispatchLookup[slot] = static_cast<UInt32>(
				frame.dispatchEntries.size());
		}

		const CrossFacadeDispatchEntry* FindDispatchEntry(
			const CrossFacadeFrameState& frame, NiTriShape* facade)
		{
			if (!facade || frame.dispatchLookup.empty())
				return nullptr;
			const size_t mask = frame.dispatchLookup.size() - 1u;
			size_t slot = HashPointer(facade) & mask;
			for (size_t probe = 0;
				probe < frame.dispatchLookup.size(); ++probe)
			{
				const UInt32 stored = frame.dispatchLookup[slot];
				if (!stored)
					return nullptr;
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < frame.dispatchEntries.size()
					&& frame.dispatchEntries[index].facade == facade)
				{
					return &frame.dispatchEntries[index];
				}
				slot = (slot + 1u) & mask;
			}
			return nullptr;
		}

		void BuildExecutionPlan(CrossFacadeFrameState& frame)
		{
			frame.candidateAtPacket.assign(frame.packets.size(), -1);
			for (size_t index = 0;
				index < frame.candidates.size(); ++index)
			{
				const CrossFacadeCandidate& candidate =
					frame.candidates[index];
				if (candidate.selected
					&& candidate.firstPacket
						< frame.candidateAtPacket.size())
				{
					frame.candidateAtPacket[candidate.firstPacket] =
						static_cast<SInt32>(index);
					frame.segments[candidate.segmentIndex].executable = true;
				}
			}

			size_t executableFacades = 0;
			for (const CrossFacadeSegment& segment : frame.segments)
			{
				if (segment.executable)
					executableFacades += segment.facadeCount;
			}
			PrepareLookup(frame.dispatchLookup, executableFacades);
			for (size_t segmentIndex = 0;
				segmentIndex < frame.segments.size(); ++segmentIndex)
			{
				CrossFacadeSegment& segment =
					frame.segments[segmentIndex];
				if (!segment.executable)
					continue;
				segment.firstCommand = frame.commands.size();
				size_t packet = segment.firstPacket;
				const size_t end =
					segment.firstPacket + segment.packetCount;
				while (packet < end)
				{
					const SInt32 candidateIndex =
						frame.candidateAtPacket[packet];
					if (candidateIndex >= 0)
					{
						CrossFacadeCommand command;
						command.type = CrossFacadeCommandType::Batch;
						command.packetOrCandidate =
							static_cast<size_t>(candidateIndex);
						frame.commands.push_back(command);
						packet += frame.candidates[
							static_cast<size_t>(candidateIndex)].packetCount;
					}
					else
					{
						CrossFacadeCommand command;
						command.type = CrossFacadeCommandType::Ordinary;
						command.packetOrCandidate = packet;
						frame.commands.push_back(command);
						++packet;
					}
				}
				segment.commandCount =
					frame.commands.size() - segment.firstCommand;
				for (size_t facadeIndex = 0;
					facadeIndex < segment.facadeCount; ++facadeIndex)
				{
					InsertDispatchEntry(frame, {
						frame.facades[
							segment.firstFacade + facadeIndex],
						segmentIndex,
						facadeIndex == 0
					});
				}
			}
		}

		struct RingSubmissionGuard
		{
			~RingSubmissionGuard()
			{
				EndNativeA8RingSubmission(submission);
			}
			NativeA8RingSubmission submission;
		};

		struct GeometrySelectionGuard
		{
			// Retail 0xB64F90 reuses the accumulator's one embedded RenderPass
			// (this+0x1A4) for the entire reverse traversal and changes only its
			// pGeometry before calling 0xB994F0. A leader can therefore execute
			// later packet commands through this same pass without fabricating or
			// retaining another RenderPass object.
			explicit GeometrySelectionGuard(
				BSShaderProperty::RenderPass* source)
				: pass(source),
				original(source ? source->pGeometry : nullptr)
			{
			}
			~GeometrySelectionGuard()
			{
				if (pass)
					pass->pGeometry = original;
			}
			void Select(NiGeometry* geometry)
			{
				pass->pGeometry = geometry;
			}
			BSShaderProperty::RenderPass* pass = nullptr;
			NiGeometry* original = nullptr;
		};

		struct FacadeShaderScope
		{
			FacadeShaderScope()
			{
				BeginNativeA8FacadeShaderBatch();
			}
			~FacadeShaderScope()
			{
				EndNativeA8FacadeShaderBatch();
			}
		};

		bool AdvanceSubmissionToPacket(CrossFacadePacketRef& source,
			NativeA8RingSubmission& submission)
		{
			while (submission.nextPacket < source.packetIndex)
			{
				const NativeA8FallbackReason skipped =
					SkipNativeA8RingPacket(*source.payload, submission,
						submission.nextPacket);
				if (skipped != NativeA8FallbackReason::None)
					return false;
			}
			return submission.nextPacket == source.packetIndex;
		}

		bool EnsurePacketConstantIsolation(
			const NativeA8PacketTemplate& packet, UInt32 generation)
		{
			if (packet.shaderClass == NativeA8ShaderClass::Coverage
				|| packet.shaderClass == NativeA8ShaderClass::Argb)
			{
				return true;
			}
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device =
				renderer ? renderer->GetD3DDevice() : nullptr;
			return device
				&& EnsureA8SortedTileConstantCapture(device, generation);
		}

		bool DrawOrdinaryPacket(BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool setupDrawmode,
			CrossFacadePacketRef& source)
		{
			RingSubmissionGuard ring;
			NativeA8FallbackReason failure =
				BeginNativeA8RingSubmission(source.facade,
					*source.payload, ring.submission);
			if (failure != NativeA8FallbackReason::None
				|| !AdvanceSubmissionToPacket(source,
					ring.submission))
			{
				RecordNativeA8Suppression(source.facade,
					*source.metadata,
					failure != NativeA8FallbackReason::None
						? failure : NativeA8FallbackReason::PacketPrepare,
					"cross-facade-ordinary");
				return false;
			}
			NiTriShape* proxy = nullptr;
			failure = PrepareNativeA8RingPacket(source.facade,
				*source.payload, ring.submission,
				source.packetIndex, proxy);
			if (failure != NativeA8FallbackReason::None || !proxy
				|| !EnsurePacketConstantIsolation(*source.packet,
					source.payload->preparedGeneration))
			{
				RecordNativeA8Suppression(source.facade,
					*source.metadata,
					failure != NativeA8FallbackReason::None
						? failure : NativeA8FallbackReason::RuntimeFault,
					"cross-facade-ordinary");
				return false;
			}

			GeometrySelectionGuard geometry(pass);
			geometry.Select(proxy);
			FacadeShaderScope shaderScope;
			State().originalTileRenderPass(pass, currentPass,
				false, true, setupDrawmode);
			RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
			if (source.packet->shaderClass
				== NativeA8ShaderClass::Composite)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::CompositeDraw);
			}
			return IsNativeA8ShaderGenerationCurrent(
				source.payload->preparedGeneration);
		}

		struct ProxyBufferSnapshot
		{
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			void* declaration = nullptr;
			UInt32 stride = 0;
			UInt32 chipOffset = 0;
			UInt32 chipLockFlags = 0;
			UInt32 chipSize = 0;
			UInt32 indexBytes = 0;
			UInt32 vertexCount = 0;
			UInt32 maxVertexCount = 0;
			UInt32 indexCount = 0;
			UInt32 baseVertex = 0;
			UInt32 triangleCount = 0;
			UInt32 maxTriangleCount = 0;
			UInt32 arrayCount = 0;
			D3DPRIMITIVETYPE primitiveType = D3DPT_FORCE_DWORD;
			NiBound bound;
			BSShader* shader = nullptr;
		};

		void CaptureProxyBuffer(NiTriShape* proxy,
			NiGeometryBufferData* buffer, NiVBChip* chip,
			ProxyBufferSnapshot& snapshot)
		{
			snapshot.vertexBuffer = chip->m_pkVB;
			snapshot.indexBuffer = buffer->m_pkIB;
			snapshot.declaration = buffer->m_hDeclaration;
			snapshot.stride = buffer->m_puiVertexStride
				? buffer->m_puiVertexStride[0] : 0;
			snapshot.chipOffset = chip->m_uiOffset;
			snapshot.chipLockFlags = chip->m_uiLockFlags;
			snapshot.chipSize = chip->m_uiSize;
			snapshot.indexBytes = buffer->m_uiIBSize;
			snapshot.vertexCount = buffer->m_uiVertCount;
			snapshot.maxVertexCount = buffer->m_uiMaxVertCount;
			snapshot.indexCount = buffer->m_uiIndexCount;
			snapshot.baseVertex = buffer->m_uiBaseVertexIndex;
			snapshot.triangleCount = buffer->m_uiTriCount;
			snapshot.maxTriangleCount = buffer->m_uiMaxTriCount;
			snapshot.arrayCount = buffer->m_uiNumArrays;
			snapshot.primitiveType = buffer->m_eType;
			snapshot.bound = proxy->GetModelData()->m_kBound;
			snapshot.shader = proxy->GetShader();
		}

		void RestoreProxyBuffer(NiTriShape* proxy,
			NiGeometryBufferData* buffer, NiVBChip* chip,
			const ProxyBufferSnapshot& snapshot)
		{
			proxy->SetShader(snapshot.shader);
			chip->m_pkVB = snapshot.vertexBuffer;
			chip->m_uiOffset = snapshot.chipOffset;
			chip->m_uiLockFlags = snapshot.chipLockFlags;
			chip->m_uiSize = snapshot.chipSize;
			buffer->m_pkIB = snapshot.indexBuffer;
			buffer->m_hDeclaration = snapshot.declaration;
			if (buffer->m_puiVertexStride)
				buffer->m_puiVertexStride[0] = snapshot.stride;
			buffer->m_uiIBSize = snapshot.indexBytes;
			buffer->m_uiVertCount = snapshot.vertexCount;
			buffer->m_uiMaxVertCount = snapshot.maxVertexCount;
			buffer->m_uiIndexCount = snapshot.indexCount;
			buffer->m_uiBaseVertexIndex = snapshot.baseVertex;
			buffer->m_uiTriCount = snapshot.triangleCount;
			buffer->m_uiMaxTriCount = snapshot.maxTriangleCount;
			buffer->m_uiNumArrays = snapshot.arrayCount;
			buffer->m_eType = snapshot.primitiveType;
			proxy->GetModelData()->m_kBound = snapshot.bound;
		}

		bool DrawBatchCandidate(BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool setupDrawmode,
			CrossFacadeFrameState& frame,
			CrossFacadeCandidate& candidate, bool& drew)
		{
			drew = false;
			CrossFacadeGpuState& gpu = GpuState();
			if (!gpu.vertexBuffer || !gpu.indexBuffer || !gpu.declaration
				|| gpu.generation != frame.generation)
			{
				FaultCrossFacadeGeneration(frame.generation);
				return false;
			}
			CrossFacadePacketRef& first =
				frame.packets[candidate.firstPacket];
			RingSubmissionGuard ring;
			NativeA8FallbackReason failure =
				BeginNativeA8RingSubmission(first.facade,
					*first.payload, ring.submission);
			if (failure != NativeA8FallbackReason::None
				|| !AdvanceSubmissionToPacket(first,
					ring.submission))
			{
				return false;
			}
			NiTriShape* proxy = nullptr;
			failure = PrepareNativeA8RingPacket(first.facade,
				*first.payload, ring.submission,
				first.packetIndex, proxy);
			if (failure != NativeA8FallbackReason::None || !proxy
				|| !ring.submission.proxyBuffer
				|| !ring.submission.proxyChip
				|| !first.batchShader
				|| !EnsurePacketConstantIsolation(*first.packet,
					first.payload->preparedGeneration))
			{
				return false;
			}

			NiGeometryBufferData* buffer =
				ring.submission.proxyBuffer;
			NiVBChip* chip = ring.submission.proxyChip;
			NiTriShapeData* data = proxy->GetModelData();
			if (!data || !buffer->m_puiVertexStride)
				return false;
			ProxyBufferSnapshot snapshot;
			CaptureProxyBuffer(proxy, buffer, chip, snapshot);

			const UInt32 quadCount = candidate.vertexCount / 4u;
			proxy->SetShader(first.batchShader);
			if (proxy->GetShader() != first.batchShader)
			{
				RestoreProxyBuffer(proxy, buffer, chip, snapshot);
				FaultCrossFacadeGeneration(frame.generation);
				return false;
			}
			chip->m_pkVB = gpu.vertexBuffer;
			chip->m_uiOffset = 0;
			chip->m_uiLockFlags = 0;
			chip->m_uiSize = candidate.vertexCount
				* sizeof(NativeA8CrossFacadeGpuVertex);
			buffer->m_pkIB = gpu.indexBuffer;
			buffer->m_hDeclaration = gpu.declaration;
			buffer->m_puiVertexStride[0] =
				sizeof(NativeA8CrossFacadeGpuVertex);
			buffer->m_uiIBSize = gpu.indexBytes;
			buffer->m_uiVertCount = candidate.vertexCount;
			buffer->m_uiMaxVertCount = candidate.vertexCount;
			buffer->m_uiIndexCount = quadCount * 6u;
			buffer->m_uiBaseVertexIndex = candidate.baseVertex;
			buffer->m_eType = D3DPT_TRIANGLELIST;
			buffer->m_uiTriCount = quadCount * 2u;
			buffer->m_uiMaxTriCount = quadCount * 2u;
			buffer->m_uiNumArrays = kCanonicalArrayCount;
			data->m_kBound = candidate.bound;

			{
				GeometrySelectionGuard geometry(pass);
				geometry.Select(proxy);
				FacadeShaderScope shaderScope;
				State().originalTileRenderPass(pass, currentPass,
					false, true, setupDrawmode);
				drew = true;
			}
			RestoreProxyBuffer(proxy, buffer, chip, snapshot);
			if (!IsNativeA8ShaderGenerationCurrent(
					first.payload->preparedGeneration))
			{
				return false;
			}

			RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
			if (first.packet->shaderClass
				== NativeA8ShaderClass::Composite)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::CompositeDraw);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::CrossFacadeSourceDraw,
				static_cast<UInt64>(candidate.packetCount));
			RecordFreeTypePerf(FreeTypePerfCounter::CrossFacadeDraw);
			RecordFreeTypePerf(FreeTypePerfCounter::CrossFacadeDrawSaved,
				static_cast<UInt64>(candidate.packetCount - 1u));
			static bool loggedFirstBatch = false;
			if (g_bEnableFreeTypeFontRenderingLog && !loggedFirstBatch)
			{
				loggedFirstBatch = true;
				FreeTypeFontDebugLog(
					"tnvse_freetype_native: cross-facade batch generation=%u facades=%u sourceDraws=%u draws=1 saved=%u vertices=%u bytes=%u texture=%p shader=%p",
					frame.generation, candidate.facadeCount,
					static_cast<UInt32>(candidate.packetCount),
					static_cast<UInt32>(candidate.packetCount - 1u),
					candidate.vertexCount,
					candidate.vertexCount * static_cast<UInt32>(
						sizeof(NativeA8CrossFacadeGpuVertex)),
					first.d3dTexture, first.batchShader);
			}
			return true;
		}

		bool ExecuteSegment(BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool setupDrawmode,
			CrossFacadeFrameState& frame,
			CrossFacadeSegment& segment)
		{
			bool result = true;
			const size_t end =
				segment.firstCommand + segment.commandCount;
			for (size_t commandIndex = segment.firstCommand;
				commandIndex < end; ++commandIndex)
			{
				const CrossFacadeCommand& command =
					frame.commands[commandIndex];
				if (command.type == CrossFacadeCommandType::Ordinary)
				{
					if (!DrawOrdinaryPacket(pass, currentPass,
							setupDrawmode,
							frame.packets[command.packetOrCandidate]))
					{
						result = false;
						break;
					}
					continue;
				}

				CrossFacadeCandidate& candidate =
					frame.candidates[command.packetOrCandidate];
				bool drew = false;
				if (DrawBatchCandidate(pass, currentPass,
						setupDrawmode, frame, candidate, drew))
				{
					continue;
				}
				if (drew)
				{
					result = false;
					break;
				}
				// A proxy or optional batch-resource failure before the combined
				// draw remains recoverable. Replay only this candidate's original
				// packet sequence; already completed commands are never repeated.
				RecordFreeTypePerf(
					FreeTypePerfCounter::CrossFacadeResourceFallback);
				for (size_t source = 0;
					source < candidate.packetCount; ++source)
				{
					if (!DrawOrdinaryPacket(pass, currentPass,
							setupDrawmode,
							frame.packets[
								candidate.firstPacket + source]))
					{
						result = false;
						break;
					}
				}
				if (!result)
					break;
			}
			segment.completed = true;
			return result;
		}
	}

	NativeA8CrossFacadePrepareResult PrepareNativeA8CrossFacadeFrame(
		const std::vector<NativeA8CrossFacadeFrameItem>& items,
		UInt32 generation)
	{
		CrossFacadeFrameState& frame = s_frame;
		FreeTypePerfScope perf(FreeTypePerfPhase::CrossFacadePrepare);
		ClearFrame(frame, false);
		if (items.size() < 2)
			return NativeA8CrossFacadePrepareResult::NoCandidate;
		if (!generation
			|| !IsNativeA8ShaderGenerationCurrent(generation))
			return NativeA8CrossFacadePrepareResult::Unavailable;
		{
			CrossFacadeGpuState& gpu = GpuState();
			std::lock_guard<std::mutex> lock(gpu.mutex);
			if (gpu.faultGeneration == generation)
				return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		if (!GetNativeA8CrossFacadeD3DDeclaration(generation))
		{
			CrossFacadeGpuState& gpu = GpuState();
			std::lock_guard<std::mutex> lock(gpu.mutex);
			if (!gpu.generation || gpu.generation == generation)
			{
				ReleaseGpuLocked(gpu);
				gpu.faultGeneration = generation;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeResourceFallback);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}

		MarkDuplicateItems(items, frame);
		const CrossFacadeOptimisticGate optimisticGate =
			BuildOptimisticSegments(items, frame);
		RefreshFrameMemory(frame);
		EnforceCpuMemoryBudget("cross-facade-descriptors");
		if (IsCpuMemoryBudgetExceeded())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCapacityFallback);
			ClearFrame(frame, true);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		if (optimisticGate == CrossFacadeOptimisticGate::NoCandidate)
			return NativeA8CrossFacadePrepareResult::NoCandidate;
		if (optimisticGate == CrossFacadeOptimisticGate::CostRejected)
			return NativeA8CrossFacadePrepareResult::CostRejected;

		MaterializeSegments(items, frame);
		RefreshFrameMemory(frame);
		EnforceCpuMemoryBudget("cross-facade-materialized");
		if (IsCpuMemoryBudgetExceeded())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCapacityFallback);
			ClearFrame(frame, true);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		if (frame.candidates.empty())
			return NativeA8CrossFacadePrepareResult::NoCandidate;
		if (!HasProfitableCandidateSet(frame))
		{
			RefreshFrameMemory(frame);
			return NativeA8CrossFacadePrepareResult::CostRejected;
		}

		CrossFacadeGpuState& gpu = GpuState();
		std::lock_guard<std::mutex> lock(gpu.mutex);
		if (!EnsureGpuLocked(gpu, generation))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeResourceFallback);
			RefreshFrameMemory(frame);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		const UInt32 selectedVertices =
			SelectCandidates(frame, gpu.vertexCapacity);
		if (!selectedVertices)
		{
			for (CrossFacadeCandidate& candidate : frame.candidates)
				candidate.selected = false;
			RefreshFrameMemory(frame);
			return NativeA8CrossFacadePrepareResult::CostRejected;
		}
		if (!UploadSelectedCandidates(frame, gpu, selectedVertices))
		{
			for (CrossFacadeCandidate& candidate : frame.candidates)
				candidate.selected = false;
			ReleaseGpuLocked(gpu);
			gpu.faultGeneration = generation;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeResourceFallback);
			RefreshFrameMemory(frame);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		RecordSelectedCandidateCost(frame);

		BuildExecutionPlan(frame);
		frame.generation = generation;
		frame.prepared = !frame.dispatchEntries.empty();
		RefreshFrameMemory(frame);
		EnforceCpuMemoryBudget("cross-facade-frame");
		if (IsCpuMemoryBudgetExceeded())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeCapacityFallback);
			ClearFrame(frame, true);
			return NativeA8CrossFacadePrepareResult::Unavailable;
		}
		return frame.prepared
			? NativeA8CrossFacadePrepareResult::Prepared
			: NativeA8CrossFacadePrepareResult::NoCandidate;
	}

	void ResetNativeA8CrossFacadeFrame()
	{
		ClearFrame(s_frame, false);
	}

	void BeginNativeA8CrossFacadeFrameDispatch()
	{
		if (s_frame.prepared)
			++s_frame.dispatchDepth;
	}

	void SuspendNativeA8CrossFacadeFrameDispatch()
	{
		if (s_frame.dispatchDepth)
			++s_frame.suspendDepth;
	}

	void ResumeNativeA8CrossFacadeFrameDispatch()
	{
		if (s_frame.suspendDepth)
			--s_frame.suspendDepth;
	}

	void EndNativeA8CrossFacadeFrameDispatch()
	{
		if (s_frame.dispatchDepth)
			--s_frame.dispatchDepth;
		if (!s_frame.dispatchDepth)
			ClearFrame(s_frame, false);
	}

	NativeA8CrossFacadeDispatch DispatchNativeA8CrossFacadeFrame(
		BSShaderProperty::RenderPass* pass, UInt32 currentPass,
		bool setupDrawmode, NiTriShape* facade)
	{
		CrossFacadeFrameState& frame = s_frame;
		if (!frame.prepared || !frame.dispatchDepth
			|| frame.suspendDepth || !pass || !facade
			|| !State().originalTileRenderPass
			|| !IsNativeA8ShaderGenerationCurrent(frame.generation))
		{
			return NativeA8CrossFacadeDispatch::NotHandled;
		}
		const CrossFacadeDispatchEntry* dispatch =
			FindDispatchEntry(frame, facade);
		if (!dispatch || dispatch->segmentIndex >= frame.segments.size())
			return NativeA8CrossFacadeDispatch::NotHandled;
		FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
		CrossFacadeSegment& segment =
			frame.segments[dispatch->segmentIndex];
		if (!dispatch->leader)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeFollowerSkipped);
			return NativeA8CrossFacadeDispatch::FollowerSuppressed;
		}
		if (!segment.completed)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeGroup);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CrossFacadeFacade,
				static_cast<UInt64>(segment.facadeCount));
			ExecuteSegment(pass, currentPass, setupDrawmode,
				frame, segment);
		}
		return NativeA8CrossFacadeDispatch::LeaderExecuted;
	}

	void ReleaseNativeA8CrossFacadeResources()
	{
		CrossFacadeGpuState& state = GpuState();
		std::lock_guard<std::mutex> lock(state.mutex);
		ReleaseGpuLocked(state);
		state.faultGeneration = 0;
		state.loggedReady = false;
	}
}
