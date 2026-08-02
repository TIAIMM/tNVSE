#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiD3DRenderState.hpp"
#include "NiDX9Renderer.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiStencilProperty.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShapeData.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace fonthook::vectorfont
{
	// Admission, leader-time upload and the D3D9 restore bracket deliberately
	// share this TU: all three mutate one render-thread-only CrossTextFrame and
	// InstancingResources lifetime. Splitting the marginally large module would
	// expose those state objects across translation units and weaken fail-open
	// ordering guarantees.
	namespace implementation::font_native_instancing {}
	using namespace implementation::font_native_instancing;

	namespace implementation::font_native_instancing
	{
		struct InstancingTilePropertyView : BSShaderProperty
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
		static_assert(sizeof(InstancingTilePropertyView) == 0xB0);
		static_assert(offsetof(
			InstancingTilePropertyView, sourceTexture) == 0x60);
		static_assert(offsetof(
			InstancingTilePropertyView, overlayColor) == 0x68);
		static_assert(offsetof(
			InstancingTilePropertyView, scissorRect) == 0x9C);

		struct NativeA8GlyphInstance
		{
			NativeA8GlyphInstanceSidecar sidecar;
			D3DXMATRIX wvpColumns = {};
			std::array<float, 4> tileColor = {};
		};
		static_assert(sizeof(NativeA8GlyphInstance)
			== kNativeA8GlyphInstanceBytes);
		static_assert(offsetof(NativeA8GlyphInstance, sidecar) == 0);
		static_assert(offsetof(NativeA8GlyphInstance, wvpColumns) == 72);
		static_assert(offsetof(NativeA8GlyphInstance, tileColor) == 136);

		struct CrossTextSequenceItem
		{
			NativeA8CrossTextCommandKind kind =
				NativeA8CrossTextCommandKind::Barrier;
			NiTriShape* geometry = nullptr;
			const A8ShapeMetadata* metadata = nullptr;
			NativeA8ShapePayload* payload = nullptr;
			UInt32 commandIndex = kInvalidNativeA8CommandIndex;
			UInt32 depthBits = 0;
		};

		struct CrossTextCompatibilityKey
		{
			const NativeA8CompiledPacketCommand* program = nullptr;
			IDirect3DVertexDeclaration9* normalDeclaration = nullptr;
			const NiTexture* sourceTexture = nullptr;
			const NiTexture* alphaTexture = nullptr;
			const void* atlasTexture = nullptr;
			std::array<float, kNativeA8PacketConstantFloatCount> constants = {};
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
			NativeA8Sampling sampling = NativeA8Sampling::Point;
			EffectQuality quality = EffectQuality::Balanced;
			DistanceFieldMethod distanceFieldMethod =
				DistanceFieldMethod::Mtsdf;
			UInt32 layer = 0;
			UInt16 atlasPage = 0;
			UInt16 alphaFlags = 0;
			UInt8 alphaTestRef = 0;
			UInt8 textureModeFlags = 0;
			std::array<UInt32, 2> shaderFlags = {};
			float shaderAlpha = 1.0f;
			float shaderFadeAlpha = 1.0f;
		};

		// Retained command-build result.  This is immutable after admission and
		// deliberately contains no transient state, WVP, TileColor, or retail-world
		// mirror.  Those values belong to the batch-local leader-time scratch below.
		struct CrossTextAdmissionMember
		{
			UInt32 sequenceIndex = kInvalidNativeA8CommandIndex;
			CrossTextSequenceItem sequence;
			const NativeA8DrawCommand* command = nullptr;
			const NativeA8PacketTemplate* packet = nullptr;
			const NativeA8GlyphInstanceSidecar* sidecars = nullptr;
			UInt32 sidecarCount = 0;
		};

		// Temporary admission-only grouping evidence.  Only the immutable member is
		// retained after a batch is accepted.
		struct CrossTextAdmissionCandidate
		{
			CrossTextAdmissionMember member;
			CrossTextCompatibilityKey compatibility;
			NativeTileInstancingTransientState transient;
		};

		// Leader-time pass-1 output.  It exists only for the batch currently being
		// prepared and never overwrites the retained admission plan.
		struct CrossTextLivePreflight
		{
			CrossTextCompatibilityKey compatibility;
			NativeTileInstancingTransientState transient;
		};

		enum class CrossTextBatchState : UInt8
		{
			Ready = 0,
			Executing,
			Consumed,
			Fallback
		};

		struct CrossTextBatch
		{
			UInt32 firstMember = 0;
			UInt32 memberCount = 0;
			UInt32 baseInstance = 0;
			UInt32 instanceCount = 0;
			UInt32 followersBegun = 0;
			CrossTextBatchState state = CrossTextBatchState::Ready;
		};

		struct InstancingResources
		{
			IDirect3DDevice9* device = nullptr;
			IDirect3DVertexBuffer9* cornerBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexBuffer9* instanceBuffer = nullptr;
			UInt32 instanceBufferBytes = 0;
			UInt32 generation = 0;
			UInt32 disabledGeneration = 0;
			UInt32 stateProofGeneration = 0;
		};

		struct CrossTextFrame
		{
			std::vector<CrossTextSequenceItem> sequence;
			std::vector<CrossTextAdmissionMember> admissionMembers;
			std::vector<CrossTextBatch> batches;
			std::vector<UInt32> sequenceToBatch;
			std::vector<CrossTextLivePreflight> livePreflight;
			std::vector<NativeTileInstancingSnapshot> liveSnapshots;
			CpuMemoryLease cpuMemory;
			UInt64 validationToken = 0;
			UInt32 generation = 0;
			UInt32 atlasTextureEpoch = 0;
			UInt32 resourceSerial = 0;
			UInt32 uploadEpoch = 0;
			UInt32 plannedInstanceCount = 0;
			UInt32 uploadCursorInstances = 0;
			UInt32 liveBatchIndex = kInvalidNativeA8CommandIndex;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			BSShaderAccumulator* accumulator = nullptr;
			bool building = false;
			bool active = false;
			bool generationUnavailable = false;
			bool uploadStarted = false;
		};

		CrossTextFrame s_crossTextFrame;
		InstancingResources s_instancingResources;

		template <class T>
		void ReleaseD3D(T*& value)
		{
			if (value)
			{
				value->Release();
				value = nullptr;
			}
		}

		void RefreshFrameMemory()
		{
			CrossTextFrame& frame = s_crossTextFrame;
			const size_t bytes = sizeof(frame)
				+ frame.sequence.capacity() * sizeof(CrossTextSequenceItem)
				+ frame.admissionMembers.capacity()
					* sizeof(CrossTextAdmissionMember)
				+ frame.batches.capacity() * sizeof(CrossTextBatch)
				+ frame.sequenceToBatch.capacity() * sizeof(UInt32)
				+ frame.livePreflight.capacity()
					* sizeof(CrossTextLivePreflight)
				+ frame.liveSnapshots.capacity()
					* sizeof(NativeTileInstancingSnapshot);
			frame.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ClearFrameVectors()
		{
			CrossTextFrame& frame = s_crossTextFrame;
			frame.sequence.clear();
			frame.admissionMembers.clear();
			frame.batches.clear();
			frame.sequenceToBatch.clear();
			frame.livePreflight.clear();
			frame.liveSnapshots.clear();
			frame.validationToken = 0;
			frame.generation = 0;
			frame.atlasTextureEpoch = 0;
			frame.resourceSerial = 0;
			frame.uploadEpoch = 0;
			frame.plannedInstanceCount = 0;
			frame.uploadCursorInstances = 0;
			frame.liveBatchIndex = kInvalidNativeA8CommandIndex;
			frame.renderer = nullptr;
			frame.device = nullptr;
			frame.accumulator = nullptr;
			frame.building = false;
			frame.active = false;
			frame.generationUnavailable = false;
			frame.uploadStarted = false;
			RefreshFrameMemory();
		}

		void ResetLiveBatchScratch()
		{
			CrossTextFrame& frame = s_crossTextFrame;
			frame.livePreflight.clear();
			frame.liveSnapshots.clear();
			frame.liveBatchIndex = kInvalidNativeA8CommandIndex;
		}

		enum class CompatibilityComparePhase : UInt8
		{
			Admission = 0,
			Live
		};

		bool RecordCompatibilityKeyMismatch(CompatibilityComparePhase phase,
			FreeTypePerfCounter field)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchTotal);
			RecordFreeTypePerf(phase == CompatibilityComparePhase::Admission
				? FreeTypePerfCounter::
					GlyphInstancingCompatibilityMismatchAdmission
				: FreeTypePerfCounter::
					GlyphInstancingCompatibilityMismatchLive);
			RecordFreeTypePerf(field);
			return false;
		}

		bool SameCompatibilityKey(const CrossTextCompatibilityKey& left,
			const CrossTextCompatibilityKey& right,
			CompatibilityComparePhase phase)
		{
			if (left.program != right.program)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchProgram);
			}
			if (left.normalDeclaration != right.normalDeclaration)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchDeclaration);
			}
			if (left.sourceTexture != right.sourceTexture)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchSourceTexture);
			}
			if (left.alphaTexture != right.alphaTexture)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchAlphaTexture);
			}
			if (left.atlasTexture != right.atlasTexture)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchAtlasTexture);
			}
			if (std::memcmp(left.constants.data(), right.constants.data(),
					left.constants.size() * sizeof(float)) != 0)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchConstants);
			}
			if (std::memcmp(&left.textureTransform,
					&right.textureTransform,
					sizeof(left.textureTransform)) != 0)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchTextureTransform);
			}
			if (left.clampMode != right.clampMode)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchClampMode);
			}
			if (left.shaderClass != right.shaderClass)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchShaderClass);
			}
			if (left.sampling != right.sampling)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchSampling);
			}
			if (left.quality != right.quality)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchQuality);
			}
			if (left.distanceFieldMethod != right.distanceFieldMethod)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchDistanceFieldMethod);
			}
			if (left.layer != right.layer)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchLayer);
			}
			if (left.atlasPage != right.atlasPage)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchAtlasPage);
			}
			if (left.alphaFlags != right.alphaFlags)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchAlphaFlags);
			}
			if (left.alphaTestRef != right.alphaTestRef)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchAlphaTestRef);
			}
			if (left.textureModeFlags != right.textureModeFlags)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchTextureModeFlags);
			}
			if (left.shaderFlags != right.shaderFlags)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchShaderFlags);
			}
			if (std::memcmp(&left.shaderAlpha, &right.shaderAlpha,
					sizeof(left.shaderAlpha)) != 0)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchShaderAlpha);
			}
			if (std::memcmp(&left.shaderFadeAlpha,
					&right.shaderFadeAlpha,
					sizeof(left.shaderFadeAlpha)) != 0)
			{
				return RecordCompatibilityKeyMismatch(phase,
					FreeTypePerfCounter::
						GlyphInstancingCompatibilityMismatchShaderFadeAlpha);
			}
			return true;
		}

		const NativeA8DrawCommand* ResolveSequenceCommand(
			const CrossTextSequenceItem& item,
			const NativeA8FrameStamp*& stamp)
		{
			stamp = nullptr;
			if (item.kind == NativeA8CrossTextCommandKind::SinglePacket)
			{
				NativeA8SinglePacketCommandView view;
				if (!FindNativeA8SinglePacketCommand(item.commandIndex,
						s_crossTextFrame.validationToken, view)
					|| !view.command)
				{
					return nullptr;
				}
				stamp = view.stamp;
				return &view.command->draw;
			}
			if (item.kind
				== NativeA8CrossTextCommandKind::VirtualSinglePacket)
			{
				NativeA8VirtualSinglePacketCommandView view;
				if (!FindNativeA8VirtualSinglePacketCommand(item.commandIndex,
						s_crossTextFrame.validationToken, view)
					|| !view.command || !view.command->draw)
				{
					return nullptr;
				}
				stamp = view.stamp;
				return view.command->draw;
			}
			return nullptr;
		}

		enum class BuildMemberFailure : UInt8
		{
			None = 0,
			State,
			Scissor,
			Topology
		};

		std::atomic<UInt32> s_instancingDiagnosticCount = 0;
		constexpr UInt32 kMaximumInstancingDiagnosticLines = 32;

		const char* BuildMemberFailureName(BuildMemberFailure failure)
		{
			switch (failure)
			{
			case BuildMemberFailure::None:
				return "none";
			case BuildMemberFailure::State:
				return "state";
			case BuildMemberFailure::Scissor:
				return "scissor";
			case BuildMemberFailure::Topology:
				return "topology";
			default:
				return "unknown";
			}
		}

		void LogInstancingBeginDiagnostic(const char* reason,
			UInt32 sequenceIndex, UInt32 batchIndex, UInt32 memberOffset,
			NiTriShape* leaderGeometry,
			BSShaderProperty::RenderPass* renderPass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupDrawmode)
		{
			if (!g_bEnableFreeTypeFontRenderingLog
				|| s_instancingDiagnosticCount.fetch_add(
					1, std::memory_order_relaxed)
					>= kMaximumInstancingDiagnosticLines)
			{
				return;
			}
			const CrossTextFrame& frame = s_crossTextFrame;
			const CrossTextBatch* batch = batchIndex < frame.batches.size()
				? &frame.batches[batchIndex] : nullptr;
			const CrossTextAdmissionMember* member = batch
				&& memberOffset < batch->memberCount
				&& static_cast<UInt64>(batch->firstMember) + memberOffset
					< frame.admissionMembers.size()
				? &frame.admissionMembers[
					batch->firstMember + memberOffset] : nullptr;
			const NativeTileInstancingTransientState* transient =
				frame.liveBatchIndex == batchIndex
				&& memberOffset < frame.livePreflight.size()
				? &frame.livePreflight[memberOffset].transient : nullptr;
			FreeTypeFontDebugLog(
				"tnvse_freetype_glyph_instancing_diag: phase=begin reason=%s sequence=%u batch=%u member=%u frameActive=%u frameBuilding=%u generation=%u resource=%u uploadEpoch=%u plannedInstances=%u uploadCursor=%u resourceGeneration=%u disabledGeneration=%u instanceBufferBytes=%u batchState=%u texts=%u instances=%u followersBegun=%u leader=%p plannedGeometry=%p pass=%p expectedPass=%p passGeometry=%p passEnum=%u currentPass=%u lights=%u lightArray=%p callback=%u/%u/%u liveTransientReady=%u scissor=%u rect=(%ld,%ld,%ld,%ld) stencilPresent=%u stencilEnabled=%u",
				reason ? reason : "unknown", sequenceIndex, batchIndex,
				memberOffset, frame.active ? 1u : 0u,
				frame.building ? 1u : 0u, frame.generation,
				frame.resourceSerial, frame.uploadEpoch,
				frame.plannedInstanceCount, frame.uploadCursorInstances,
				s_instancingResources.generation,
				s_instancingResources.disabledGeneration,
				s_instancingResources.instanceBufferBytes,
				batch ? static_cast<UInt32>(batch->state) : 0xFFFFFFFFu,
				batch ? batch->memberCount : 0u,
				batch ? batch->instanceCount : 0u,
				batch ? batch->followersBegun : 0u,
				leaderGeometry,
				member ? member->sequence.geometry : nullptr,
				renderPass,
				frame.accumulator ? &frame.accumulator->kTileRenderPass : nullptr,
				renderPass ? renderPass->pGeometry : nullptr,
				renderPass ? renderPass->usPassEnum : 0u, currentPass,
				renderPass ? renderPass->ucNumLights : 0u,
				renderPass ? renderPass->ppSceneLights : nullptr,
				testAlpha ? 1u : 0u, blendAlpha ? 1u : 0u,
				setupDrawmode ? 1u : 0u,
				transient ? 1u : 0u,
				transient && transient->scissorEnabled ? 1u : 0u,
				transient ? transient->scissorRect.left : 0,
				transient ? transient->scissorRect.top : 0,
				transient ? transient->scissorRect.right : 0,
				transient ? transient->scissorRect.bottom : 0,
				transient && transient->stencilPresent ? 1u : 0u,
				transient && transient->stencilEnabled ? 1u : 0u);
		}

		void DisableInstancingGeneration(UInt32 generation,
			bool deviceFailure = true)
		{
			if (!generation
				|| s_instancingResources.disabledGeneration == generation)
			{
				return;
			}
			s_instancingResources.disabledGeneration = generation;
			if (deviceFailure)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingDeviceFailure);
			}
		}

		struct ResolvedCrossTextMember
		{
			const NativeA8FrameStamp* stamp = nullptr;
			const NativeA8DrawCommand* command = nullptr;
			const NativeA8PayloadTemplate* artifact = nullptr;
			const NativeA8PacketTemplate* packet = nullptr;
			const NativeA8GlyphInstanceSidecar* sidecars = nullptr;
			UInt32 sidecarCount = 0;
			const InstancingTilePropertyView* tile = nullptr;
			const NiAlphaProperty* alpha = nullptr;
		};

		BuildMemberFailure ResolveCrossTextCommand(
			const CrossTextSequenceItem& item,
			ResolvedCrossTextMember& resolved)
		{
			resolved = {};
			if (item.kind == NativeA8CrossTextCommandKind::Barrier
				|| !item.geometry || !item.metadata || !item.payload
				|| item.commandIndex == kInvalidNativeA8CommandIndex
				|| !item.payload->buildComplete
				|| !item.payload->payloadTemplate)
			{
				return BuildMemberFailure::Topology;
			}

			resolved.command = ResolveSequenceCommand(item, resolved.stamp);
			const NativeA8DrawCommand* command = resolved.command;
			const NativeA8FrameStamp* stamp = resolved.stamp;
			if (!command || !stamp || !stamp->accumulator || !stamp->renderer
				|| !stamp->device || !stamp->generation || !stamp->resourceSerial
				|| command->sourceGeometry != item.geometry
				|| command->expectedGeometry != item.geometry
				|| command->payload != item.payload
				|| !command->packet || command->packetIndex != 0
				|| !command->program || !command->program->active
				|| !command->program->directDrawLiteReady
				|| command->program->standardV2SlotProofs
					!= NativeA8CompiledPacketCommand::kStandardV2RequiredProofs
				|| !command->standardPassLite
				|| !command->standardPassLite->ready
				|| !command->standardPassLite->standardV2Ready
				|| !IsNativeA8StandardPassLiteDispatchCurrent(
					*command->standardPassLite, item.geometry,
					command->program, stamp->generation)
				|| !command->binding.active
				|| !IsNativeA8FramePacketBindingCurrent(command->binding))
			{
				return BuildMemberFailure::State;
			}
			return BuildMemberFailure::None;
		}

		BuildMemberFailure AdmitCrossTextFrameStamp(
			const NativeA8FrameStamp& stamp)
		{
			CrossTextFrame& frame = s_crossTextFrame;
			if (s_instancingResources.disabledGeneration == stamp.generation)
			{
				frame.generationUnavailable = true;
				return BuildMemberFailure::State;
			}
			if (!frame.generation)
			{
				NativeA8InstancingShaderResources shaderResources;
				if (!GetNativeA8InstancingShaderResources(
						stamp.generation, shaderResources)
					|| shaderResources.device != stamp.device)
				{
					// Capability or shader deployment failures are stable for this
					// generation. Avoid repeating the complete member proof every
					// frame, but do not classify an unsupported device as a runtime
					// D3D failure.
					DisableInstancingGeneration(stamp.generation, false);
					frame.generationUnavailable = true;
					return BuildMemberFailure::State;
				}
				frame.generation = stamp.generation;
				frame.atlasTextureEpoch = stamp.atlasTextureEpoch;
				frame.resourceSerial = stamp.resourceSerial;
				frame.uploadEpoch = stamp.uploadEpoch;
				frame.renderer = stamp.renderer;
				frame.device = stamp.device;
				frame.accumulator = stamp.accumulator;
				return BuildMemberFailure::None;
			}
			return frame.generation == stamp.generation
				&& frame.atlasTextureEpoch == stamp.atlasTextureEpoch
				&& frame.resourceSerial == stamp.resourceSerial
				&& frame.uploadEpoch == stamp.uploadEpoch
				&& frame.renderer == stamp.renderer
				&& frame.device == stamp.device
				&& frame.accumulator == stamp.accumulator
				? BuildMemberFailure::None : BuildMemberFailure::State;
		}

		bool IsCrossTextFrameStampLive(const NativeA8FrameStamp& stamp)
		{
			const CrossTextFrame& frame = s_crossTextFrame;
			return s_instancingResources.disabledGeneration != stamp.generation
				&& frame.generation == stamp.generation
				&& frame.atlasTextureEpoch == stamp.atlasTextureEpoch
				&& frame.resourceSerial == stamp.resourceSerial
				&& frame.uploadEpoch == stamp.uploadEpoch
				&& frame.renderer == stamp.renderer
				&& frame.device == stamp.device
				&& frame.accumulator == stamp.accumulator;
		}

		BuildMemberFailure ResolveCrossTextPayload(
			const CrossTextSequenceItem& item,
			ResolvedCrossTextMember& resolved)
		{
			NiTriShapeData* geometryData = item.geometry->GetModelData();
			if (!IsA8AtlasShape(item.geometry)
				|| item.geometry->GetSkinInstance()
				|| item.geometry->GetControllers()
				|| !geometryData
				|| geometryData->m_spAdditionalGeomData.m_pObject
				|| (geometryData->m_usDirtyFlags
					& NiGeometryData::CONSISTENCY_MASK)
					!= NiGeometryData::STATIC
				|| !geometryData->GetActiveVertexCount())
			{
				return BuildMemberFailure::Topology;
			}

			resolved.artifact = item.payload->payloadTemplate.get();
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(*resolved.artifact,
					item.payload->useCompositePackets);
			if (packets.size() != 1
				|| resolved.command->packet != &packets[0]
				|| item.payload->packetShaders.size() != 1
				|| item.payload->packetShaders[0]
					!= resolved.command->program->shader
				|| item.payload->packetPrograms.size() != 1
				|| item.payload->packetPrograms[0] != resolved.command->program
				|| !packets[0].instanceTopologyValid
				|| !packets[0].instanceSidecarCount
				|| packets[0].atlasPage
					>= resolved.artifact->atlasProperties.size()
				|| packets[0].atlasPage
					>= resolved.artifact->atlasTextures.size()
				|| !resolved.artifact->atlasProperties[packets[0].atlasPage]
				|| !resolved.artifact->atlasTextures[packets[0].atlasPage]
				|| static_cast<UInt64>(packets[0].instanceSidecarFirst)
					+ packets[0].instanceSidecarCount
					> resolved.artifact->glyphInstanceSidecars.size())
			{
				return BuildMemberFailure::Topology;
			}

			NiShadeProperty* shade = item.geometry->GetShadeProperty();
			resolved.tile = shade
				&& shade->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<const InstancingTilePropertyView*>(shade)
				: nullptr;
			resolved.alpha = item.geometry->GetAlphaProperty();
			if (!resolved.tile || !resolved.alpha
				|| resolved.tile->alphaTexture.m_pObject
				|| resolved.tile->noTexture)
			{
				return BuildMemberFailure::State;
			}
			if (item.kind
					== NativeA8CrossTextCommandKind::VirtualSinglePacket
				&& (!IsNativeA8VirtualStockPacketAtlasCurrent(
						item.geometry, *item.payload, 0)
					|| item.geometry->GetShader()
						!= resolved.command->program->shader))
			{
				// Unlike an ordinary facade, the Virtual-stock singleton route does
				// not replace its atlas property, source texture, or shader around
				// TileShader. Re-prove those persistent bindings before allowing a
				// leader or follower to inherit the shared instanced state.
				return BuildMemberFailure::State;
			}

			resolved.packet = &packets[0];
			resolved.sidecars = resolved.artifact->glyphInstanceSidecars.data()
				+ packets[0].instanceSidecarFirst;
			resolved.sidecarCount = packets[0].instanceSidecarCount;
			return BuildMemberFailure::None;
		}

		void BuildCompatibilityKey(const ResolvedCrossTextMember& resolved,
			CrossTextCompatibilityKey& key)
		{
			key = {};
			key.program = resolved.command->program;
			key.normalDeclaration = resolved.command->binding.declaration;
			key.sourceTexture = resolved.artifact->atlasTextures[
				resolved.packet->atlasPage].m_pObject;
			key.alphaTexture = resolved.tile->alphaTexture.m_pObject;
			key.atlasTexture = resolved.command->atlasTexture;
			key.constants = resolved.packet->constants;
			// Retail and the symbolized test build map VS c4 to TileShader::
			// TexScroll. It is refreshed only for rotating Tile properties. Exact
			// texture-transform plus rotates identity therefore proves c4 is the
			// same for every member; the instanced VS never mutates that register.
			key.textureTransform = resolved.tile->textureTransform;
			key.clampMode = resolved.tile->rotates
				? NiTexturingProperty::WRAP_S_WRAP_T
				: resolved.tile->clampMode;
			key.shaderClass = resolved.packet->shaderClass;
			key.sampling = resolved.packet->sampling;
			key.quality = resolved.packet->quality;
			key.distanceFieldMethod = resolved.packet->distanceFieldMethod;
			key.layer = resolved.packet->layer;
			key.atlasPage = resolved.packet->atlasPage;
			key.alphaFlags = resolved.alpha->m_usFlags.Get()
				& ~NiAlphaProperty::TEST_ENABLE_MASK;
			key.alphaTestRef = resolved.alpha->m_ucAlphaTestRef;
			key.textureModeFlags =
				(resolved.tile->rotates ? 1u : 0u)
				| (resolved.tile->hasVertexColors ? 2u : 0u);
			key.shaderFlags = {
				resolved.tile->ulFlags[0], resolved.tile->ulFlags[1] };
			key.shaderAlpha = resolved.tile->fAlpha;
			key.shaderFadeAlpha = resolved.tile->fFadeAlpha;
		}

		BuildMemberFailure BuildAdmissionMember(
			const CrossTextSequenceItem& item, UInt32 sequenceIndex,
			CrossTextAdmissionCandidate& candidate)
		{
			ResolvedCrossTextMember resolved;
			BuildMemberFailure failure = ResolveCrossTextCommand(item, resolved);
			if (failure != BuildMemberFailure::None)
				return failure;
			failure = AdmitCrossTextFrameStamp(*resolved.stamp);
			if (failure != BuildMemberFailure::None)
				return failure;
			failure = ResolveCrossTextPayload(item, resolved);
			if (failure != BuildMemberFailure::None)
				return failure;

			const NativeTileInstancingSnapshotResult transientResult =
				BuildNativeTileInstancingTransientState(
					item.geometry, &item.geometry->m_kProperties,
					candidate.transient);
			if (transientResult
				== NativeTileInstancingSnapshotResult::ScaledScissor)
			{
				return BuildMemberFailure::Scissor;
			}
			if (transientResult != NativeTileInstancingSnapshotResult::Ready)
				return BuildMemberFailure::State;

			candidate.member.sequenceIndex = sequenceIndex;
			candidate.member.sequence = item;
			candidate.member.command = resolved.command;
			candidate.member.packet = resolved.packet;
			candidate.member.sidecars = resolved.sidecars;
			candidate.member.sidecarCount = resolved.sidecarCount;
			BuildCompatibilityKey(resolved, candidate.compatibility);
			return BuildMemberFailure::None;
		}

		BuildMemberFailure BuildLivePreflightMember(
			const CrossTextAdmissionMember& planned,
			CrossTextLivePreflight& live)
		{
			const CrossTextSequenceItem& item = planned.sequence;
			ResolvedCrossTextMember resolved;
			BuildMemberFailure failure = ResolveCrossTextCommand(item, resolved);
			if (failure != BuildMemberFailure::None)
				return failure;
			if (!IsCrossTextFrameStampLive(*resolved.stamp))
				return BuildMemberFailure::State;
			failure = ResolveCrossTextPayload(item, resolved);
			if (failure != BuildMemberFailure::None)
				return failure;
			if (resolved.command != planned.command
				|| resolved.packet != planned.packet
				|| resolved.sidecars != planned.sidecars
				|| resolved.sidecarCount != planned.sidecarCount)
			{
				return BuildMemberFailure::Topology;
			}

			const NativeTileInstancingSnapshotResult transientResult =
				BuildNativeTileInstancingTransientState(
					item.geometry, &item.geometry->m_kProperties,
					live.transient);
			if (transientResult
				== NativeTileInstancingSnapshotResult::ScaledScissor)
			{
				return BuildMemberFailure::Scissor;
			}
			if (transientResult != NativeTileInstancingSnapshotResult::Ready)
				return BuildMemberFailure::State;

			// Both ordinary direct-shape and Virtual-stock singleton submission
			// temporarily apply the payload origin before reaching TileShader. This
			// pass uses the effective world only for conservative visibility.
			NiTransform effectiveWorld;
			ApplyNativeA8GeometryOrigin(effectiveWorld,
				item.geometry->m_kWorld, item.payload->geometryOrigin);
			// Followers do not enter their own late-visibility scope after a
			// successful batch. Prove every member before constructing any complete
			// live matrix/color snapshot for the batch.
			if (IsNativeA8PayloadOutsideScissorForWorld(
					*item.payload, &item.geometry->m_kProperties,
					s_crossTextFrame.renderer, effectiveWorld))
			{
				return BuildMemberFailure::Scissor;
			}
			BuildCompatibilityKey(resolved, live.compatibility);
			return BuildMemberFailure::None;
		}

		BuildMemberFailure BuildCompleteLiveMemberSnapshot(
			const CrossTextAdmissionMember& planned,
			const CrossTextLivePreflight& preflight,
			NativeTileInstancingSnapshot& snapshot)
		{
			CrossTextFrame& frame = s_crossTextFrame;
			NiTriShape* geometry = planned.sequence.geometry;
			NativeA8ShapePayload* payload = planned.sequence.payload;
			if (!geometry || !payload)
				return BuildMemberFailure::Topology;
			if (!frame.renderer)
				return BuildMemberFailure::State;

			NiTransform effectiveWorld;
			ApplyNativeA8GeometryOrigin(effectiveWorld,
				geometry->m_kWorld, payload->geometryOrigin);
			const NativeTileInstancingSnapshotResult snapshotResult =
				BuildNativeTileInstancingSnapshotForWorld(geometry,
					&geometry->m_kProperties, frame.renderer,
					effectiveWorld, snapshot);
			if (snapshotResult
				== NativeTileInstancingSnapshotResult::ScaledScissor)
			{
				return BuildMemberFailure::Scissor;
			}
			if (snapshotResult != NativeTileInstancingSnapshotResult::Ready)
				return BuildMemberFailure::State;

			// No callback or device submission occurs between the two live passes.
			// Recheck the complete builder's slot-31 result against pass 1 so upload
			// and restoration cannot observe a different transient state.
			if (!SameNativeTileInstancingTransientState(
					preflight.transient, snapshot.transient))
			{
				return BuildMemberFailure::Scissor;
			}
			return BuildMemberFailure::None;
		}

		void RecordBuildFailure(BuildMemberFailure failure)
		{
			switch (failure)
			{
			case BuildMemberFailure::Scissor:
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingScissorFallback);
				break;
			case BuildMemberFailure::Topology:
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingTopologyFallback);
				break;
			case BuildMemberFailure::State:
			default:
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingStateFallback);
				break;
			}
		}

		void ReleaseResources(InstancingResources& resources)
		{
			ReleaseD3D(resources.instanceBuffer);
			ReleaseD3D(resources.indexBuffer);
			ReleaseD3D(resources.cornerBuffer);
			resources.device = nullptr;
			resources.instanceBufferBytes = 0;
			resources.generation = 0;
			resources.stateProofGeneration = 0;
		}

		bool CreateImmutableResources(InstancingResources& resources,
			IDirect3DDevice9* device, UInt32 generation)
		{
			if (!device || !generation)
				return false;
			if (resources.device == device
				&& resources.generation == generation
				&& resources.cornerBuffer && resources.indexBuffer)
			{
				return true;
			}
			ReleaseResources(resources);
			resources.device = device;
			resources.generation = generation;
			auto fail = [&resources]()
			{
				ReleaseResources(resources);
				return false;
			};

			HRESULT result = device->CreateVertexBuffer(
				4u * 4u * sizeof(float), D3DUSAGE_WRITEONLY, 0,
				D3DPOOL_DEFAULT, &resources.cornerBuffer, nullptr);
			if (FAILED(result) || !resources.cornerBuffer)
				return fail();
			void* cornerData = nullptr;
			result = resources.cornerBuffer->Lock(
				0, 0, &cornerData, 0);
			if (FAILED(result) || !cornerData)
				return fail();
			const float corners[16] = {
				1.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 1.0f, 1.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 1.0f,
				1.0f, 0.0f, 0.0f, 1.0f
			};
			std::memcpy(cornerData, corners, sizeof(corners));
			result = resources.cornerBuffer->Unlock();
			if (FAILED(result))
				return fail();

			result = device->CreateIndexBuffer(
				6u * sizeof(UInt16), D3DUSAGE_WRITEONLY,
				D3DFMT_INDEX16, D3DPOOL_DEFAULT,
				&resources.indexBuffer, nullptr);
			if (FAILED(result) || !resources.indexBuffer)
				return fail();
			void* indexData = nullptr;
			result = resources.indexBuffer->Lock(0, 0, &indexData, 0);
			if (FAILED(result) || !indexData)
				return fail();
			const UInt16 indices[6] = { 0, 2, 1, 0, 3, 2 };
			std::memcpy(indexData, indices, sizeof(indices));
			result = resources.indexBuffer->Unlock();
			return SUCCEEDED(result) ? true : fail();
		}

		UInt32 RoundInstanceBufferBytes(UInt32 requiredBytes)
		{
			UInt32 bytes = 64u * 1024u;
			while (bytes < requiredBytes
				&& bytes < kNativeA8MaximumInstanceBufferBytes)
			{
				bytes = std::min(bytes * 2u,
					kNativeA8MaximumInstanceBufferBytes);
			}
			return bytes >= requiredBytes ? bytes : 0;
		}

		bool EnsureInstanceBuffer(InstancingResources& resources,
			UInt32 requiredBytes)
		{
			if (resources.instanceBuffer
				&& resources.instanceBufferBytes >= requiredBytes)
			{
				return true;
			}
			const UInt32 targetBytes = RoundInstanceBufferBytes(requiredBytes);
			if (!targetBytes)
				return false;
			IDirect3DVertexBuffer9* replacement = nullptr;
			const HRESULT result = resources.device->CreateVertexBuffer(
				targetBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
				D3DPOOL_DEFAULT, &replacement, nullptr);
			if (FAILED(result) || !replacement)
			{
				if (replacement)
					replacement->Release();
				return false;
			}
			ReleaseD3D(resources.instanceBuffer);
			resources.instanceBuffer = replacement;
			resources.instanceBufferBytes = targetBytes;
			resources.stateProofGeneration = 0;
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingBufferGrowth);
			return true;
		}

		bool PrepareFrameInstanceResources()
		{
			CrossTextFrame& frame = s_crossTextFrame;
			if (frame.batches.empty() || frame.admissionMembers.empty()
				|| !frame.renderer || !frame.device || !frame.generation)
			{
				return false;
			}
			NativeA8InstancingShaderResources shaderResources;
			if (!GetNativeA8InstancingShaderResources(
					frame.generation, shaderResources)
				|| shaderResources.device != frame.device
				|| s_instancingResources.disabledGeneration
					== frame.generation
				|| !CreateImmutableResources(s_instancingResources,
					frame.device, frame.generation))
			{
				return false;
			}

			UInt64 totalInstances = 0;
			for (CrossTextBatch& batch : frame.batches)
			{
				batch.baseInstance = 0;
				totalInstances += batch.instanceCount;
			}
			const UInt64 totalBytes64 = totalInstances
				* sizeof(NativeA8GlyphInstance);
			if (!totalInstances
				|| totalBytes64 > kNativeA8MaximumInstanceBufferBytes
				|| !EnsureInstanceBuffer(s_instancingResources,
					static_cast<UInt32>(totalBytes64)))
			{
				return false;
			}

			frame.plannedInstanceCount = static_cast<UInt32>(totalInstances);
			frame.uploadCursorInstances = 0;
			frame.uploadStarted = false;
			return true;
		}

		bool UploadBatchInstances(UInt32 batchIndex, CrossTextBatch& batch)
		{
			FreeTypePerfScope uploadPerf(
				FreeTypePerfPhase::GlyphInstancingUpload);
			CrossTextFrame& frame = s_crossTextFrame;
			if (!frame.active || !frame.device || !frame.generation
				|| !batch.instanceCount || !batch.memberCount
				|| batch.firstMember >= frame.admissionMembers.size()
				|| static_cast<UInt64>(batch.firstMember) + batch.memberCount
					> frame.admissionMembers.size()
				|| frame.liveBatchIndex != batchIndex
				|| frame.liveSnapshots.size() != batch.memberCount
				|| frame.uploadCursorInstances > frame.plannedInstanceCount
				|| batch.instanceCount
					> frame.plannedInstanceCount - frame.uploadCursorInstances
				|| !s_instancingResources.instanceBuffer)
			{
				return false;
			}

			const UInt32 baseInstance = frame.uploadCursorInstances;
			const UInt32 byteOffset = baseInstance
				* kNativeA8GlyphInstanceBytes;
			const UInt32 byteCount = batch.instanceCount
				* kNativeA8GlyphInstanceBytes;
			if (static_cast<UInt64>(byteOffset) + byteCount
				> s_instancingResources.instanceBufferBytes)
			{
				return false;
			}

			const DWORD lockFlags = frame.uploadStarted
				? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
			void* mapped = nullptr;
			HRESULT result = s_instancingResources.instanceBuffer->Lock(
				byteOffset, byteCount, &mapped, lockFlags);
			if (FAILED(result) || !mapped)
			{
				DisableInstancingGeneration(frame.generation);
				return false;
			}
			if (!frame.uploadStarted)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingDiscard);
			}

			UInt8* output = static_cast<UInt8*>(mapped);
			for (UInt32 memberOffset = 0;
				memberOffset < batch.memberCount; ++memberOffset)
			{
				const CrossTextAdmissionMember& member = frame.admissionMembers[
					batch.firstMember + memberOffset];
				const NativeTileInstancingSnapshot& snapshot =
					frame.liveSnapshots[memberOffset];
				for (UInt32 glyph = 0; glyph < member.sidecarCount; ++glyph)
				{
					NativeA8GlyphInstance instance;
					instance.sidecar = member.sidecars[glyph];
					instance.wvpColumns = snapshot.wvpColumns;
					instance.tileColor = snapshot.tileColor;
					std::memcpy(output, &instance, sizeof(instance));
					output += sizeof(instance);
				}
			}
			const bool exactUpload = output
				== static_cast<UInt8*>(mapped) + byteCount;
			result = s_instancingResources.instanceBuffer->Unlock();
			if (!exactUpload || FAILED(result))
			{
				DisableInstancingGeneration(frame.generation);
				return false;
			}

			batch.baseInstance = baseInstance;
			frame.uploadCursorInstances += batch.instanceCount;
			frame.uploadStarted = true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingUploadByte,
				byteCount);
			return true;
		}

		bool BeginFollowerCommand(const CrossTextAdmissionMember& member)
		{
			if (member.sequence.kind
				== NativeA8CrossTextCommandKind::SinglePacket)
			{
				NativeA8SinglePacketCommandView view;
				return BeginNativeA8SinglePacketCommandExecution(
					member.sequence.commandIndex,
					member.sequence.geometry, view);
			}
			if (member.sequence.kind
				== NativeA8CrossTextCommandKind::VirtualSinglePacket)
			{
				NativeA8VirtualSinglePacketCommandView view;
				return BeginNativeA8VirtualSinglePacketCommandExecution(
					member.sequence.commandIndex,
					member.sequence.metadata,
					member.sequence.geometry, view);
			}
			return false;
		}

		void EndFollowerCommand(const CrossTextAdmissionMember& member,
			bool success)
		{
			if (member.sequence.kind
				== NativeA8CrossTextCommandKind::SinglePacket)
			{
				if (success)
				{
					EndNativeA8SinglePacketCommandExecution(
						member.sequence.commandIndex, true, true);
				}
				else
				{
					AbandonNativeA8SinglePacketCommandExecution(
						member.sequence.commandIndex);
				}
			}
			else if (member.sequence.kind
				== NativeA8CrossTextCommandKind::VirtualSinglePacket)
			{
				if (success)
				{
					EndNativeA8VirtualSinglePacketCommandExecution(
						member.sequence.commandIndex, true, true);
				}
				else
				{
					AbandonNativeA8VirtualSinglePacketCommandExecution(
						member.sequence.commandIndex);
				}
			}
		}

		bool IsLeaderCommandConsumed(const CrossTextAdmissionMember& member)
		{
			if (member.sequence.kind
				== NativeA8CrossTextCommandKind::SinglePacket)
			{
				return IsNativeA8SinglePacketCommandConsumed(
					member.sequence.commandIndex,
					s_crossTextFrame.validationToken);
			}
			if (member.sequence.kind
				== NativeA8CrossTextCommandKind::VirtualSinglePacket)
			{
				return IsNativeA8VirtualSinglePacketCommandConsumed(
					member.sequence.commandIndex,
					s_crossTextFrame.validationToken);
			}
			return false;
		}

		bool IsRestorableMember(const CrossTextAdmissionMember* member,
			IDirect3DDevice9* device)
		{
			return member && member->sequence.geometry && member->command
				&& member->command->program
				&& member->command->program->active
				&& member->command->program->device == device
				&& member->command->program->vertexShader
				&& member->command->program->pixelShader
				&& member->command->binding.active
				&& member->command->binding.declaration
				&& member->command->binding.vertexBuffer
				&& member->command->binding.indexBuffer;
		}

		// Instancing is a bracketed device-only mutation. Do not publish the
		// temporary VS/declaration through NiD3DRenderState: the symbolized Xenon
		// PrepareGeometryForRendering and retail PC E812F0 both cache declaration
		// identity in software and assume it describes the live device. Publishing
		// an instanced declaration and merely clearing that cache on exit leaves a
		// window where the 152-byte instance stream can be interpreted as ordinary
		// per-vertex input. Restore the complete normal draw state on every exit.
		class InstancingDeviceStateGuard
		{
		public:
			InstancingDeviceStateGuard(IDirect3DDevice9* device,
				NiDX9Renderer* renderer,
				NiD3DRenderState* renderState,
				UInt32 generation,
				const CrossTextAdmissionMember* leader,
				const NativeTileInstancingSnapshot* leaderSnapshot,
				const CrossTextAdmissionMember* last,
				const NativeTileInstancingSnapshot* lastSnapshot)
				: m_device(device), m_renderer(renderer),
				  m_renderState(renderState), m_generation(generation),
				  m_leader(leader), m_leaderSnapshot(leaderSnapshot),
				  m_last(last), m_lastSnapshot(lastSnapshot)
			{
			}

			~InstancingDeviceStateGuard()
			{
				Restore();
			}

			void MarkDrawSucceeded()
			{
				m_drawSucceeded = true;
			}

			bool Restore()
			{
				if (m_restored)
					return m_restoreSucceeded;
				FreeTypePerfScope restorePerf(
					FreeTypePerfPhase::GlyphInstancingRestore);
				m_restored = true;
				if (!m_device)
					return false;

				const CrossTextAdmissionMember* target = m_drawSucceeded
					? m_last : m_leader;
				const NativeTileInstancingSnapshot* targetSnapshot =
					m_drawSucceeded ? m_lastSnapshot : m_leaderSnapshot;
				const bool targetReady = targetSnapshot
					&& IsRestorableMember(target, m_device);
				const NativeA8CompiledPacketCommand* program = targetReady
					? target->command->program : nullptr;
				const NativeA8FramePacketBinding* binding = targetReady
					? &target->command->binding : nullptr;
				const HRESULT reset0 =
					m_device->SetStreamSourceFreq(0, 1);
				const HRESULT reset1 =
					m_device->SetStreamSourceFreq(1, 1);
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingStreamFrequencyReset,
					2);
				const HRESULT unbind =
					m_device->SetStreamSource(1, nullptr, 0, 0);
				HRESULT declaration = D3DERR_INVALIDCALL;
				HRESULT stream0 = D3DERR_INVALIDCALL;
				HRESULT indices = D3DERR_INVALIDCALL;
				HRESULT vertexShader = D3DERR_INVALIDCALL;
				HRESULT pixelShader = D3DERR_INVALIDCALL;
				HRESULT vertexConstants = D3DERR_INVALIDCALL;
				HRESULT pixelConstant = D3DERR_INVALIDCALL;
				if (targetReady)
				{
					declaration = m_device->SetVertexDeclaration(
						binding->declaration);
					stream0 = m_device->SetStreamSource(0,
						binding->vertexBuffer, 0,
						static_cast<UINT>(sizeof(NativeA8GpuVertex)));
					indices = m_device->SetIndices(binding->indexBuffer);
					vertexShader = m_device->SetVertexShader(
						program->vertexShader);
					pixelShader = m_device->SetPixelShader(
						program->pixelShader);
					vertexConstants = m_device->SetVertexShaderConstantF(
						0, &targetSnapshot->wvpColumns._11, 4);
					// c4 is TileShader::TexScroll, not part of WVP. The live preflight
					// proves identical textureTransform/rotates inputs and the temporary
					// VS does not write c4, so retaining the leader value is the exact
					// state the logical last member would have published.
					pixelConstant = m_device->SetPixelShaderConstantF(
						0, targetSnapshot->tileColor.data(), 1);
				}

				const bool bindingFailure = FAILED(reset0) || FAILED(reset1)
					|| FAILED(unbind) || FAILED(declaration)
					|| FAILED(stream0) || FAILED(indices)
					|| FAILED(vertexShader) || FAILED(pixelShader);
				const bool constantFailure = FAILED(vertexConstants)
					|| FAILED(pixelConstant);
				if (bindingFailure)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingBindingFailure);
				}
				if (constantFailure)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingConstantFailure);
				}
				if (bindingFailure || constantFailure)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingRestoreFailure);
					DisableInstancingGeneration(m_generation);
					return false;
				}

				// The temporary device mutation was deliberately invisible to the
				// Gamebryo shader cache. Reaffirm its normal-program mirror and make
				// renderer property/world identity match the last logically drawn text
				// on success (or the leader before fail-open replay).
				if (m_renderState)
				{
					m_renderState->m_hCurrentVertexShader =
						program->vertexShader;
					m_renderState->m_hCurrentPixelShader =
						program->pixelShader;
				}
				if (m_renderer && m_renderer->GetD3DDevice() == m_device)
				{
					m_renderer->m_pkCurrProp =
						&target->sequence.geometry->m_kProperties;
					m_renderer->m_pkCurrEffects = nullptr;
					std::memcpy(&m_renderer->m_kD3DMat,
						&targetSnapshot->retailWorld,
						sizeof(targetSnapshot->retailWorld));
				}
				m_restoreSucceeded = true;
				return true;
			}

		private:
			IDirect3DDevice9* m_device = nullptr;
			NiDX9Renderer* m_renderer = nullptr;
			NiD3DRenderState* m_renderState = nullptr;
			UInt32 m_generation = 0;
			const CrossTextAdmissionMember* m_leader = nullptr;
			const NativeTileInstancingSnapshot* m_leaderSnapshot = nullptr;
			const CrossTextAdmissionMember* m_last = nullptr;
			const NativeTileInstancingSnapshot* m_lastSnapshot = nullptr;
			bool m_drawSucceeded = false;
			bool m_restored = false;
			bool m_restoreSucceeded = false;
		};

		bool ProveInstancingDeviceState(IDirect3DDevice9* device,
			const NativeA8InstancingShaderResources& shaders,
			const NativeA8CompiledPacketCommand& normalProgram,
			UInt32 expectedFrequency0, UInt32 instanceOffset)
		{
			UINT frequency0 = 0;
			UINT frequency1 = 0;
			IDirect3DVertexBuffer9* stream0Buffer = nullptr;
			IDirect3DVertexBuffer9* stream1Buffer = nullptr;
			UINT stream0Offset = 0;
			UINT stream1Offset = 0;
			UINT stream0Stride = 0;
			UINT stream1Stride = 0;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			IDirect3DVertexShader9* vertexShader = nullptr;
			IDirect3DPixelShader9* pixelShader = nullptr;

			const HRESULT frequency0Result = device->GetStreamSourceFreq(
				0, &frequency0);
			const HRESULT frequency1Result = device->GetStreamSourceFreq(
				1, &frequency1);
			const HRESULT stream0Result = device->GetStreamSource(0,
				&stream0Buffer, &stream0Offset, &stream0Stride);
			const HRESULT stream1Result = device->GetStreamSource(1,
				&stream1Buffer, &stream1Offset, &stream1Stride);
			const HRESULT indexResult = device->GetIndices(&indexBuffer);
			const HRESULT declarationResult =
				device->GetVertexDeclaration(&declaration);
			const HRESULT vertexShaderResult =
				device->GetVertexShader(&vertexShader);
			const HRESULT pixelShaderResult =
				device->GetPixelShader(&pixelShader);

			const bool ready = SUCCEEDED(frequency0Result)
				&& SUCCEEDED(frequency1Result)
				&& SUCCEEDED(stream0Result) && SUCCEEDED(stream1Result)
				&& SUCCEEDED(indexResult) && SUCCEEDED(declarationResult)
				&& SUCCEEDED(vertexShaderResult)
				&& SUCCEEDED(pixelShaderResult)
				&& frequency0 == expectedFrequency0
				&& frequency1 == (D3DSTREAMSOURCE_INSTANCEDATA | 1u)
				&& stream0Buffer == s_instancingResources.cornerBuffer
				&& stream0Offset == 0 && stream0Stride == 4u * sizeof(float)
				&& stream1Buffer == s_instancingResources.instanceBuffer
				&& stream1Offset == instanceOffset
				&& stream1Stride == kNativeA8GlyphInstanceBytes
				&& indexBuffer == s_instancingResources.indexBuffer
				&& declaration == shaders.declaration
				&& vertexShader == shaders.vertexShader
				&& pixelShader == normalProgram.pixelShader;

			ReleaseD3D(stream0Buffer);
			ReleaseD3D(stream1Buffer);
			ReleaseD3D(indexBuffer);
			ReleaseD3D(declaration);
			ReleaseD3D(vertexShader);
			ReleaseD3D(pixelShader);
			return ready;
		}

		bool ProveNormalDeviceState(IDirect3DDevice9* device,
			const CrossTextAdmissionMember& member,
			const NativeTileInstancingSnapshot& snapshot)
		{
			if (!IsRestorableMember(&member, device))
				return false;
			UINT frequency0 = 0;
			UINT frequency1 = 0;
			IDirect3DVertexBuffer9* stream0Buffer = nullptr;
			IDirect3DVertexBuffer9* stream1Buffer = nullptr;
			UINT stream0Offset = 0;
			UINT stream1Offset = 0;
			UINT stream0Stride = 0;
			UINT stream1Stride = 0;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			IDirect3DVertexShader9* vertexShader = nullptr;
			IDirect3DPixelShader9* pixelShader = nullptr;
			D3DXMATRIX vertexConstants = {};
			std::array<float, 4> pixelConstant = {};

			const HRESULT frequency0Result = device->GetStreamSourceFreq(
				0, &frequency0);
			const HRESULT frequency1Result = device->GetStreamSourceFreq(
				1, &frequency1);
			const HRESULT stream0Result = device->GetStreamSource(0,
				&stream0Buffer, &stream0Offset, &stream0Stride);
			const HRESULT stream1Result = device->GetStreamSource(1,
				&stream1Buffer, &stream1Offset, &stream1Stride);
			const HRESULT indexResult = device->GetIndices(&indexBuffer);
			const HRESULT declarationResult =
				device->GetVertexDeclaration(&declaration);
			const HRESULT vertexShaderResult =
				device->GetVertexShader(&vertexShader);
			const HRESULT pixelShaderResult =
				device->GetPixelShader(&pixelShader);
			const HRESULT vertexConstantResult =
				device->GetVertexShaderConstantF(
					0, &vertexConstants._11, 4);
			const HRESULT pixelConstantResult =
				device->GetPixelShaderConstantF(
					0, pixelConstant.data(), 1);

			const NativeA8FramePacketBinding& binding =
				member.command->binding;
			const NativeA8CompiledPacketCommand& program =
				*member.command->program;
			const bool ready = SUCCEEDED(frequency0Result)
				&& SUCCEEDED(frequency1Result)
				&& SUCCEEDED(stream0Result) && SUCCEEDED(stream1Result)
				&& SUCCEEDED(indexResult) && SUCCEEDED(declarationResult)
				&& SUCCEEDED(vertexShaderResult)
				&& SUCCEEDED(pixelShaderResult)
				&& SUCCEEDED(vertexConstantResult)
				&& SUCCEEDED(pixelConstantResult)
				&& frequency0 == 1u && frequency1 == 1u
				&& stream0Buffer == binding.vertexBuffer
				&& stream0Offset == 0
				&& stream0Stride == sizeof(NativeA8GpuVertex)
				// Once stream 1 is unbound, its offset and stride are inert.  Some
				// D3D9 implementations retain the last stride in GetStreamSource
				// even though SetStreamSource accepted a null buffer.  Prove the
				// semantically relevant state without rejecting that legal residue.
				&& !stream1Buffer
				&& indexBuffer == binding.indexBuffer
				&& declaration == binding.declaration
				&& vertexShader == program.vertexShader
				&& pixelShader == program.pixelShader
				&& std::memcmp(&vertexConstants,
					&snapshot.wvpColumns,
					sizeof(vertexConstants)) == 0
				&& std::memcmp(pixelConstant.data(),
					snapshot.tileColor.data(),
					pixelConstant.size() * sizeof(float)) == 0;

			ReleaseD3D(stream0Buffer);
			ReleaseD3D(stream1Buffer);
			ReleaseD3D(indexBuffer);
			ReleaseD3D(declaration);
			ReleaseD3D(vertexShader);
			ReleaseD3D(pixelShader);
			return ready;
		}
	}

	void BeginNativeA8CrossTextBatchFrame(
		size_t sequenceCapacity, UInt64 validationToken)
	{
		// Clear first so a runtime config toggle cannot leave a previous frame's
		// follower mappings or uploaded-instance cursor reachable while disabled.
		ClearFrameVectors();
		if (!g_bEnableFreeTypeFontCrossTextBatch
			|| !g_bEnableFreeTypeFontCommandBuffer)
		{
			return;
		}
		CrossTextFrame& frame = s_crossTextFrame;
		frame.sequence.reserve(sequenceCapacity);
		frame.sequenceToBatch.reserve(sequenceCapacity);
		frame.validationToken = validationToken;
		frame.building = frame.validationToken != 0;
		RefreshFrameMemory();
	}

	UInt32 AddNativeA8CrossTextBatchSequenceItem(
		NativeA8CrossTextCommandKind kind, NiTriShape* geometry,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload,
		UInt32 commandIndex, float accumulatorDepth)
	{
		CrossTextFrame& frame = s_crossTextFrame;
		if (!frame.building)
			return kInvalidNativeA8CommandIndex;
		CrossTextSequenceItem item;
		item.kind = kind;
		item.geometry = geometry;
		item.metadata = metadata;
		item.payload = payload;
		item.commandIndex = commandIndex;
		if (!std::isfinite(accumulatorDepth))
		{
			item.kind = NativeA8CrossTextCommandKind::Barrier;
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingDepthFallback);
		}
		else
		{
			std::memcpy(&item.depthBits, &accumulatorDepth,
				sizeof(item.depthBits));
		}
		const UInt32 index = static_cast<UInt32>(frame.sequence.size());
		frame.sequence.push_back(item);
		frame.sequenceToBatch.push_back(kInvalidNativeA8CommandIndex);
		if (item.kind != NativeA8CrossTextCommandKind::Barrier)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingCandidate);
		}
		return index;
	}

	void MarkNativeA8CrossTextBatchSequenceBarrier(UInt32 sequenceIndex)
	{
		CrossTextFrame& frame = s_crossTextFrame;
		if (sequenceIndex >= frame.sequence.size())
			return;
		frame.sequence[sequenceIndex] = {};
		frame.sequenceToBatch[sequenceIndex] = kInvalidNativeA8CommandIndex;
	}

	void PrepareNativeA8CrossTextBatches()
	{
		CrossTextFrame& frame = s_crossTextFrame;
		if (!frame.building || frame.sequence.size() < 2)
		{
			frame.building = false;
			return;
		}
		FreeTypePerfScope admissionPerf(
			FreeTypePerfPhase::GlyphInstancingAdmission);
		frame.admissionMembers.reserve(frame.sequence.size());
		frame.batches.reserve(frame.sequence.size() / 2u);
		// Reuse one small proof scratch for the entire traversal. Allocating a
		// vector for every singleton would erase much of the CPU saving in menus
		// whose adjacent depths rarely match.
		std::vector<CrossTextAdmissionCandidate> candidateMembers;
		candidateMembers.reserve(8);
		UInt64 acceptedBytes = 0;
		for (UInt32 index = 0;
			index < frame.sequence.size() && !frame.generationUnavailable;)
		{
			const CrossTextSequenceItem& firstItem = frame.sequence[index];
			if (firstItem.kind == NativeA8CrossTextCommandKind::Barrier)
			{
				++index;
				continue;
			}
			// A candidate cannot form the required two-text batch unless its
			// immediate successor is also a same-depth candidate. Avoid matrices,
			// property snapshots and command proofs for known singletons.
			if (index + 1u >= frame.sequence.size()
				|| frame.sequence[index + 1u].kind
					== NativeA8CrossTextCommandKind::Barrier)
			{
				++index;
				continue;
			}
			if (frame.sequence[index + 1u].depthBits != firstItem.depthBits)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingDepthFallback);
				++index;
				continue;
			}

			candidateMembers.clear();
			UInt64 candidateInstances = 0;
			UInt32 cursor = index;
			for (; cursor < frame.sequence.size(); ++cursor)
			{
				const CrossTextSequenceItem& item = frame.sequence[cursor];
				if (item.kind == NativeA8CrossTextCommandKind::Barrier)
					break;
				if (item.depthBits != firstItem.depthBits)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::GlyphInstancingDepthFallback);
					break;
				}
				CrossTextAdmissionCandidate candidate;
				const BuildMemberFailure failure =
					BuildAdmissionMember(item, cursor, candidate);
				if (failure != BuildMemberFailure::None)
				{
					RecordBuildFailure(failure);
					break;
				}
				if (!candidateMembers.empty())
				{
					const CrossTextAdmissionCandidate& leader =
						candidateMembers.front();
					if (!SameCompatibilityKey(
							leader.compatibility, candidate.compatibility,
							CompatibilityComparePhase::Admission))
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							GlyphInstancingStateFallback);
						break;
					}
					if (!SameNativeTileInstancingTransientState(
							leader.transient, candidate.transient))
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							GlyphInstancingScissorFallback);
						break;
					}
				}
				const UInt64 memberBytes = static_cast<UInt64>(
					candidate.member.sidecarCount)
					* kNativeA8GlyphInstanceBytes;
				if (acceptedBytes + candidateInstances
						* kNativeA8GlyphInstanceBytes + memberBytes
					> kNativeA8MaximumInstanceBufferBytes)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingBudgetFallback);
					break;
				}
				candidateInstances += candidate.member.sidecarCount;
				candidateMembers.push_back(std::move(candidate));
			}

			if (candidateMembers.size() >= 2)
			{
				CrossTextBatch batch;
				batch.firstMember = static_cast<UInt32>(
					frame.admissionMembers.size());
				batch.memberCount = static_cast<UInt32>(candidateMembers.size());
				batch.instanceCount = static_cast<UInt32>(candidateInstances);
				const UInt32 batchIndex = static_cast<UInt32>(frame.batches.size());
				for (CrossTextAdmissionCandidate& candidate : candidateMembers)
				{
					frame.sequenceToBatch[candidate.member.sequenceIndex] =
						batchIndex;
					frame.admissionMembers.push_back(
						std::move(candidate.member));
				}
				frame.batches.push_back(batch);
				acceptedBytes += candidateInstances
					* kNativeA8GlyphInstanceBytes;
				index = cursor;
			}
			else
			{
				++index;
			}
		}

		frame.building = false;
		const bool hadAcceptedBatches = !frame.batches.empty();
		UInt32 maximumBatchMembers = 0;
		for (const CrossTextBatch& batch : frame.batches)
			maximumBatchMembers = std::max(maximumBatchMembers, batch.memberCount);
		// Reserve both leader-time arrays during command build.  Begin never grows
		// either vector, so live capture cannot allocate inside TileShader.
		frame.livePreflight.reserve(maximumBatchMembers);
		frame.liveSnapshots.reserve(maximumBatchMembers);
		// Allocate and size the D3D resources now, but do not freeze WVP/TileColor
		// during command construction.  Each leader refreshes and uploads only its
		// own batch immediately before drawing.
		frame.active = hadAcceptedBatches
			&& PrepareFrameInstanceResources();
		if (!frame.active)
		{
			if (hadAcceptedBatches && frame.generation)
				DisableInstancingGeneration(frame.generation);
			for (UInt32& mapping : frame.sequenceToBatch)
				mapping = kInvalidNativeA8CommandIndex;
			frame.batches.clear();
			frame.admissionMembers.clear();
			ResetLiveBatchScratch();
		}
		else
		{
			// "accepted" means immutable admission and instance-buffer capacity
			// are ready.  Live matrices/colors are uploaded at the leader callback;
			// successful draw counts remain separate from this CPU admission count.
			for (const CrossTextBatch& batch : frame.batches)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingAcceptedBatch);
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingAcceptedText,
					batch.memberCount);
				RecordFreeTypePerf(
					FreeTypePerfCounter::GlyphInstancingInstance,
					batch.instanceCount);
				RecordFreeTypeGlyphInstancingBatchSize(
					batch.memberCount, batch.instanceCount);
			}
		}
		RefreshFrameMemory();
	}

	bool ShouldConsumeNativeA8CrossTextBatchFollower(
		UInt32 sequenceIndex, NiTriShape* geometry)
	{
		const CrossTextFrame& frame = s_crossTextFrame;
		if (!frame.active || sequenceIndex >= frame.sequenceToBatch.size())
			return false;
		const UInt32 batchIndex = frame.sequenceToBatch[sequenceIndex];
		if (batchIndex == kInvalidNativeA8CommandIndex
			|| batchIndex >= frame.batches.size())
		{
			return false;
		}
		const CrossTextBatch& batch = frame.batches[batchIndex];
		if (batch.state != CrossTextBatchState::Consumed
			|| !batch.memberCount)
		{
			return false;
		}
		const CrossTextAdmissionMember& leader =
			frame.admissionMembers[batch.firstMember];
		if (leader.sequenceIndex == sequenceIndex)
			return false;
		// Accepted members are a contiguous slice of the final accumulator
		// sequence. Resolve the follower directly instead of linearly scanning a
		// potentially glyph-heavy menu batch for every callback.
		if (sequenceIndex < leader.sequenceIndex)
			return false;
		const UInt32 offset = sequenceIndex - leader.sequenceIndex;
		if (!offset || offset >= batch.memberCount)
			return false;
		const CrossTextAdmissionMember& member =
			frame.admissionMembers[batch.firstMember + offset];
		if (member.sequenceIndex == sequenceIndex
			&& member.sequence.geometry == geometry)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingFollowerConsumed);
			return true;
		}
		return false;
	}

	bool BeginNativeA8CrossTextBatchExecution(UInt32 sequenceIndex,
		NiTriShape* leaderGeometry,
		BSShaderProperty::RenderPass* renderPass, UInt32 currentPass,
		bool testAlpha, bool blendAlpha, bool setupDrawmode,
		NativeA8CrossTextBatchExecutionView& view)
	{
		view = {};
		CrossTextFrame& frame = s_crossTextFrame;
		if (!frame.active || sequenceIndex >= frame.sequenceToBatch.size()
			|| !leaderGeometry || !renderPass || !frame.accumulator)
		{
			return false;
		}
		const UInt32 batchIndex = frame.sequenceToBatch[sequenceIndex];
		if (batchIndex == kInvalidNativeA8CommandIndex
			|| batchIndex >= frame.batches.size())
		{
			return false;
		}
		CrossTextBatch& batch = frame.batches[batchIndex];
		if (batch.state != CrossTextBatchState::Ready
			|| batch.memberCount < 2)
		{
			return false;
		}
		FreeTypePerfScope leaderPerf(
			FreeTypePerfPhase::GlyphInstancingLeader);
		const auto rejectBegin = [&](FreeTypePerfCounter detail,
			const char* reason, UInt32 memberOffset = 0u)
		{
			LogInstancingBeginDiagnostic(reason, sequenceIndex, batchIndex,
				memberOffset, leaderGeometry, renderPass, currentPass,
				testAlpha, blendAlpha, setupDrawmode);
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingBeginFallback);
			RecordFreeTypePerf(detail);
			batch.state = CrossTextBatchState::Fallback;
			ResetLiveBatchScratch();
			return false;
		};
		if (batch.firstMember >= frame.admissionMembers.size()
			|| static_cast<UInt64>(batch.firstMember) + batch.memberCount
				> frame.admissionMembers.size())
		{
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginImmutableFallback, "admission-range");
		}
		const CrossTextAdmissionMember& leader =
			frame.admissionMembers[batch.firstMember];
		if (leader.sequenceIndex != sequenceIndex
			|| leader.sequence.geometry != leaderGeometry
			// Formal B64FB5-B64FD1 reuses this one accumulator-owned Tile pass for
			// the complete backwards loop, changing only pGeometry and pushing
			// testAlpha/blendAlpha/setupDrawmode = 1/1/1. This exact identity and
			// call contract prove pass enum/flags/lights and drawmode applicability
			// for every accepted follower.
			|| renderPass != &frame.accumulator->kTileRenderPass
			|| renderPass->pGeometry != leaderGeometry
			|| renderPass->usPassEnum != currentPass)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingBeginContractFallback);
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginPassFallback, "pass-contract");
		}
		if (renderPass->ucNumLights
			|| !testAlpha || !blendAlpha || !setupDrawmode)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingBeginContractFallback);
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginCallbackFallback, "callback-contract");
		}
		if (!IsNativeA8ShaderGenerationCurrent(frame.generation)
			|| !IsNativeA8FrameResourceStampCurrent(frame.generation,
				frame.resourceSerial, frame.uploadEpoch))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingBeginContractFallback);
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginResourceFallback, "resource-epoch");
		}
		ResetLiveBatchScratch();
		if (frame.livePreflight.capacity() < batch.memberCount
			|| frame.liveSnapshots.capacity() < batch.memberCount)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginResourceFallback, "live-scratch-capacity");
		}
		frame.liveBatchIndex = batchIndex;
		const auto rejectLivePreflight = [&](FreeTypePerfCounter detail,
			const char* reason, UInt32 memberOffset)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingBeginPreflightFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingLiveSnapshotAvoidedText,
				batch.memberCount);
			return rejectBegin(detail, reason, memberOffset);
		};

		// Pass 1 independently re-resolves immutable/property identity into the
		// batch-local live scratch, normalizes the compatibility key and transient
		// state, then proves current visibility.  The retained admission plan is
		// never modified, and no WVP/TileColor is built until the whole pass succeeds.
		{
			FreeTypePerfScope livePreflightPerf(
				FreeTypePerfPhase::GlyphInstancingLivePreflight);
			for (UInt32 offset = 0; offset < batch.memberCount; ++offset)
			{
				const CrossTextAdmissionMember& planned =
					frame.admissionMembers[batch.firstMember + offset];
				frame.livePreflight.emplace_back();
				CrossTextLivePreflight& current =
					frame.livePreflight.back();
				const BuildMemberFailure failure = BuildLivePreflightMember(
					planned, current);
				if (failure != BuildMemberFailure::None)
				{
					RecordBuildFailure(failure);
					return rejectLivePreflight(
						failure == BuildMemberFailure::Scissor
						? FreeTypePerfCounter::
							GlyphInstancingBeginTransientFallback
						: FreeTypePerfCounter::
							GlyphInstancingBeginImmutableFallback,
						BuildMemberFailureName(failure), offset);
				}
				if (offset != 0
					&& !SameCompatibilityKey(
						frame.livePreflight.front().compatibility,
						current.compatibility,
						CompatibilityComparePhase::Live))
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::GlyphInstancingStateFallback);
					return rejectLivePreflight(FreeTypePerfCounter::
						GlyphInstancingBeginImmutableFallback,
						"compatibility-key", offset);
				}
				if (offset != 0
					&& !SameNativeTileInstancingTransientState(
						frame.livePreflight.front().transient,
						current.transient))
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::GlyphInstancingScissorFallback);
					return rejectLivePreflight(FreeTypePerfCounter::
						GlyphInstancingBeginTransientFallback,
						"transient-state", offset);
				}
			}
		}

		// Pass 2: the render-thread-only preflight above completed without a
		// callback or device submission. Freeze exactly one complete live snapshot
		// per member, and upload only after every snapshot succeeds.
		{
			FreeTypePerfScope snapshotPerf(
				FreeTypePerfPhase::GlyphInstancingSnapshot);
			for (UInt32 offset = 0; offset < batch.memberCount; ++offset)
			{
				const CrossTextAdmissionMember& planned =
					frame.admissionMembers[batch.firstMember + offset];
				frame.liveSnapshots.emplace_back();
				const BuildMemberFailure failure =
					BuildCompleteLiveMemberSnapshot(planned,
						frame.livePreflight[offset],
						frame.liveSnapshots.back());
				if (failure != BuildMemberFailure::None)
				{
					RecordBuildFailure(failure);
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingBeginSnapshotFallback);
					return rejectBegin(failure == BuildMemberFailure::Scissor
						? FreeTypePerfCounter::
							GlyphInstancingBeginTransientFallback
						: FreeTypePerfCounter::
							GlyphInstancingBeginImmutableFallback,
						BuildMemberFailureName(failure), offset);
				}
			}
		}

		if (!UploadBatchInstances(batchIndex, batch))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			return rejectBegin(FreeTypePerfCounter::
				GlyphInstancingBeginUploadFallback, "instance-upload");
		}

		batch.followersBegun = 0;
		{
			FreeTypePerfScope followerReservePerf(
				FreeTypePerfPhase::GlyphInstancingFollowerReserve);
			for (UInt32 offset = 1; offset < batch.memberCount; ++offset)
			{
				const CrossTextAdmissionMember& follower =
					frame.admissionMembers[batch.firstMember + offset];
				if (!BeginFollowerCommand(follower))
				{
					for (UInt32 rollback = 1;
						rollback <= batch.followersBegun; ++rollback)
					{
						EndFollowerCommand(frame.admissionMembers[
							batch.firstMember + rollback], false);
					}
					batch.followersBegun = 0;
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingStateFallback);
					return rejectBegin(FreeTypePerfCounter::
						GlyphInstancingBeginFollowerFallback,
						"follower-command", offset);
				}
				++batch.followersBegun;
			}
		}

		batch.state = CrossTextBatchState::Executing;
		view.batchIndex = batchIndex;
		view.leaderSequenceIndex = sequenceIndex;
		view.textCount = batch.memberCount;
		view.instanceCount = batch.instanceCount;
		view.baseInstance = batch.baseInstance;
		view.generation = frame.generation;
		view.leaderGeometry = leaderGeometry;
		view.lastGeometry = frame.admissionMembers[
			batch.firstMember + batch.memberCount - 1u].sequence.geometry;
		view.active = true;
		return true;
	}

	void EndNativeA8CrossTextBatchExecution(
		NativeA8CrossTextBatchExecutionView& view, bool success)
	{
		CrossTextFrame& frame = s_crossTextFrame;
		if (!view.active || view.batchIndex >= frame.batches.size())
		{
			view = {};
			return;
		}
		CrossTextBatch& batch = frame.batches[view.batchIndex];
		if (batch.state != CrossTextBatchState::Executing)
		{
			if (frame.liveBatchIndex == view.batchIndex)
				ResetLiveBatchScratch();
			view = {};
			return;
		}
		const bool liveSnapshotsReady =
			frame.liveBatchIndex == view.batchIndex
			&& frame.liveSnapshots.size() == batch.memberCount;
		if (success && !liveSnapshotsReady)
		{
			success = false;
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingValidationFallback);
		}
		if (success)
		{
			const CrossTextAdmissionMember& leader =
				frame.admissionMembers[batch.firstMember];
			if (!IsLeaderCommandConsumed(leader))
			{
				success = false;
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingValidationFallback);
			}
		}
		for (UInt32 offset = 1; offset <= batch.followersBegun; ++offset)
		{
			const CrossTextAdmissionMember& follower =
				frame.admissionMembers[batch.firstMember + offset];
			EndFollowerCommand(follower, success);
			if (success && follower.sequence.kind
				== NativeA8CrossTextCommandKind::VirtualSinglePacket)
			{
				VirtualStockSingletonState* singleton =
					follower.sequence.metadata
						? GetVirtualStockSingletonState(
							*follower.sequence.metadata) : nullptr;
				if (singleton)
				{
					singleton->directDrawCount.fetch_add(
						1, std::memory_order_acq_rel);
				}
			}
		}
		batch.followersBegun = 0;
		batch.state = success
			? CrossTextBatchState::Consumed
			: CrossTextBatchState::Fallback;
		if (success)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingDrawSaved,
				batch.memberCount - 1u);
			for (UInt32 offset = 0; offset < batch.memberCount; ++offset)
			{
				NiTriShape* geometry = frame.admissionMembers[
					batch.firstMember + offset].sequence.geometry;
				NiTriShapeData* data = geometry
					? geometry->GetModelData() : nullptr;
				if (data)
					data->m_usDirtyFlags &= 0xF000u;
			}
			const CrossTextAdmissionMember& last = frame.admissionMembers[
				batch.firstMember + batch.memberCount - 1u];
			const NativeTileInstancingSnapshot& lastSnapshot =
				frame.liveSnapshots.back();
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (renderer && renderer->GetD3DDevice() == frame.device)
			{
				renderer->m_pkCurrProp =
					&last.sequence.geometry->m_kProperties;
				renderer->m_pkCurrEffects = nullptr;
				std::memcpy(&renderer->m_kD3DMat,
					&lastSnapshot.retailWorld,
					sizeof(lastSnapshot.retailWorld));
			}
		}
		ResetLiveBatchScratch();
		view = {};
	}

	bool ExecuteNativeA8CrossTextInstancedDraw(
		const NativeA8CrossTextBatchExecutionView& view,
		NiDX9Renderer* renderer, NiD3DRenderState* renderState,
		const char*& operation, HRESULT& result)
	{
		operation = "validate-glyph-instancing";
		result = D3DERR_INVALIDCALL;
		CrossTextFrame& frame = s_crossTextFrame;
		if (!view.active || !frame.active
			|| view.batchIndex >= frame.batches.size()
			|| frame.batches[view.batchIndex].state
				!= CrossTextBatchState::Executing
			|| view.textCount
				!= frame.batches[view.batchIndex].memberCount
			|| view.instanceCount
				!= frame.batches[view.batchIndex].instanceCount
			|| frame.liveBatchIndex != view.batchIndex
			|| frame.liveSnapshots.size() != view.textCount
			|| (static_cast<UInt64>(view.baseInstance)
					+ view.instanceCount) * kNativeA8GlyphInstanceBytes
					> s_instancingResources.instanceBufferBytes
			|| view.generation != frame.generation
			|| !renderer || !renderState || !frame.device
			|| renderer->GetD3DDevice() != frame.device
			|| renderState->m_pkD3DDevice != frame.device
			|| s_instancingResources.device != frame.device
			|| s_instancingResources.generation != frame.generation
			|| s_instancingResources.disabledGeneration == frame.generation
			|| !s_instancingResources.cornerBuffer
			|| !s_instancingResources.indexBuffer
			|| !s_instancingResources.instanceBuffer)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingValidationFallback);
			return false;
		}
		NativeA8InstancingShaderResources shaders;
		if (!GetNativeA8InstancingShaderResources(frame.generation, shaders)
			|| shaders.device != frame.device)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingValidationFallback);
			return false;
		}
		CrossTextBatch& batch = frame.batches[view.batchIndex];
		if (view.baseInstance != batch.baseInstance
			|| batch.firstMember >= frame.admissionMembers.size()
			|| static_cast<UInt64>(batch.firstMember) + batch.memberCount
				> frame.admissionMembers.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingValidationFallback);
			return false;
		}
		const CrossTextAdmissionMember& leader =
			frame.admissionMembers[batch.firstMember];
		const CrossTextAdmissionMember& last = frame.admissionMembers[
			batch.firstMember + batch.memberCount - 1u];
		const NativeTileInstancingSnapshot& leaderSnapshot =
			frame.liveSnapshots.front();
		const NativeTileInstancingSnapshot& lastSnapshot =
			frame.liveSnapshots.back();
		if (!IsRestorableMember(&leader, frame.device)
			|| !IsRestorableMember(&last, frame.device)
			|| leader.command->program != last.command->program
			|| leader.command->binding.declaration
				!= last.command->binding.declaration
			|| renderState->m_hCurrentVertexShader
				!= leader.command->program->vertexShader
			|| renderState->m_hCurrentPixelShader
				!= leader.command->program->pixelShader
			|| renderer->m_pkCurrProp
				!= &leader.sequence.geometry->m_kProperties
			|| renderer->m_pkCurrEffects)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateFallback);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingValidationFallback);
			return false;
		}

		IDirect3DDevice9* device = frame.device;
		const UInt32 instanceOffset = view.baseInstance
			* kNativeA8GlyphInstanceBytes;
		// Follow the D3D9 indexed-instancing state order exactly. These calls are
		// deliberately made on the device, without publishing a temporary program
		// or declaration to Gamebryo's software cache; the guard restores the
		// complete normal state before control returns to either replay path.
		const UInt32 expectedFrequency0 =
			D3DSTREAMSOURCE_INDEXEDDATA | view.instanceCount;
		const float identity[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		auto bindInstancingState = [&]()
		{
			FreeTypePerfScope bindPerf(
				FreeTypePerfPhase::GlyphInstancingBind);
			operation = "bind-glyph-instancing";
			const HRESULT frequency0 = device->SetStreamSourceFreq(
				0, expectedFrequency0);
			const HRESULT frequency1 = device->SetStreamSourceFreq(1,
				D3DSTREAMSOURCE_INSTANCEDATA | 1u);
			const HRESULT stream0 = device->SetStreamSource(0,
				s_instancingResources.cornerBuffer, 0,
				4u * sizeof(float));
			const HRESULT stream1 = device->SetStreamSource(1,
				s_instancingResources.instanceBuffer, instanceOffset,
				kNativeA8GlyphInstanceBytes);
			const HRESULT declaration = device->SetVertexDeclaration(
				shaders.declaration);
			const HRESULT vertexShader =
				device->SetVertexShader(shaders.vertexShader);
			const HRESULT pixelShader = device->SetPixelShader(
				leader.command->program->pixelShader);
			const HRESULT indices = device->SetIndices(
				s_instancingResources.indexBuffer);
			RecordFreeTypePerf(FreeTypePerfCounter::
				GlyphInstancingStreamFrequencySet, 2);
			if (FAILED(stream0) || FAILED(stream1) || FAILED(indices)
				|| FAILED(frequency0) || FAILED(frequency1)
				|| FAILED(declaration) || FAILED(vertexShader)
				|| FAILED(pixelShader))
			{
				result = FAILED(stream0) ? stream0
					: FAILED(stream1) ? stream1
					: FAILED(indices) ? indices
					: FAILED(frequency0) ? frequency0
					: FAILED(frequency1) ? frequency1
					: FAILED(declaration) ? declaration
					: FAILED(vertexShader) ? vertexShader : pixelShader;
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingBindingFailure);
				DisableInstancingGeneration(frame.generation);
				return false;
			}

			operation = "set-glyph-instancing-identity-c0";
			result = device->SetPixelShaderConstantF(0, identity, 1);
			if (FAILED(result))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingConstantFailure);
				DisableInstancingGeneration(frame.generation);
				return false;
			}
			return true;
		};

		std::optional<InstancingDeviceStateGuard> resetGuard;
		const auto armResetGuard = [&]()
		{
			resetGuard.reset();
			resetGuard.emplace(device, renderer, renderState,
				frame.generation, &leader, &leaderSnapshot,
				&last, &lastSnapshot);
		};
		armResetGuard();
		if (!bindInstancingState())
		{
			resetGuard->Restore();
			return false;
		}
		if (s_instancingResources.stateProofGeneration != frame.generation)
		{
			operation = "prove-glyph-instancing-state";
			if (!ProveInstancingDeviceState(device, shaders,
					*leader.command->program, expectedFrequency0,
					instanceOffset))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingStateProofFailure);
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingBindingFailure);
				DisableInstancingGeneration(frame.generation);
				resetGuard->Restore();
				return false;
			}
			// Prove the complete inverse transition before the first real draw of
			// a generation.  A wrapper that accepts instanced bindings but cannot
			// restore both frequencies and the normal Tile declaration is rejected
			// before it can emit stretched geometry into the frame.
			operation = "prove-glyph-instancing-restore";
			const bool restoreReady = resetGuard->Restore();
			const bool normalStateReady = restoreReady
				&& ProveNormalDeviceState(device, leader, leaderSnapshot);
			if (!normalStateReady)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					GlyphInstancingStateProofFailure);
				if (restoreReady)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						GlyphInstancingRestoreFailure);
				}
				DisableInstancingGeneration(frame.generation);
				return false;
			}
			s_instancingResources.stateProofGeneration = frame.generation;
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingStateProof);
			armResetGuard();
			if (!bindInstancingState())
			{
				resetGuard->Restore();
				return false;
			}
		}

		operation = "draw-glyph-instancing";
		{
			FreeTypePerfScope drawPerf(
				FreeTypePerfPhase::GlyphInstancingDraw);
			result = device->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
		}
		if (FAILED(result))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::GlyphInstancingDrawFailure);
			DisableInstancingGeneration(frame.generation);
			resetGuard->Restore();
			return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GlyphInstancingDraw);
		resetGuard->MarkDrawSucceeded();
		if (!resetGuard->Restore())
		{
			// The indexed draw has already been submitted, so replaying the same
			// non-commutative alpha sequence would be less correct than consuming
			// it once.  The generation is disabled by Restore(); the pre-draw inverse
			// proof above makes this path indicative of device loss/state failure,
			// not an unsupported wrapper transition.
			operation = "restore-after-glyph-instancing";
			result = D3DERR_DEVICELOST;
			return true;
		}
		operation = "none";
		return true;
	}

	void EndNativeA8CrossTextBatchFrame()
	{
		const CrossTextFrame& frame = s_crossTextFrame;
		if (!frame.building && !frame.active
			&& frame.sequence.empty() && frame.admissionMembers.empty()
			&& frame.batches.empty() && frame.sequenceToBatch.empty()
			&& frame.livePreflight.empty() && frame.liveSnapshots.empty()
			&& !frame.validationToken)
		{
			return;
		}
		ClearFrameVectors();
	}

	void ReleaseNativeA8CrossTextInstancingResources()
	{
		EndNativeA8CrossTextBatchFrame();
		ReleaseResources(s_instancingResources);
		s_instancingResources.disabledGeneration = 0;
	}
}
