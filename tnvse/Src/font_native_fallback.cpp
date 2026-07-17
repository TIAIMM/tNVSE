#include "font_native_internal.h"

#include "font_a8_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "TESMain.hpp"

#include <atomic>
#include <Windows.h>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kMaximumSuppressionLogs = 64;
		std::atomic<UInt32> s_suppressionLogCount = 0;
		std::atomic<UInt64> s_runtimeFaultCount = 0;

		NativeA8FallbackReason NormalizeFailureReason(
			NativeA8FallbackReason reason)
		{
			return reason == NativeA8FallbackReason::None
				? NativeA8FallbackReason::RuntimeFault : reason;
		}
	}

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason)
	{
		switch (reason)
		{
		case NativeA8FallbackReason::None:
			return "none";
		case NativeA8FallbackReason::ShaderGeneration:
			return "shader-generation";
		case NativeA8FallbackReason::PacketBuild:
			return "packet-build";
		case NativeA8FallbackReason::PacketPrepare:
			return "packet-prepare";
		case NativeA8FallbackReason::PacketPending:
			return "packet-pending";
		case NativeA8FallbackReason::AtlasGeneration:
			return "atlas-generation";
		case NativeA8FallbackReason::PageTexture:
			return "page-texture";
		case NativeA8FallbackReason::PropertySync:
			return "property-sync";
		case NativeA8FallbackReason::AccumulatorConflict:
			return "accumulator-conflict";
		case NativeA8FallbackReason::TileRouteConflict:
			return "tile-route-conflict";
		case NativeA8FallbackReason::DirectImmediate:
			return "direct-immediate";
		case NativeA8FallbackReason::DeviceReset:
			return "device-reset";
		case NativeA8FallbackReason::RuntimeFault:
			return "runtime-fault";
		default:
			return "unknown";
		}
	}

	const char* NativeA8PacketPrepareFailureName(
		NativeA8PacketPrepareFailure failure)
	{
		switch (failure)
		{
		case NativeA8PacketPrepareFailure::None:
			return "none";
		case NativeA8PacketPrepareFailure::Generation:
			return "generation";
		case NativeA8PacketPrepareFailure::Profile:
			return "profile";
		case NativeA8PacketPrepareFailure::Geometry:
			return "geometry";
		case NativeA8PacketPrepareFailure::Purge:
			return "purge";
		case NativeA8PacketPrepareFailure::ShaderSetup:
			return "shader-setup";
		case NativeA8PacketPrepareFailure::ShaderBinding:
			return "shader-binding";
		case NativeA8PacketPrepareFailure::Precache:
			return "precache";
		case NativeA8PacketPrepareFailure::RendererBuffer:
			return "renderer-buffer";
		case NativeA8PacketPrepareFailure::Declaration:
			return "declaration";
		case NativeA8PacketPrepareFailure::StreamCount:
			return "stream-count";
		case NativeA8PacketPrepareFailure::VertexStride:
			return "vertex-stride";
		case NativeA8PacketPrepareFailure::VertexCount:
			return "vertex-count";
		case NativeA8PacketPrepareFailure::IndexCount:
			return "index-count";
		case NativeA8PacketPrepareFailure::IndexBuffer:
			return "index-buffer";
		case NativeA8PacketPrepareFailure::VertexBuffer:
			return "vertex-buffer";
		default:
			return "unknown";
		}
	}

	const char* NativeA8PacketPendingStageName(
		NativeA8PacketPendingStage stage)
	{
		switch (stage)
		{
		case NativeA8PacketPendingStage::None:
			return "none";
		case NativeA8PacketPendingStage::ExternalQueue:
			return "external-queue";
		case NativeA8PacketPendingStage::RendererPacking:
			return "renderer-packing";
		default:
			return "unknown";
		}
	}

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const UInt32 ordinal = s_suppressionLogCount.fetch_add(
			1, std::memory_order_relaxed);
		if (ordinal > kMaximumSuppressionLogs)
			return;
		if (ordinal == kMaximumSuppressionLogs)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: native submission suppression logs capped at %u entries",
				kMaximumSuppressionLogs);
			return;
		}

		reason = NormalizeFailureReason(reason);
		NativeA8PacketPrepareFailure packetFailure =
			NativeA8PacketPrepareFailure::None;
		NativeA8PacketPendingStage pendingStage = NativeA8PacketPendingStage::None;
		UInt32 blockedGeneration = 0;
		bool sticky = false;
		if (metadata.nativePayload)
		{
			packetFailure = metadata.nativePayload->packetPrepareFailure.load(
				std::memory_order_relaxed);
			pendingStage = metadata.nativePayload->packetPendingStage.load(
				std::memory_order_acquire);
			blockedGeneration = metadata.nativePayload->blockedGeneration.load(
				std::memory_order_acquire);
			sticky = blockedGeneration != 0
				&& blockedGeneration == GetNativeA8ShaderGeneration();
		}
		TESMain* main = TESMain::GetSingleton();
		const UInt32 threadId = GetCurrentThreadId();
		const UInt32 mainThreadId = main ? main->uiMainThreadID : 0;
		gLog.FormattedMessage(
			"tnvse_freetype_native: submission-suppressed reason=%s phase=%s packetStage=%s thread=%u mainThread=%u isMain=%u shape=%p font=%u generation=%u blockedGeneration=%u pages=%u ranges=%u quads=%u glyphs=%u sticky=%u",
			NativeA8FallbackReasonName(reason), phase ? phase : "unknown",
			reason == NativeA8FallbackReason::PacketPending
				? NativeA8PacketPendingStageName(pendingStage)
				: NativeA8PacketPrepareFailureName(packetFailure),
			threadId, mainThreadId,
			mainThreadId && threadId == mainThreadId ? 1 : 0,
			shape, metadata.fontId, GetNativeA8ShaderGeneration(),
			blockedGeneration,
			static_cast<UInt32>(metadata.effects.atlasTextures.size()),
			static_cast<UInt32>(metadata.compiledRanges.size()), metadata.quadCount,
			metadata.glyphCount, sticky ? 1 : 0);
	}

	void MarkNativeA8RuntimeFault(NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason)
	{
		reason = NormalizeFailureReason(reason);
		NativeA8FallbackReason expected = NativeA8FallbackReason::None;
		payload.stickyReason.compare_exchange_strong(expected, reason,
			std::memory_order_relaxed, std::memory_order_relaxed);
		const bool alreadyMarked = payload.suppressNextSubmit.exchange(true,
			std::memory_order_release);
		if (alreadyMarked)
			return;

		const UInt64 faultId = s_runtimeFaultCount.fetch_add(
			1, std::memory_order_relaxed) + 1;
		gLog.FormattedMessage(
			"tnvse_freetype_native: runtime-fault fault=%llu reason=%s font=%u preparedGeneration=%u currentGeneration=%u pages=%u packets=%u quads=%u action=suppress-next-submit",
			static_cast<unsigned long long>(faultId),
			NativeA8FallbackReasonName(payload.stickyReason.load(
				std::memory_order_relaxed)), payload.fontId, payload.preparedGeneration,
			GetNativeA8ShaderGeneration(), payload.pageCount,
			static_cast<UInt32>(payload.packets.size()), payload.quadCount);
	}
}
