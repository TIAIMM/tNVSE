#include "font_native_internal.h"

#include "font_a8_internal.h"
#include "tnvse.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <Windows.h>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kMaximumTrackedFallbackShapes = 512;
		constexpr UInt32 kTransitionLogWindowMilliseconds = 5000;
		constexpr UInt32 kMaximumTransitionLogsPerWindow = 64;

		struct ShapeFallbackEpisode
		{
			UInt64 episode = 0;
			UInt32 fontId = 0;
			UInt32 fallbackCount = 0;
			UInt32 suppressedRepeatCount = 0;
			NativeA8FallbackReason reason = NativeA8FallbackReason::None;
			NativeA8PacketPrepareFailure packetFailure =
				NativeA8PacketPrepareFailure::None;
			UInt32 blockedGeneration = 0;
			bool bridgeReady = false;
			bool sticky = false;
		};

		struct FallbackLogState
		{
			std::mutex mutex;
			std::unordered_map<const NiTriShape*, ShapeFallbackEpisode> episodes;
			std::deque<const NiTriShape*> episodeOrder;
			std::atomic<UInt32> pendingEpisodes = 0;
			UInt64 nextEpisode = 0;
			UInt64 transitionCount = 0;
			UInt64 runtimeFaultCount = 0;
			DWORD logWindowStart = 0;
			UInt32 emittedInWindow = 0;
			UInt32 suppressedInWindow = 0;
		};

		FallbackLogState& FallbackLogs()
		{
			static FallbackLogState state;
			return state;
		}

		NativeA8FallbackReason NormalizeFallbackReason(
			NativeA8FallbackReason reason)
		{
			return reason == NativeA8FallbackReason::None
				? NativeA8FallbackReason::RuntimeFault : reason;
		}

		bool ReserveTransitionLog(FallbackLogState& state,
			UInt32& suppressedTransitions)
		{
			const DWORD now = GetTickCount();
			if (!state.logWindowStart
				|| now - state.logWindowStart >= kTransitionLogWindowMilliseconds)
			{
				state.logWindowStart = now;
				state.emittedInWindow = 0;
			}

			if (state.emittedInWindow >= kMaximumTransitionLogsPerWindow)
			{
				++state.suppressedInWindow;
				return false;
			}

			++state.emittedInWindow;
			suppressedTransitions = state.suppressedInWindow;
			state.suppressedInWindow = 0;
			return true;
		}

		void RemoveTrackedShape(FallbackLogState& state, const NiTriShape* shape)
		{
			if (state.episodes.erase(shape))
				state.pendingEpisodes.fetch_sub(1, std::memory_order_release);
			state.episodeOrder.erase(std::remove(state.episodeOrder.begin(),
				state.episodeOrder.end(), shape), state.episodeOrder.end());
		}

		ShapeFallbackEpisode& StartFallbackEpisode(FallbackLogState& state,
			const NiTriShape* shape, UInt32 fontId)
		{
			while (state.episodes.size() >= kMaximumTrackedFallbackShapes
				&& !state.episodeOrder.empty())
			{
				const NiTriShape* oldest = state.episodeOrder.front();
				state.episodeOrder.pop_front();
				if (state.episodes.erase(oldest))
					state.pendingEpisodes.fetch_sub(1, std::memory_order_release);
			}

			ShapeFallbackEpisode episode;
			episode.episode = ++state.nextEpisode;
			episode.fontId = fontId;
			state.episodeOrder.push_back(shape);
			auto inserted = state.episodes.emplace(shape, episode);
			if (inserted.second)
				state.pendingEpisodes.fetch_add(1, std::memory_order_release);
			return inserted.first->second;
		}
	}

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason)
	{
		switch (reason)
		{
		case NativeA8FallbackReason::None:
			return "none";
		case NativeA8FallbackReason::NativeInitialization:
			return "native-initialization";
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
		case NativeA8FallbackReason::ShaderRefresh:
			return "shader-refresh";
		case NativeA8FallbackReason::RuntimeFault:
			return "runtime-fault";
		case NativeA8FallbackReason::BridgeUnavailable:
			return "bridge-unavailable";
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

	bool EnsureA8BridgeFallbackReady()
	{
		// This is the sole bridge activation point for native rendering.  The
		// existing function first accepts an already-published bridge and otherwise
		// follows the audited Tile/renderer/DIP installation path.
		return InitializeA8BridgeFallback(true);
	}

	void RecordNativeA8Fallback(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		bool bridgeReady)
	{
		reason = NormalizeFallbackReason(reason);
		NativeA8PacketPrepareFailure packetFailure =
			NativeA8PacketPrepareFailure::None;
		UInt32 blockedGeneration = 0;
		bool sticky = false;
		if (metadata.nativePayload)
		{
			packetFailure = metadata.nativePayload->packetPrepareFailure.load(
				std::memory_order_relaxed);
			blockedGeneration = metadata.nativePayload->blockedGeneration.load(
				std::memory_order_acquire);
			sticky = blockedGeneration != 0
				&& blockedGeneration == GetNativeA8ShaderGeneration();
		}
		FallbackLogState& state = FallbackLogs();
		std::lock_guard<std::mutex> lock(state.mutex);

		auto found = state.episodes.find(shape);
		if (found != state.episodes.end()
			&& found->second.fontId != metadata.fontId)
		{
			// A recycled NiTriShape address must not inherit another shape's episode.
			RemoveTrackedShape(state, shape);
			found = state.episodes.end();
		}
		ShapeFallbackEpisode& episode = found != state.episodes.end()
			? found->second : StartFallbackEpisode(state, shape, metadata.fontId);
		++episode.fallbackCount;
		const bool transition = episode.fallbackCount == 1
			|| episode.reason != reason || episode.bridgeReady != bridgeReady
			|| episode.packetFailure != packetFailure
			|| episode.blockedGeneration != blockedGeneration
			|| episode.sticky != sticky;
		if (!transition)
		{
			++episode.suppressedRepeatCount;
			return;
		}

		episode.reason = reason;
		episode.packetFailure = packetFailure;
		episode.blockedGeneration = blockedGeneration;
		episode.bridgeReady = bridgeReady;
		episode.sticky = sticky;
		const UInt64 transitionId = ++state.transitionCount;
		UInt32 suppressedTransitions = 0;
		if (!ReserveTransitionLog(state, suppressedTransitions))
			return;

		gLog.FormattedMessage(
			"tnvse_freetype_native: route-transition route=bridge-fallback transition=%llu episode=%llu attempt=%u reason=%s packetStage=%s shape=%p font=%u generation=%u blockedGeneration=%u pages=%u ranges=%u quads=%u glyphs=%u bridgeReady=%u sticky=%u repeatedSuppressed=%u transitionsSuppressed=%u",
			static_cast<unsigned long long>(transitionId),
			static_cast<unsigned long long>(episode.episode),
			episode.fallbackCount, NativeA8FallbackReasonName(reason),
			reason == NativeA8FallbackReason::PacketPending ? "pending"
				: NativeA8PacketPrepareFailureName(packetFailure),
			shape, metadata.fontId, GetNativeA8ShaderGeneration(),
			blockedGeneration,
			static_cast<UInt32>(metadata.effects.atlasTextures.size()),
			static_cast<UInt32>(metadata.compiledRanges.size()), metadata.quadCount,
			metadata.glyphCount, bridgeReady ? 1 : 0, sticky ? 1 : 0,
			episode.suppressedRepeatCount, suppressedTransitions);
		episode.suppressedRepeatCount = 0;
	}

	void ForgetNativeA8FallbackShape(const NiTriShape* shape)
	{
		if (!shape)
			return;
		FallbackLogState& state = FallbackLogs();
		if (!state.pendingEpisodes.load(std::memory_order_acquire))
			return;
		std::lock_guard<std::mutex> lock(state.mutex);
		RemoveTrackedShape(state, shape);
	}

	void RecordNativeA8Recovery(NiTriShape* shape,
		const A8ShapeMetadata& metadata)
	{
		FallbackLogState& state = FallbackLogs();
		if (!state.pendingEpisodes.load(std::memory_order_acquire))
			return;
		std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.episodes.find(shape);
		if (found == state.episodes.end()
			|| found->second.fontId != metadata.fontId)
		{
			return;
		}

		const ShapeFallbackEpisode episode = found->second;
		RemoveTrackedShape(state, shape);
		const UInt64 transitionId = ++state.transitionCount;
		UInt32 suppressedTransitions = 0;
		if (!ReserveTransitionLog(state, suppressedTransitions))
			return;

		gLog.FormattedMessage(
			"tnvse_freetype_native: route-transition route=native-recovered transition=%llu episode=%llu fallbackAttempts=%u lastReason=%s lastPacketStage=%s lastBlockedGeneration=%u shape=%p font=%u generation=%u pages=%u ranges=%u quads=%u glyphs=%u repeatedSuppressed=%u transitionsSuppressed=%u",
			static_cast<unsigned long long>(transitionId),
			static_cast<unsigned long long>(episode.episode),
			episode.fallbackCount, NativeA8FallbackReasonName(episode.reason),
			episode.reason == NativeA8FallbackReason::PacketPending ? "pending"
				: NativeA8PacketPrepareFailureName(episode.packetFailure),
			episode.blockedGeneration, shape, metadata.fontId,
			GetNativeA8ShaderGeneration(),
			static_cast<UInt32>(metadata.effects.atlasTextures.size()),
			static_cast<UInt32>(metadata.compiledRanges.size()), metadata.quadCount,
			metadata.glyphCount, episode.suppressedRepeatCount,
			suppressedTransitions);
	}

	void MarkNativeA8RuntimeFault(NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason)
	{
		reason = NormalizeFallbackReason(reason);
		NativeA8FallbackReason expected = NativeA8FallbackReason::None;
		payload.stickyReason.compare_exchange_strong(expected, reason,
			std::memory_order_relaxed, std::memory_order_relaxed);
		const bool alreadyMarked = payload.bridgeNextSubmit.exchange(true,
			std::memory_order_release);
		if (alreadyMarked)
			return;

		FallbackLogState& state = FallbackLogs();
		std::lock_guard<std::mutex> lock(state.mutex);
		const UInt64 faultId = ++state.runtimeFaultCount;
		const UInt64 episode = ++state.nextEpisode;
		const UInt64 transitionId = ++state.transitionCount;
		UInt32 suppressedTransitions = 0;
		if (!ReserveTransitionLog(state, suppressedTransitions))
			return;

		gLog.FormattedMessage(
			"tnvse_freetype_native: route-transition route=bridge-next-submit transition=%llu episode=%llu fault=%llu reason=%s font=%u preparedGeneration=%u currentGeneration=%u pages=%u packets=%u quads=%u bridgeReady=unknown sticky=1 transitionsSuppressed=%u",
			static_cast<unsigned long long>(transitionId),
			static_cast<unsigned long long>(episode),
			static_cast<unsigned long long>(faultId),
			NativeA8FallbackReasonName(payload.stickyReason.load(
				std::memory_order_relaxed)), payload.fontId, payload.preparedGeneration,
			GetNativeA8ShaderGeneration(), payload.pageCount,
			static_cast<UInt32>(payload.packets.size()), payload.quadCount,
			suppressedTransitions);
	}
}
