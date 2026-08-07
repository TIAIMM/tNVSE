#include "font_native_internal.h"

#include "font_native_shape_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "TESMain.hpp"

#include <atomic>
#include <Windows.h>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_fallback {}
	using namespace implementation::font_native_fallback;

	namespace implementation::font_native_fallback
	{
		constexpr UInt32 kMaximumSuppressionLogs = 64;
		std::atomic<UInt32> s_suppressionLogCount = 0;
		std::atomic<UInt64> s_runtimeFaultCount = 0;

		NativeFontFallbackReason NormalizeFailureReason(
			NativeFontFallbackReason reason)
		{
			return reason == NativeFontFallbackReason::None
				? NativeFontFallbackReason::RuntimeFault : reason;
		}
	}

	const char* NativeFontFallbackReasonName(NativeFontFallbackReason reason)
	{
		switch (reason)
		{
		case NativeFontFallbackReason::None:
			return "none";
		case NativeFontFallbackReason::ShaderGeneration:
			return "shader-generation";
		case NativeFontFallbackReason::PacketBuild:
			return "packet-build";
		case NativeFontFallbackReason::PacketPrepare:
			return "packet-prepare";
		case NativeFontFallbackReason::AtlasGeneration:
			return "atlas-generation";
		case NativeFontFallbackReason::PageTexture:
			return "page-texture";
		case NativeFontFallbackReason::PropertySync:
			return "property-sync";
		case NativeFontFallbackReason::AccumulatorConflict:
			return "accumulator-conflict";
		case NativeFontFallbackReason::TileRouteConflict:
			return "tile-route-conflict";
		case NativeFontFallbackReason::DirectImmediate:
			return "direct-immediate";
		case NativeFontFallbackReason::DeviceReset:
			return "device-reset";
		case NativeFontFallbackReason::RuntimeFault:
			return "runtime-fault";
		default:
			return "unknown";
		}
	}

	const char* NativeFontPacketPrepareFailureName(
		NativeFontPacketPrepareFailure failure)
	{
		switch (failure)
		{
		case NativeFontPacketPrepareFailure::None:
			return "none";
		case NativeFontPacketPrepareFailure::Generation:
			return "generation";
		case NativeFontPacketPrepareFailure::Geometry:
			return "geometry";
		case NativeFontPacketPrepareFailure::ShaderBinding:
			return "shader-binding";
		case NativeFontPacketPrepareFailure::Declaration:
			return "declaration";
		case NativeFontPacketPrepareFailure::ProxyUnavailable:
			return "proxy-unavailable";
		case NativeFontPacketPrepareFailure::RingCapacity:
			return "ring-capacity";
		case NativeFontPacketPrepareFailure::IndexBuffer:
			return "index-buffer";
		case NativeFontPacketPrepareFailure::VertexBuffer:
			return "vertex-buffer";
		default:
			return "unknown";
		}
	}

	void RecordNativeFontSuppression(NiTriShape* shape,
		const NativeFontShapeMetadata& metadata, NativeFontFallbackReason reason,
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
		NativeFontPacketPrepareFailure packetFailure =
			NativeFontPacketPrepareFailure::None;
		if (metadata.nativePayload.buildComplete)
		{
			packetFailure = metadata.nativePayload.packetPrepareFailure.load(
				std::memory_order_relaxed);
		}
		const NativeFontPayloadTemplate* artifact =
			metadata.nativePayload.payloadTemplate.get();
		TESMain* main = TESMain::GetSingleton();
		const UInt32 threadId = GetCurrentThreadId();
		const UInt32 mainThreadId = main ? main->uiMainThreadID : 0;
		gLog.FormattedMessage(
			"tnvse_freetype_native: submission-suppressed reason=%s phase=%s packetFailure=%s thread=%u mainThread=%u isMain=%u shape=%p font=%u generation=%u pages=%u ranges=%u quads=%u glyphs=%u",
			NativeFontFallbackReasonName(reason), phase ? phase : "unknown",
			NativeFontPacketPrepareFailureName(packetFailure),
			threadId, mainThreadId,
			mainThreadId && threadId == mainThreadId ? 1 : 0,
			shape, metadata.fontId, GetNativeFontShaderGeneration(),
			artifact ? artifact->pageCount : 0u,
			artifact ? artifact->sourceRangeCount : 0u, metadata.quadCount,
			metadata.glyphCount);
	}

	void MarkNativeFontRuntimeFault(const NativeFontShapeMetadata& metadata,
		NativeFontShapePayload& payload,
		NativeFontFallbackReason reason)
	{
		reason = NormalizeFailureReason(reason);
		NativeFontFallbackReason expected = NativeFontFallbackReason::None;
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
			NativeFontFallbackReasonName(payload.stickyReason.load(
				std::memory_order_relaxed)), metadata.fontId,
			payload.preparedGeneration, GetNativeFontShaderGeneration(),
			payload.payloadTemplate ? payload.payloadTemplate->pageCount : 0u,
			static_cast<UInt32>(payload.packetShaders.size()), metadata.quadCount);
	}
}
