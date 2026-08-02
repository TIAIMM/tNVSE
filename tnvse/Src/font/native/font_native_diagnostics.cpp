#include "font_native_internal.h"

#include "load_config.h"

#include "NiDX9Renderer.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiTriShapeData.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_diagnostics {}
	using namespace implementation::font_native_diagnostics;

	namespace implementation::font_native_diagnostics
	{
		constexpr UInt32 kInitialDrawProbeCount = 8;
		constexpr UInt32 kLargeDrawVertexThreshold = 128;
		constexpr UInt32 kMaximumSelectedDrawProbes = 16;

		std::atomic<UInt32> s_drawProbeCandidates = 0;
		std::atomic<UInt32> s_selectedDrawProbes = 0;

		const char* DiagnosticStageName(
			NativeA8DrawPathDiagnosticStage stage)
		{
			switch (stage)
			{
			case NativeA8DrawPathDiagnosticStage::DirectBefore:
				return "direct-before";
			case NativeA8DrawPathDiagnosticStage::DirectAfter:
				return "direct-after";
			case NativeA8DrawPathDiagnosticStage::Slot27Before:
				return "slot27-before";
			case NativeA8DrawPathDiagnosticStage::Slot27After:
				return "slot27-after";
			case NativeA8DrawPathDiagnosticStage::InstancingFallbackAfter:
				return "instancing-fallback-after";
			default:
				return "unknown";
			}
		}

		const char* ConstantsActionName(
			NativeA8DrawConstantsDiagnosticAction action)
		{
			switch (action)
			{
			case NativeA8DrawConstantsDiagnosticAction::ExactReuse:
				return "exact-reuse";
			case NativeA8DrawConstantsDiagnosticAction::ConstantsLite:
				return "constants-lite";
			case NativeA8DrawConstantsDiagnosticAction::TranslationLite:
				return "translation-lite";
			case NativeA8DrawConstantsDiagnosticAction::RetailFull:
				return "retail-full";
			case NativeA8DrawConstantsDiagnosticAction::Unknown:
			default:
				return "unknown";
			}
		}

		UInt64 HashBytes(const void* data, size_t size)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			UInt64 value = 14695981039346656037ull;
			for (size_t index = 0; index < size; ++index)
			{
				value ^= bytes[index];
				value *= 1099511628211ull;
			}
			return value;
		}

		template <class T>
		void ReleaseDiagnosticReference(T*& value)
		{
			if (value)
			{
				value->Release();
				value = nullptr;
			}
		}
	}

	UInt32 AcquireNativeA8DrawPathDiagnostic(
		const NativeA8DrawCommand& command)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return 0;
		const UInt32 candidate = s_drawProbeCandidates.fetch_add(
			1, std::memory_order_relaxed) + 1u;
		if (candidate > kInitialDrawProbeCount
			&& command.binding.vertexCount < kLargeDrawVertexThreshold)
		{
			return 0;
		}
		const UInt32 selected = s_selectedDrawProbes.fetch_add(
			1, std::memory_order_relaxed) + 1u;
		return selected <= kMaximumSelectedDrawProbes ? selected : 0;
	}

	void LogNativeA8DrawPathDiagnostic(UInt32 diagnosticId,
		NativeA8DrawPathDiagnosticStage stage, NiTriShape* geometry,
		const NiPropertyState* properties, NiDX9Renderer* renderer,
		const NativeA8CompiledPacketCommand& program,
		const NativeA8DrawCommand& command,
		const NativeA8DrawPathDiagnosticContext& context,
		NiGeometryBufferData* buffer, UInt32 directFallback,
		bool cachedBindingReady)
	{
		if (!diagnosticId || !g_bEnableFreeTypeFontRenderingLog)
			return;
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		if (!device)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_native_draw_diag: id=%u stage=%s crossText=%u geometry=%p renderer=%p device=null fallback=%u",
				diagnosticId, DiagnosticStageName(stage),
				g_bEnableFreeTypeFontCrossTextBatch ? 1u : 0u,
				geometry, renderer, directFallback);
			return;
		}

		UINT frequency0 = 0;
		UINT frequency1 = 0;
		UINT stream0Offset = 0;
		UINT stream0Stride = 0;
		UINT stream1Offset = 0;
		UINT stream1Stride = 0;
		IDirect3DVertexBuffer9* stream0 = nullptr;
		IDirect3DVertexBuffer9* stream1 = nullptr;
		IDirect3DIndexBuffer9* indices = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		IDirect3DVertexShader9* vertexShader = nullptr;
		IDirect3DPixelShader9* pixelShader = nullptr;
		const HRESULT frequency0Result =
			device->GetStreamSourceFreq(0, &frequency0);
		const HRESULT frequency1Result =
			device->GetStreamSourceFreq(1, &frequency1);
		const HRESULT stream0Result = device->GetStreamSource(
			0, &stream0, &stream0Offset, &stream0Stride);
		const HRESULT stream1Result = device->GetStreamSource(
			1, &stream1, &stream1Offset, &stream1Stride);
		const HRESULT indexResult = device->GetIndices(&indices);
		const HRESULT declarationResult =
			device->GetVertexDeclaration(&declaration);
		const HRESULT vertexShaderResult =
			device->GetVertexShader(&vertexShader);
		const HRESULT pixelShaderResult =
			device->GetPixelShader(&pixelShader);

		float actualWvp[16] = {};
		float actualTile[4] = {};
		const HRESULT wvpResult =
			device->GetVertexShaderConstantF(0, actualWvp, 4);
		const HRESULT tileResult =
			device->GetPixelShaderConstantF(0, actualTile, 1);
		NativeTileInstancingSnapshot expectedSnapshot;
		const NativeTileInstancingSnapshotResult snapshotResult =
			BuildNativeTileInstancingSnapshot(
				geometry, properties, renderer, expectedSnapshot);
		const bool snapshotReady = snapshotResult
			== NativeTileInstancingSnapshotResult::Ready;
		// D3DXMATRIX does not value-initialize its float storage. A failed
		// snapshot deliberately leaves the matrix unavailable, so keep logging
		// from inspecting indeterminate bytes while reporting that failure.
		const std::array<float, 16> unavailableExpectedWvp = {};
		const float* expectedWvp = snapshotReady
			? &expectedSnapshot.wvpColumns._11
			: unavailableExpectedWvp.data();
		const bool wvpMatch = snapshotReady && SUCCEEDED(wvpResult)
			&& std::memcmp(actualWvp, expectedWvp,
				sizeof(actualWvp)) == 0;
		const bool tileMatch = snapshotReady && SUCCEEDED(tileResult)
			&& std::memcmp(actualTile, expectedSnapshot.tileColor.data(),
				sizeof(actualTile)) == 0;
		UInt32 wvpMismatchMask = 0;
		UInt32 actualWvpFiniteMask = 0;
		UInt32 expectedWvpFiniteMask = 0;
		float wvpMaximumAbsoluteDelta = 0.0f;
		for (UInt32 index = 0; index < 16; ++index)
		{
			const bool actualFinite = std::isfinite(actualWvp[index]);
			const bool expectedFinite = std::isfinite(expectedWvp[index]);
			if (actualFinite)
				actualWvpFiniteMask |= 1u << index;
			if (expectedFinite)
				expectedWvpFiniteMask |= 1u << index;
			if (std::memcmp(&actualWvp[index], &expectedWvp[index],
				sizeof(float)) != 0)
			{
				wvpMismatchMask |= 1u << index;
			}
			if (actualFinite && expectedFinite)
			{
				wvpMaximumAbsoluteDelta = std::max(
					wvpMaximumAbsoluteDelta,
					std::fabs(actualWvp[index] - expectedWvp[index]));
			}
		}
		NativeA8RingPacketDiagnostic ringDiagnostic;
		const bool ringInspected = InspectNativeA8RingPacketForDiagnostic(
			command, ringDiagnostic);
		const NativeA8FramePacketBinding& expected = command.binding;
		const bool frequencyReady = SUCCEEDED(frequency0Result)
			&& SUCCEEDED(frequency1Result)
			&& frequency0 == 1u && frequency1 == 1u;
		const bool bindingMatch = SUCCEEDED(stream0Result)
			&& SUCCEEDED(indexResult) && SUCCEEDED(declarationResult)
			&& stream0 == expected.vertexBuffer
			&& stream0Offset == 0u
			&& stream0Stride == sizeof(NativeA8GpuVertex)
			&& indices == expected.indexBuffer
			&& declaration == expected.declaration;
		const bool shaderMatch = SUCCEEDED(vertexShaderResult)
			&& SUCCEEDED(pixelShaderResult)
			&& vertexShader == program.vertexShader
			&& pixelShader == program.pixelShader;
		NiTriShapeData* data = geometry ? geometry->GetModelData() : nullptr;
		const UInt32 vertexCount = expected.vertexCount;
		const UInt32 triangleCount = vertexCount / 4u * 2u;
		const NiTransform* world = geometry ? &geometry->m_kWorld : nullptr;

		FreeTypeFontDebugLog(
			"tnvse_freetype_native_draw_diag: id=%u stage=%s crossText=%u fallback=%u cached=%u geometry=%p data=%p buffer=%p vertices=%u base=%u triangles=%u dirty=0x%04X worldScale=%.9g worldT=(%.9g,%.9g,%.9g) freq=(%u,%u) freqHr=(0x%08X,0x%08X) freqReady=%u stream0=%p/%u/%u expected0=%p/0/%u stream0Hr=0x%08X stream1=%p/%u/%u stream1Hr=0x%08X ib=%p expectedIb=%p ibHr=0x%08X decl=%p expectedDecl=%p declHr=0x%08X bindingMatch=%u vs=%p expectedVs=%p vsHr=0x%08X ps=%p expectedPs=%p psHr=0x%08X shaderMatch=%u snapshot=%u wvpHr=0x%08X wvpMatch=%u wvpHash=%016llX expectedWvpHash=%016llX wvp0=(%.9g,%.9g,%.9g,%.9g) expected0=(%.9g,%.9g,%.9g,%.9g) wvp3=(%.9g,%.9g,%.9g,%.9g) expected3=(%.9g,%.9g,%.9g,%.9g) tileHr=0x%08X tileMatch=%u tile=(%.9g,%.9g,%.9g,%.9g) expectedTile=(%.9g,%.9g,%.9g,%.9g)",
			diagnosticId, DiagnosticStageName(stage),
			g_bEnableFreeTypeFontCrossTextBatch ? 1u : 0u,
			directFallback, cachedBindingReady ? 1u : 0u,
			geometry, data, buffer, vertexCount, expected.baseVertex,
			triangleCount, data ? data->m_usDirtyFlags : 0u,
			world ? world->m_fScale : 0.0f,
			world ? world->m_Translate.x : 0.0f,
			world ? world->m_Translate.y : 0.0f,
			world ? world->m_Translate.z : 0.0f,
			frequency0, frequency1,
			static_cast<UInt32>(frequency0Result),
			static_cast<UInt32>(frequency1Result),
			frequencyReady ? 1u : 0u,
			stream0, stream0Offset, stream0Stride,
			expected.vertexBuffer,
			static_cast<UInt32>(sizeof(NativeA8GpuVertex)),
			static_cast<UInt32>(stream0Result),
			stream1, stream1Offset, stream1Stride,
			static_cast<UInt32>(stream1Result),
			indices, expected.indexBuffer,
			static_cast<UInt32>(indexResult),
			declaration, expected.declaration,
			static_cast<UInt32>(declarationResult),
			bindingMatch ? 1u : 0u,
			vertexShader, program.vertexShader,
			static_cast<UInt32>(vertexShaderResult),
			pixelShader, program.pixelShader,
			static_cast<UInt32>(pixelShaderResult),
			shaderMatch ? 1u : 0u,
			static_cast<UInt32>(snapshotResult),
			static_cast<UInt32>(wvpResult), wvpMatch ? 1u : 0u,
			static_cast<unsigned long long>(
				HashBytes(actualWvp, sizeof(actualWvp))),
			static_cast<unsigned long long>(
				HashBytes(expectedWvp, sizeof(actualWvp))),
			actualWvp[0], actualWvp[1], actualWvp[2], actualWvp[3],
			expectedWvp[0], expectedWvp[1], expectedWvp[2], expectedWvp[3],
			actualWvp[12], actualWvp[13], actualWvp[14], actualWvp[15],
			expectedWvp[12], expectedWvp[13], expectedWvp[14], expectedWvp[15],
			static_cast<UInt32>(tileResult), tileMatch ? 1u : 0u,
			actualTile[0], actualTile[1], actualTile[2], actualTile[3],
			expectedSnapshot.tileColor[0], expectedSnapshot.tileColor[1],
			expectedSnapshot.tileColor[2], expectedSnapshot.tileColor[3]);

		FreeTypeFontDebugLog(
			"tnvse_freetype_native_command_diag: id=%u stage=%s commandBuffer=%u crossText=%u currentPass=%u firstPass=%u span=%u offset=%u kind=%u packetIndex=%u relation=%u constantsAction=%s constantsLiteResult=%u translationLiteResult=%u cleanup=%u passCached=%u constantsKey=%u source=%p expectedGeometry=%p payload=%p artifact=%p packet=%p bindGeneration=%u bindResource=%u bindEpoch=%u bindStatic=%u bindActive=%u wvpMismatchMask=0x%04X actualFiniteMask=0x%04X expectedFiniteMask=0x%04X wvpMaxAbsDelta=%.9g viewHash=%016llX projectionHash=%016llX",
			diagnosticId, DiagnosticStageName(stage),
			g_bEnableFreeTypeFontCommandBuffer ? 1u : 0u,
			g_bEnableFreeTypeFontCrossTextBatch ? 1u : 0u,
			context.currentPass, context.firstPass ? 1u : 0u,
			context.commandSpanIndex, context.commandOffset,
			context.commandKind, command.packetIndex,
			context.constantsRelation,
			ConstantsActionName(context.constantsAction),
			context.constantsLiteResult, context.translationLiteResult,
			context.cleanupRequired ? 1u : 0u,
			context.passStateReady ? 1u : 0u,
			context.constantsKeyReady ? 1u : 0u,
			command.sourceGeometry, command.expectedGeometry,
			command.payload, ringDiagnostic.artifact,
			ringDiagnostic.packet, expected.generation,
			expected.resourceSerial, expected.uploadEpoch,
			expected.staticResident ? 1u : 0u,
			expected.active ? 1u : 0u,
			wvpMismatchMask, actualWvpFiniteMask,
			expectedWvpFiniteMask, wvpMaximumAbsoluteDelta,
			static_cast<unsigned long long>(HashBytes(
				&renderer->m_kD3DView, sizeof(renderer->m_kD3DView))),
			static_cast<unsigned long long>(HashBytes(
				&renderer->m_kD3DProj, sizeof(renderer->m_kD3DProj))));

		FreeTypeFontDebugLog(
			"tnvse_freetype_native_ring_diag: id=%u stage=%s inspected=%u artifact=%p packet=%p cpuPayloadHash=%016llX cpuPacketHash=%016llX payloadVertices=%u packetFirst=%u packetVertices=%u commandBase=%u commandEnd=%u expectedPayloadBase=%u static=%u leaseActive=%u bindingCurrent=%u state=(generation=%u,resource=%u,epoch=%u,next=%u,capacity=%u,write=%u,discard=%u) lease=(generation=%u,resource=%u,epoch=%u) record=(found=%u,owner=%u,base=%u,vertices=%u,epoch=%u,write=%u,discard=%u,hash=%016llX) proofs=(recordRange=%u,packetIdentity=%u,packetRange=%u,published=%u,hashRecorded=%u,hashMatch=%u,indexRange=%u)",
			diagnosticId, DiagnosticStageName(stage),
			ringInspected ? 1u : 0u, ringDiagnostic.artifact,
			ringDiagnostic.packet,
			static_cast<unsigned long long>(
				ringDiagnostic.cpuPayloadHash),
			static_cast<unsigned long long>(
				ringDiagnostic.cpuPacketHash),
			ringDiagnostic.payloadVertexCount,
			ringDiagnostic.packetFirstVertex,
			ringDiagnostic.packetVertexCount,
			expected.baseVertex,
			ringDiagnostic.expectedPacketEndVertex,
			ringDiagnostic.expectedPayloadBaseVertex,
			ringDiagnostic.staticResident ? 1u : 0u,
			ringDiagnostic.leaseActive ? 1u : 0u,
			ringDiagnostic.bindingCurrent ? 1u : 0u,
			ringDiagnostic.stateGeneration,
			ringDiagnostic.stateResourceSerial,
			ringDiagnostic.stateUploadEpoch,
			ringDiagnostic.stateNextVertex,
			ringDiagnostic.stateVertexCapacity,
			ringDiagnostic.stateWriteSerial,
			ringDiagnostic.stateDiscardSerial,
			ringDiagnostic.leaseGeneration,
			ringDiagnostic.leaseResourceSerial,
			ringDiagnostic.leaseUploadEpoch,
			ringDiagnostic.residencyFound ? 1u : 0u,
			ringDiagnostic.ownerMatch ? 1u : 0u,
			ringDiagnostic.recordBaseVertex,
			ringDiagnostic.recordVertexCount,
			ringDiagnostic.recordUploadEpoch,
			ringDiagnostic.recordWriteSerial,
			ringDiagnostic.recordDiscardSerial,
			static_cast<unsigned long long>(
				ringDiagnostic.recordedPayloadHash),
			ringDiagnostic.recordRangeMatch ? 1u : 0u,
			ringDiagnostic.packetIdentityMatch ? 1u : 0u,
			ringDiagnostic.packetRangeMatch ? 1u : 0u,
			ringDiagnostic.rangePublished ? 1u : 0u,
			ringDiagnostic.hashRecorded ? 1u : 0u,
			ringDiagnostic.hashMatch ? 1u : 0u,
			ringDiagnostic.canonicalIndexRangeReady ? 1u : 0u);

		ReleaseDiagnosticReference(stream0);
		ReleaseDiagnosticReference(stream1);
		ReleaseDiagnosticReference(indices);
		ReleaseDiagnosticReference(declaration);
		ReleaseDiagnosticReference(vertexShader);
		ReleaseDiagnosticReference(pixelShader);
	}

	void LogNativeA8DirectDrawSubmissionDiagnostic(UInt32 diagnosticId,
		IDirect3DDevice9* device, UInt32 baseVertex, UInt32 vertexCount,
		UInt32 triangleCount, bool bindingWasReused,
		HRESULT streamResult, HRESULT indexResult, HRESULT drawResult)
	{
		if (!diagnosticId || !g_bEnableFreeTypeFontRenderingLog)
			return;
		FreeTypeFontDebugLog(
			"tnvse_freetype_native_dip_diag: id=%u device=%p primitive=%u base=%u minVertex=0 vertices=%u startIndex=0 triangles=%u bindingReused=%u streamHr=0x%08X indexHr=0x%08X drawHr=0x%08X",
			diagnosticId, device,
			static_cast<UInt32>(D3DPT_TRIANGLELIST), baseVertex,
			vertexCount, triangleCount, bindingWasReused ? 1u : 0u,
			static_cast<UInt32>(streamResult),
			static_cast<UInt32>(indexResult),
			static_cast<UInt32>(drawResult));
	}
}
