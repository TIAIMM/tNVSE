#include "font_native_command_detail.h"

#include "load_config.h"

#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	using namespace implementation::font_native_command_buffer;

	bool IsNativeFontStandardPassLiteDispatchCurrent(
		const NativeFontStandardPassLiteDispatch& dispatch,
		const NiTriShape* geometry,
		const NativeFontCompiledPacketCommand* program,
		UInt32 generation)
	{
		return dispatch.ready && geometry && program && generation
			&& dispatch.geometry == geometry
			&& dispatch.properties == &geometry->m_kProperties
			&& dispatch.program == program
			&& dispatch.shader == program->shader
			&& dispatch.renderer
			&& dispatch.generation == generation
			&& program->generation == generation
			&& dispatch.standardV2Ready
				== (program->standardV2SlotProofs
					== NativeFontCompiledPacketCommand::
						kStandardV2RequiredProofs);
	}

	void InvalidateNativeFontStandardPassLiteDispatch(
		NativeFontStandardPassLiteDispatch& dispatch)
	{
		dispatch = {};
	}

	bool BuildNativeFontStandardPassLiteDispatch(
		NiTriShape* geometry,
		const NativeFontCompiledPacketCommand* program,
		UInt32 generation,
		NativeFontStandardPassLiteDispatch& dispatch)
	{
		if (IsNativeFontStandardPassLiteDispatchCurrent(
				dispatch, geometry, program, generation))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteRetainedReuse);
			return true;
		}

		InvalidateNativeFontStandardPassLiteDispatch(dispatch);
		if (!g_bEnableFreeTypeFontCommandBuffer
			|| !geometry || !program || !generation
			|| !State().standardPassLitePredicatesValidated
			|| geometry->GetSkinInstance()
			|| !geometry->GetModelData())
		{
			return false;
		}

		void** geometryVtable =
			*reinterpret_cast<void***>(geometry);
		TileShader* shader = program->shader;
		void** shaderVtable = shader
			? *reinterpret_cast<void***>(shader) : nullptr;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!IsNativeFontAtlasShape(geometry)
			|| geometryVtable[kRenderImmediateAltSlot]
				!= reinterpret_cast<void*>(&NativeFontRenderImmediateAlt)
			|| !program->active || !program->profile
			|| program->generation != generation
			|| !shader || !shaderVtable
			|| shaderVtable != program->shaderVtable
			|| !program->prepareGeometryForRendering
			|| !program->setupGeometryTextures
			|| !program->setupGeometryConstants
			|| !program->setupGeometryAlphaBlending
			|| !program->setupGeometryAlphaTesting
			|| !program->setupGeometryRenderStates
			|| !program->postGeometry
			|| !program->setupNonFirstPass
			|| !renderer
			|| program->device != renderer->GetD3DDevice())
		{
			return false;
		}

		dispatch.geometry = geometry;
		dispatch.properties = &geometry->m_kProperties;
		dispatch.renderer = renderer;
		dispatch.shader = shader;
		dispatch.program = program;
		dispatch.generation = generation;
		dispatch.standardV2Ready =
			program->standardV2SlotProofs
				== NativeFontCompiledPacketCommand::
					kStandardV2RequiredProofs;
		dispatch.ready = true;
		RecordFreeTypePerf(
			FreeTypePerfCounter::StandardPassLiteRetainedBuild);
		return true;
	}

	size_t GetNativeFontTileRetainedCapacityBytes(
		const NativeFontShapePayload& payload)
	{
		return payload.retainedText.packets.heap_capacity()
				* sizeof(NativeFontTileRetainedPacket)
			+ payload.retainedText.runs.heap_capacity()
				* sizeof(NativeFontTileRetainedRun);
	}

	void InvalidateNativeFontTileRetainedText(
		NativeFontShapePayload& payload,
		bool preserveStandardPassLite)
	{
		NativeFontTileRetainedText& retained = payload.retainedText;
		retained.ready = false;
		retained.atlasTextureEpoch = 0;
		retained.bridgeEligible = false;
		if (!preserveStandardPassLite)
		{
			InvalidateNativeFontStandardPassLiteDispatch(
				retained.standardPassLite);
		}
	}

	bool BuildNativeFontTileRetainedText(NiTriShape* ownerTile,
		NativeFontShapePayload& payload, UInt32 generation,
		UInt32 atlasTextureEpoch)
	{
		NativeFontTileRetainedText& retained = payload.retainedText;
		// Keep an identity-matching Standard-lite dispatch available while a
		// preflight refresh proves that the Tile/program pair is unchanged.
		retained.ready = false;
		retained.atlasTextureEpoch = 0;
		retained.bridgeEligible = false;
		if (!g_bEnableFreeTypeFontCommandBuffer || !ownerTile
			|| !generation || !atlasTextureEpoch
			|| !payload.payloadTemplate)
		{
			InvalidateNativeFontStandardPassLiteDispatch(
				retained.standardPassLite);
			return false;
		}

		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(artifact, payload.useCompositePackets);
		auto discardRetained = [&retained]()
		{
			retained.ready = false;
			retained.ownerTile = nullptr;
			retained.artifact = nullptr;
			retained.generation = 0;
			retained.atlasTextureEpoch = 0;
			retained.useCompositePackets = false;
			retained.bridgeEligible = false;
			retained.packets.clear();
			retained.runs.clear();
			InvalidateNativeFontStandardPassLiteDispatch(
				retained.standardPassLite);
		};
		if (packets.empty()
			|| payload.packetShaders.size() != packets.size()
			|| payload.packetPrograms.size() != packets.size()
			|| payload.preflightAtlasTextures.size()
				!= artifact.atlasTextures.size())
		{
			discardRetained();
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}

		bool canRefresh = retained.ownerTile == ownerTile
			&& retained.artifact == &artifact
			&& retained.generation == generation
			&& retained.useCompositePackets
				== payload.useCompositePackets
			&& retained.packets.size() == packets.size()
			&& !retained.runs.empty();
		for (UInt32 index = 0;
			index < static_cast<UInt32>(packets.size()); ++index)
		{
			const NativeFontPacketTemplate& packet = packets[index];
			const NativeFontCompiledPacketCommand* program =
				payload.packetPrograms[index];
			const UInt64 packetEnd =
				static_cast<UInt64>(packet.firstVertex)
					+ packet.vertexCount;
			if (!packet.vertexCount || (packet.vertexCount & 3u)
				|| packetEnd > artifact.gpuVertices.size()
				|| packet.atlasPage
					>= payload.preflightAtlasTextures.size()
				|| !payload.preflightAtlasTextures[packet.atlasPage]
				|| !payload.packetShaders[index] || !program
				|| !program->active || !program->profile
				|| program->generation != generation
				|| program->shader != payload.packetShaders[index])
			{
				discardRetained();
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedMiss);
				return false;
			}

			if (canRefresh)
			{
				const NativeFontTileRetainedPacket& existing =
					retained.packets[index];
				canRefresh = existing.packet == &packet
					&& existing.program == program
					&& existing.packetIndex == index
					&& existing.firstVertex == packet.firstVertex
					&& existing.vertexCount == packet.vertexCount
					&& existing.atlasPage == packet.atlasPage;
				continue;
			}
		}

		if (canRefresh)
		{
			if (retained.packets.size() == 1)
			{
				BuildNativeFontStandardPassLiteDispatch(
					ownerTile, retained.packets.front().program,
					generation, retained.standardPassLite);
			}
			else
			{
				InvalidateNativeFontStandardPassLiteDispatch(
					retained.standardPassLite);
			}
			retained.atlasTextureEpoch = atlasTextureEpoch;
			retained.bridgeEligible = retained.packets.size() > 1;
			retained.ready = true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedRefresh);
			return true;
		}

		discardRetained();
		if (retained.packets.capacity() < packets.size()
			|| retained.runs.capacity() < packets.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}

		for (UInt32 index = 0;
			index < static_cast<UInt32>(packets.size()); ++index)
		{
			const NativeFontPacketTemplate& packet = packets[index];
			NativeFontTileRetainedPacket command;
			command.packet = &packet;
			command.program = payload.packetPrograms[index];
			command.packetIndex = index;
			command.firstVertex = packet.firstVertex;
			command.vertexCount = packet.vertexCount;
			command.atlasPage = packet.atlasPage;
			retained.packets.push_back(command);
		}

		for (UInt32 first = 0;
			first < static_cast<UInt32>(retained.packets.size());)
		{
			const void* profile =
				retained.packets[first].program->profile;
			UInt32 end = first + 1u;
			while (end < retained.packets.size()
				&& retained.packets[end].program->profile == profile)
			{
				++end;
			}
			NativeFontTileRetainedRun run;
			run.firstPacket = first;
			run.packetCount = end - first;
			run.bridgeEligible = true;
			run.continuesBridgeSpan = first != 0;
			retained.runs.push_back(run);
			first = end;
		}

		retained.ownerTile = ownerTile;
		retained.artifact = &artifact;
		retained.generation = generation;
		retained.atlasTextureEpoch = atlasTextureEpoch;
		retained.useCompositePackets = payload.useCompositePackets;
		retained.bridgeEligible = retained.packets.size() > 1;
		if (retained.packets.size() == 1)
		{
			BuildNativeFontStandardPassLiteDispatch(
				ownerTile, retained.packets.front().program,
				generation, retained.standardPassLite);
		}
		else
		{
			InvalidateNativeFontStandardPassLiteDispatch(
				retained.standardPassLite);
		}
		retained.ready = !retained.packets.empty()
			&& !retained.runs.empty();
		if (!retained.ready)
		{
			discardRetained();
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandTileRetainedBuild);
		return true;
	}

	bool IsNativeFontTileRetainedTextCurrent(
		const NativeFontShapePayload& payload,
		const NiTriShape* ownerTile, UInt32 generation,
		UInt32 atlasTextureEpoch)
	{
		const NativeFontTileRetainedText& retained =
			payload.retainedText;
		if (!retained.ready || !ownerTile || !generation
			|| !atlasTextureEpoch || !payload.payloadTemplate
			|| retained.ownerTile != ownerTile
			|| retained.artifact != payload.payloadTemplate.get()
			|| retained.generation != generation
			|| retained.atlasTextureEpoch != atlasTextureEpoch
			|| retained.useCompositePackets
				!= payload.useCompositePackets)
		{
			return false;
		}
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		return !retained.packets.empty()
			&& !retained.runs.empty()
			&& retained.packets.size() == packets.size();
	}
}
