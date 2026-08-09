#include "font_vector_internal.h"

#include "load_config.h"

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace fonthook::vectorfont
{
	namespace implementation::font_performance {}
	using namespace implementation::font_performance;

	namespace implementation::font_performance
	{
		constexpr size_t kCounterCount = static_cast<size_t>(FreeTypePerfCounter::Count);
		constexpr size_t kPhaseCount =
			static_cast<size_t>(FreeTypePerfPhase::Count);
		constexpr size_t kDurationExponentBuckets = 64;
		constexpr size_t kDurationSubBuckets = 4;
		constexpr size_t kDurationBuckets =
			kDurationExponentBuckets * kDurationSubBuckets;
		constexpr size_t kGpuQueryRingSize = 32;
		constexpr UInt32 kVanillaLayoutGpuTimingSampleRate = 16u;
		constexpr size_t kGpuVanillaLayoutDrawBucketCount = 5;

		struct PerfCounterBatchState
		{
			std::array<UInt64, kCounterCount> values = {};
			std::array<UInt16, kCounterCount> touched = {};
			size_t touchedCount = 0;
			UInt64 recordCount = 0;
			UInt64 scopeCount = 0;
			UInt32 depth = 0;
		};

		thread_local PerfCounterBatchState s_perfCounterBatch;

		enum class GpuTimingCounter : size_t
		{
			Submitted = 0,
			Completed,
			RingFull,
			Nested,
			CreateFailure,
			IssueFailure,
			ReadFailure,
			Disjoint,
			InvalidRange,
			ResetDiscarded,
			PreparedPayloadEligible,
			VanillaLayoutEligible,
			VanillaLayoutSampleAttempt,
			VanillaLayoutSampleSkipped,
			VanillaLayoutSubmitted,
			VanillaLayoutCompleted,
			Count,
		};
		constexpr size_t kGpuTimingCounterCount =
			static_cast<size_t>(GpuTimingCounter::Count);

		struct GpuEnvelopeWorkload
		{
			FreeTypeGpuEnvelopeViewport viewport;
			UInt32 immediatePasses = 0;
			UInt32 foreignPasses = 0;
			UInt32 nativeFacadePasses = 0;
			UInt32 vanillaPasses = 0;
			UInt32 vanillaCulls = 0;
			UInt32 vanillaAppCulls = 0;
			UInt32 vanillaAlphaCulls = 0;
			UInt32 vanillaClipCulls = 0;
			UInt32 vanillaScissorCulls = 0;
			UInt32 vanillaStandardLiteDraws = 0;
			UInt32 vanillaStockDraws = 0;
			UInt32 vanillaRuntimeFallbacks = 0;
			UInt64 vanillaVertices = 0;
			UInt64 vanillaTriangles = 0;
			UInt32 scissoredDraws = 0;
			UInt32 unscissoredDraws = 0;
			UInt32 invalidScissorDraws = 0;
			UInt32 viewportUnavailableDraws = 0;
			UInt64 effectiveClipRectPixels = 0;
			UInt64 viewportOpportunityPixels = 0;
		};

		struct GpuVanillaLayoutDrawBucket
		{
			UInt64 samples = 0;
			UInt64 nanoseconds = 0;
			UInt64 maximumNanoseconds = 0;
		};

		struct GpuVanillaLayoutWorkloadAggregate
		{
			UInt64 samples = 0;
			UInt64 immediatePasses = 0;
			UInt64 foreignPasses = 0;
			UInt64 nativeFacadePasses = 0;
			UInt64 vanillaPasses = 0;
			UInt64 vanillaCulls = 0;
			UInt64 vanillaAppCulls = 0;
			UInt64 vanillaAlphaCulls = 0;
			UInt64 vanillaClipCulls = 0;
			UInt64 vanillaScissorCulls = 0;
			UInt64 vanillaStandardLiteDraws = 0;
			UInt64 vanillaStockDraws = 0;
			UInt64 vanillaRuntimeFallbacks = 0;
			UInt64 vanillaVertices = 0;
			UInt64 vanillaTriangles = 0;
			UInt64 scissoredDraws = 0;
			UInt64 unscissoredDraws = 0;
			UInt64 invalidScissorDraws = 0;
			UInt64 viewportUnavailableDraws = 0;
			UInt64 effectiveClipRectPixels = 0;
			UInt64 viewportOpportunityPixels = 0;
			std::array<GpuVanillaLayoutDrawBucket,
				kGpuVanillaLayoutDrawBucketCount> drawBuckets = {};
			UInt64 worstNanoseconds = 0;
			GpuEnvelopeWorkload worstWorkload;
		};

		struct AccumulatorPrepTailWorst
		{
			UInt64 nanoseconds = 0;
			UInt64 resetNanoseconds = 0;
			UInt64 topologyNanoseconds = 0;
			UInt64 visibilityNanoseconds = 0;
			UInt64 metadataNanoseconds = 0;
			UInt64 readinessNanoseconds = 0;
			UInt64 lookupNanoseconds = 0;
			UInt64 facadeLoopNanoseconds = 0;
			UInt64 ringNanoseconds = 0;
			UInt64 singletonNanoseconds = 0;
			UInt64 commandNanoseconds = 0;
			UInt64 publishNanoseconds = 0;
			UInt32 itemCount = 0;
			UInt32 facadeCount = 0;
			UInt32 survivorCount = 0;
			UInt32 payloadCount = 0;
			UInt32 singletonCount = 0;
			bool commandFrameActive = false;
		};

		struct PerformanceState
		{
			std::array<std::atomic<UInt64>, kCounterCount> counters = {};
			std::array<std::array<std::atomic<UInt64>,
				kDurationBuckets>, kPhaseCount> durations = {};
			std::array<std::atomic<UInt64>, kPhaseCount>
				durationNanoseconds = {};
			std::array<std::atomic<UInt64>, kPhaseCount>
				durationMaximumNanoseconds = {};
			std::array<std::atomic<UInt64>, kDurationBuckets>
				gpuAlphaEnvelopeDurations = {};
			std::atomic<UInt64> gpuAlphaEnvelopeNanoseconds = 0;
			std::atomic<UInt64> gpuAlphaEnvelopeMaximumNanoseconds = 0;
			std::array<std::atomic<UInt64>, kDurationBuckets>
				gpuVanillaLayoutEnvelopeDurations = {};
			std::atomic<UInt64> gpuVanillaLayoutEnvelopeNanoseconds = 0;
			std::atomic<UInt64>
				gpuVanillaLayoutEnvelopeMaximumNanoseconds = 0;
			std::array<std::atomic<UInt64>, kGpuTimingCounterCount>
				gpuTimingCounters = {};
			std::atomic<UInt64> gpuQueriesInFlight = 0;
			std::atomic<UInt64> gpuVanillaLayoutQueriesInFlight = 0;
			std::mutex gpuVanillaLayoutWorkloadMutex;
			GpuVanillaLayoutWorkloadAggregate
				gpuVanillaLayoutWorkload;
			std::array<std::atomic<UInt64>, 4> accumulatorPrepTailCounts = {};
			std::mutex accumulatorPrepTailMutex;
			AccumulatorPrepTailWorst accumulatorPrepTailWorst;
			ULONGLONG lastReport = 0;
		};

		PerformanceState& GetPerformanceState()
		{
			static PerformanceState* state =
				new PerformanceState();
			return *state;
		}

		SInt64 QueryPerfFrequency()
		{
			static const SInt64 frequency = []()
			{
				LARGE_INTEGER value = {};
				return QueryPerformanceFrequency(&value)
					? value.QuadPart : 0;
			}();
			return frequency;
		}

		void RecordDuration(FreeTypePerfPhase phase, SInt64 ticks)
		{
			const SInt64 frequency = QueryPerfFrequency();
			if (ticks <= 0 || frequency <= 0)
				return;
			const long double scaled =
				static_cast<long double>(ticks)
				* 1000000000.0L
				/ static_cast<long double>(frequency);
			const UInt64 nanoseconds = static_cast<UInt64>(
				std::max<long double>(1.0L, scaled));
			size_t exponent = 0;
			UInt64 upper = 1;
			while (upper < nanoseconds
				&& exponent + 1 < kDurationExponentBuckets)
			{
				upper = upper
					<= std::numeric_limits<UInt64>::max() / 2
						? upper * 2
						: std::numeric_limits<UInt64>::max();
				++exponent;
			}
			size_t subBucket = kDurationSubBuckets - 1u;
			if (exponent >= 3 && upper != std::numeric_limits<UInt64>::max())
			{
				const UInt64 lower = upper / 2u;
				const UInt64 step = (upper - lower) / kDurationSubBuckets;
				subBucket = static_cast<size_t>(std::min<UInt64>(
					kDurationSubBuckets - 1u,
					(nanoseconds - lower - 1u) / step));
			}
			const size_t bucket = exponent * kDurationSubBuckets + subBucket;
			PerformanceState& state = GetPerformanceState();
			state.durations[
				static_cast<size_t>(phase)][bucket]
				.fetch_add(1, std::memory_order_relaxed);
			state.durationNanoseconds[
				static_cast<size_t>(phase)].fetch_add(
					nanoseconds, std::memory_order_relaxed);
			std::atomic<UInt64>& maximum = state.durationMaximumNanoseconds[
				static_cast<size_t>(phase)];
			UInt64 current = maximum.load(std::memory_order_relaxed);
			while (current < nanoseconds
				&& !maximum.compare_exchange_weak(current, nanoseconds,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
			{
			}
		}

		size_t GpuDurationBucketForNanoseconds(UInt64 nanoseconds)
		{
			size_t exponent = 0;
			UInt64 upper = 1;
			while (upper < nanoseconds
				&& exponent + 1 < kDurationExponentBuckets)
			{
				upper = upper
					<= std::numeric_limits<UInt64>::max() / 2
						? upper * 2
						: std::numeric_limits<UInt64>::max();
				++exponent;
			}
			size_t subBucket = kDurationSubBuckets - 1u;
			if (exponent >= 3 && upper != std::numeric_limits<UInt64>::max())
			{
				const UInt64 lower = upper / 2u;
				const UInt64 step = (upper - lower) / kDurationSubBuckets;
				subBucket = static_cast<size_t>(std::min<UInt64>(
					kDurationSubBuckets - 1u,
					(nanoseconds - lower - 1u) / step));
			}
			return exponent * kDurationSubBuckets + subBucket;
		}

		void RecordGpuTimingCounter(GpuTimingCounter counter,
			UInt64 amount = 1)
		{
			GetPerformanceState().gpuTimingCounters[
				static_cast<size_t>(counter)].fetch_add(
					amount, std::memory_order_relaxed);
		}

		void RecordGpuAlphaEnvelopeDuration(UInt64 nanoseconds)
		{
			if (!nanoseconds)
				return;
			PerformanceState& state = GetPerformanceState();
			state.gpuAlphaEnvelopeDurations[
				GpuDurationBucketForNanoseconds(nanoseconds)].fetch_add(
					1, std::memory_order_relaxed);
			state.gpuAlphaEnvelopeNanoseconds.fetch_add(
				nanoseconds, std::memory_order_relaxed);
			std::atomic<UInt64>& maximum =
				state.gpuAlphaEnvelopeMaximumNanoseconds;
			UInt64 current = maximum.load(std::memory_order_relaxed);
			while (current < nanoseconds
				&& !maximum.compare_exchange_weak(current, nanoseconds,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
			{
			}
		}

		void RecordGpuVanillaLayoutEnvelopeDuration(UInt64 nanoseconds)
		{
			if (!nanoseconds)
				return;
			PerformanceState& state = GetPerformanceState();
			state.gpuVanillaLayoutEnvelopeDurations[
				GpuDurationBucketForNanoseconds(nanoseconds)].fetch_add(
					1, std::memory_order_relaxed);
			state.gpuVanillaLayoutEnvelopeNanoseconds.fetch_add(
				nanoseconds, std::memory_order_relaxed);
			std::atomic<UInt64>& maximum =
				state.gpuVanillaLayoutEnvelopeMaximumNanoseconds;
			UInt64 current = maximum.load(std::memory_order_relaxed);
			while (current < nanoseconds
				&& !maximum.compare_exchange_weak(current, nanoseconds,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
			{
			}
		}

		size_t GpuVanillaLayoutDrawBucketFor(UInt32 draws)
		{
			if (draws <= 8u)
				return 0;
			if (draws <= 16u)
				return 1;
			if (draws <= 32u)
				return 2;
			if (draws <= 64u)
				return 3;
			return 4;
		}

		void RecordGpuVanillaLayoutEnvelopeWorkload(
			UInt64 nanoseconds, const GpuEnvelopeWorkload& workload)
		{
			PerformanceState& state = GetPerformanceState();
			std::lock_guard<std::mutex> lock(
				state.gpuVanillaLayoutWorkloadMutex);
			GpuVanillaLayoutWorkloadAggregate& aggregate =
				state.gpuVanillaLayoutWorkload;
			++aggregate.samples;
			aggregate.immediatePasses += workload.immediatePasses;
			aggregate.foreignPasses += workload.foreignPasses;
			aggregate.nativeFacadePasses += workload.nativeFacadePasses;
			aggregate.vanillaPasses += workload.vanillaPasses;
			aggregate.vanillaCulls += workload.vanillaCulls;
			aggregate.vanillaAppCulls += workload.vanillaAppCulls;
			aggregate.vanillaAlphaCulls += workload.vanillaAlphaCulls;
			aggregate.vanillaClipCulls += workload.vanillaClipCulls;
			aggregate.vanillaScissorCulls += workload.vanillaScissorCulls;
			aggregate.vanillaStandardLiteDraws +=
				workload.vanillaStandardLiteDraws;
			aggregate.vanillaStockDraws += workload.vanillaStockDraws;
			aggregate.vanillaRuntimeFallbacks +=
				workload.vanillaRuntimeFallbacks;
			aggregate.vanillaVertices += workload.vanillaVertices;
			aggregate.vanillaTriangles += workload.vanillaTriangles;
			aggregate.scissoredDraws += workload.scissoredDraws;
			aggregate.unscissoredDraws += workload.unscissoredDraws;
			aggregate.invalidScissorDraws +=
				workload.invalidScissorDraws;
			aggregate.viewportUnavailableDraws +=
				workload.viewportUnavailableDraws;
			aggregate.effectiveClipRectPixels +=
				workload.effectiveClipRectPixels;
			aggregate.viewportOpportunityPixels +=
				workload.viewportOpportunityPixels;

			const UInt32 vanillaDraws =
				workload.vanillaStandardLiteDraws
					+ workload.vanillaStockDraws;
			GpuVanillaLayoutDrawBucket& bucket = aggregate.drawBuckets[
				GpuVanillaLayoutDrawBucketFor(vanillaDraws)];
			++bucket.samples;
			bucket.nanoseconds += nanoseconds;
			bucket.maximumNanoseconds = std::max(
				bucket.maximumNanoseconds, nanoseconds);
			if (nanoseconds > aggregate.worstNanoseconds)
			{
				aggregate.worstNanoseconds = nanoseconds;
				aggregate.worstWorkload = workload;
			}
		}

		struct GpuQuerySlot
		{
			IDirect3DQuery9* disjoint = nullptr;
			IDirect3DQuery9* frequency = nullptr;
			IDirect3DQuery9* beginTimestamp = nullptr;
			IDirect3DQuery9* endTimestamp = nullptr;
			bool pending = false;
			bool includesVanillaLayout = false;
			GpuEnvelopeWorkload workload;
		};

		struct GpuQueryState
		{
			IDirect3DDevice9* device = nullptr;
			std::array<GpuQuerySlot, kGpuQueryRingSize> slots = {};
			size_t nextSlot = 0;
			size_t activeSlot = std::numeric_limits<size_t>::max();
			UInt32 vanillaLayoutSampleCursor = 0;
			bool unavailable = false;
			bool loggedReady = false;
		};

		GpuQueryState& GetGpuQueryState()
		{
			static GpuQueryState* state = new GpuQueryState();
			return *state;
		}

		GpuEnvelopeWorkload* GetActiveGpuEnvelopeWorkload()
		{
			GpuQueryState& state = GetGpuQueryState();
			if (state.activeSlot >= state.slots.size())
				return nullptr;
			GpuQuerySlot& slot = state.slots[state.activeSlot];
			return slot.includesVanillaLayout ? &slot.workload : nullptr;
		}

		void IncrementEnvelopeCounter(UInt32& value)
		{
			if (value != std::numeric_limits<UInt32>::max())
				++value;
		}

		void ReleaseGpuQuery(IDirect3DQuery9*& query)
		{
			if (query)
			{
				query->Release();
				query = nullptr;
			}
		}

		void ReleaseGpuQuerySlot(GpuQuerySlot& slot)
		{
			ReleaseGpuQuery(slot.disjoint);
			ReleaseGpuQuery(slot.frequency);
			ReleaseGpuQuery(slot.beginTimestamp);
			ReleaseGpuQuery(slot.endTimestamp);
			slot.pending = false;
			slot.includesVanillaLayout = false;
			slot.workload = {};
		}

		void ResetGpuQueryState(bool countDiscarded)
		{
			GpuQueryState& state = GetGpuQueryState();
			UInt64 discarded = 0;
			UInt64 pendingDiscarded = 0;
			UInt64 pendingVanillaLayoutDiscarded = 0;
			for (size_t index = 0; index < state.slots.size(); ++index)
			{
				GpuQuerySlot& slot = state.slots[index];
				if (slot.pending)
				{
					++pendingDiscarded;
					if (slot.includesVanillaLayout)
						++pendingVanillaLayoutDiscarded;
				}
				if (countDiscarded && (slot.pending
					|| index == state.activeSlot))
				{
					++discarded;
				}
				ReleaseGpuQuerySlot(slot);
			}
			if (discarded)
			{
				RecordGpuTimingCounter(
					GpuTimingCounter::ResetDiscarded, discarded);
			}
			if (pendingDiscarded)
			{
				GetPerformanceState().gpuQueriesInFlight.fetch_sub(
					pendingDiscarded, std::memory_order_relaxed);
			}
			if (pendingVanillaLayoutDiscarded)
			{
				GetPerformanceState().gpuVanillaLayoutQueriesInFlight.fetch_sub(
					pendingVanillaLayoutDiscarded,
					std::memory_order_relaxed);
			}
			state.device = nullptr;
			state.nextSlot = 0;
			state.activeSlot = std::numeric_limits<size_t>::max();
			state.vanillaLayoutSampleCursor = 0;
			state.unavailable = false;
			state.loggedReady = false;
		}

		bool EnsureGpuQuerySlot(IDirect3DDevice9* device,
			GpuQuerySlot& slot)
		{
			if (slot.disjoint && slot.frequency
				&& slot.beginTimestamp && slot.endTimestamp)
			{
				return true;
			}
			ReleaseGpuQuerySlot(slot);
			const HRESULT disjointResult = device->CreateQuery(
				D3DQUERYTYPE_TIMESTAMPDISJOINT, &slot.disjoint);
			const HRESULT frequencyResult = SUCCEEDED(disjointResult)
				? device->CreateQuery(
					D3DQUERYTYPE_TIMESTAMPFREQ, &slot.frequency)
				: disjointResult;
			const HRESULT beginResult = SUCCEEDED(frequencyResult)
				? device->CreateQuery(
					D3DQUERYTYPE_TIMESTAMP, &slot.beginTimestamp)
				: frequencyResult;
			const HRESULT endResult = SUCCEEDED(beginResult)
				? device->CreateQuery(
					D3DQUERYTYPE_TIMESTAMP, &slot.endTimestamp)
				: beginResult;
			if (FAILED(disjointResult) || FAILED(frequencyResult)
				|| FAILED(beginResult) || FAILED(endResult))
			{
				ReleaseGpuQuerySlot(slot);
				RecordGpuTimingCounter(GpuTimingCounter::CreateFailure);
				return false;
			}
			return true;
		}

		void PollGpuQueries()
		{
			GpuQueryState& state = GetGpuQueryState();
			for (GpuQuerySlot& slot : state.slots)
			{
				if (!slot.pending)
					continue;
				BOOL disjoint = TRUE;
				UInt64 frequency = 0;
				UInt64 beginTimestamp = 0;
				UInt64 endTimestamp = 0;
				const HRESULT disjointResult = slot.disjoint->GetData(
					&disjoint, sizeof(disjoint), 0);
				const HRESULT frequencyResult = slot.frequency->GetData(
					&frequency, sizeof(frequency), 0);
				const HRESULT beginResult = slot.beginTimestamp->GetData(
					&beginTimestamp, sizeof(beginTimestamp), 0);
				const HRESULT endResult = slot.endTimestamp->GetData(
					&endTimestamp, sizeof(endTimestamp), 0);
				if (disjointResult == S_FALSE || frequencyResult == S_FALSE
					|| beginResult == S_FALSE || endResult == S_FALSE)
				{
					continue;
				}
				if (FAILED(disjointResult) || FAILED(frequencyResult)
					|| FAILED(beginResult) || FAILED(endResult))
				{
					const bool includesVanillaLayout =
						slot.includesVanillaLayout;
					GetPerformanceState().gpuQueriesInFlight.fetch_sub(
						1, std::memory_order_relaxed);
					if (includesVanillaLayout)
					{
						GetPerformanceState().
							gpuVanillaLayoutQueriesInFlight.fetch_sub(
								1, std::memory_order_relaxed);
					}
					RecordGpuTimingCounter(GpuTimingCounter::ReadFailure);
					ReleaseGpuQuerySlot(slot);
					continue;
				}
				const bool includesVanillaLayout =
					slot.includesVanillaLayout;
				const GpuEnvelopeWorkload workload = slot.workload;
				slot.pending = false;
				slot.includesVanillaLayout = false;
				slot.workload = {};
				GetPerformanceState().gpuQueriesInFlight.fetch_sub(
					1, std::memory_order_relaxed);
				if (includesVanillaLayout)
				{
					GetPerformanceState().
						gpuVanillaLayoutQueriesInFlight.fetch_sub(
							1, std::memory_order_relaxed);
				}
				if (disjoint)
				{
					RecordGpuTimingCounter(GpuTimingCounter::Disjoint);
					continue;
				}
				if (!frequency || endTimestamp <= beginTimestamp)
				{
					RecordGpuTimingCounter(GpuTimingCounter::InvalidRange);
					continue;
				}
				const long double scaled =
					static_cast<long double>(endTimestamp - beginTimestamp)
					* 1000000000.0L
					/ static_cast<long double>(frequency);
				if (!(scaled >= 1.0L)
					|| scaled > static_cast<long double>(
						std::numeric_limits<UInt64>::max()))
				{
					RecordGpuTimingCounter(GpuTimingCounter::InvalidRange);
					continue;
				}
				RecordGpuAlphaEnvelopeDuration(
					static_cast<UInt64>(scaled));
				RecordGpuTimingCounter(GpuTimingCounter::Completed);
				if (includesVanillaLayout)
				{
					RecordGpuVanillaLayoutEnvelopeDuration(
						static_cast<UInt64>(scaled));
					RecordGpuVanillaLayoutEnvelopeWorkload(
						static_cast<UInt64>(scaled), workload);
					RecordGpuTimingCounter(
						GpuTimingCounter::VanillaLayoutCompleted);
				}
			}
		}

		bool BeginGpuAlphaEnvelope(IDirect3DDevice9* device,
			bool hasPreparedPayloads, bool hasVanillaLayout,
			const FreeTypeGpuEnvelopeViewport& viewport)
		{
			if (!g_bEnableFreeTypeFontRenderingLog
				|| (!hasPreparedPayloads && !hasVanillaLayout))
				return false;
			if (hasPreparedPayloads)
			{
				RecordGpuTimingCounter(
					GpuTimingCounter::PreparedPayloadEligible);
			}
			if (hasVanillaLayout)
			{
				RecordGpuTimingCounter(
					GpuTimingCounter::VanillaLayoutEligible);
			}
			if (!device)
				return false;
			GpuQueryState& state = GetGpuQueryState();
			if (state.device != device)
			{
				ResetGpuQueryState(true);
				state.device = device;
			}
			if (state.unavailable)
				return false;
			// Preserve the established full-rate prepared-payload diagnostic. The
			// new Vanilla-only route is sampled so four D3D9 query Issue calls do not
			// become a material part of the frame cost being measured.
			if (hasVanillaLayout && !hasPreparedPayloads)
			{
				const UInt32 sample = state.vanillaLayoutSampleCursor++;
				if (sample % kVanillaLayoutGpuTimingSampleRate != 0u)
				{
					RecordGpuTimingCounter(
						GpuTimingCounter::VanillaLayoutSampleSkipped);
					return false;
				}
			}
			if (hasVanillaLayout)
			{
				RecordGpuTimingCounter(
					GpuTimingCounter::VanillaLayoutSampleAttempt);
			}
			PollGpuQueries();
			if (state.activeSlot != std::numeric_limits<size_t>::max())
			{
				RecordGpuTimingCounter(GpuTimingCounter::Nested);
				return false;
			}
			for (size_t offset = 0; offset < state.slots.size(); ++offset)
			{
				const size_t index = (state.nextSlot + offset)
					% state.slots.size();
				GpuQuerySlot& slot = state.slots[index];
				if (slot.pending)
					continue;
				if (!EnsureGpuQuerySlot(device, slot))
				{
					state.unavailable = true;
					return false;
				}
				if (!state.loggedReady)
				{
					state.loggedReady = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_gpu_timing: enabled scope=tile_alpha_envelope tracked=prepared_payload_or_vanilla_layout async=1 flush=0 ring=%u disjoint_validation=1 vanilla_only_sample_rate=%u",
						static_cast<UInt32>(state.slots.size()),
						kVanillaLayoutGpuTimingSampleRate);
				}
				slot.includesVanillaLayout = hasVanillaLayout;
				slot.workload = {};
				slot.workload.viewport = viewport;
				const HRESULT disjointResult = slot.disjoint->Issue(
					D3DISSUE_BEGIN);
				const HRESULT frequencyResult = SUCCEEDED(disjointResult)
					? slot.frequency->Issue(D3DISSUE_END)
					: disjointResult;
				const HRESULT timestampResult = SUCCEEDED(frequencyResult)
					? slot.beginTimestamp->Issue(D3DISSUE_END)
					: frequencyResult;
				if (FAILED(disjointResult) || FAILED(frequencyResult)
					|| FAILED(timestampResult))
				{
					if (SUCCEEDED(disjointResult))
						slot.disjoint->Issue(D3DISSUE_END);
					RecordGpuTimingCounter(GpuTimingCounter::IssueFailure);
					ReleaseGpuQuerySlot(slot);
					state.unavailable = true;
					return false;
				}
				state.activeSlot = index;
				state.nextSlot = (index + 1) % state.slots.size();
				return true;
			}
			RecordGpuTimingCounter(GpuTimingCounter::RingFull);
			return false;
		}

		void EndGpuAlphaEnvelope()
		{
			GpuQueryState& state = GetGpuQueryState();
			if (state.activeSlot == std::numeric_limits<size_t>::max())
				return;
			GpuQuerySlot& slot = state.slots[state.activeSlot];
			state.activeSlot = std::numeric_limits<size_t>::max();
			const HRESULT timestampResult = slot.endTimestamp->Issue(
				D3DISSUE_END);
			const HRESULT disjointResult = slot.disjoint->Issue(D3DISSUE_END);
			if (FAILED(timestampResult) || FAILED(disjointResult))
			{
				RecordGpuTimingCounter(GpuTimingCounter::IssueFailure);
				ReleaseGpuQuerySlot(slot);
				state.unavailable = true;
				return;
			}
			slot.pending = true;
			GetPerformanceState().gpuQueriesInFlight.fetch_add(
				1, std::memory_order_relaxed);
			RecordGpuTimingCounter(GpuTimingCounter::Submitted);
			if (slot.includesVanillaLayout)
			{
				GetPerformanceState().gpuVanillaLayoutQueriesInFlight.fetch_add(
					1, std::memory_order_relaxed);
				RecordGpuTimingCounter(
					GpuTimingCounter::VanillaLayoutSubmitted);
			}
		}

		struct DurationSummary
		{
			UInt64 count = 0;
			double medianMicroseconds = 0.0;
			double p95Microseconds = 0.0;
			double p99Microseconds = 0.0;
			double meanMicroseconds = 0.0;
			double maximumMicroseconds = 0.0;
		};

		DurationSummary ConsumeDurationSummary(
			FreeTypePerfPhase phase)
		{
			std::array<UInt64, kDurationBuckets> values = {};
			DurationSummary result;
			for (size_t bucket = 0;
				bucket < values.size(); ++bucket)
			{
				values[bucket] = GetPerformanceState().durations[
					static_cast<size_t>(phase)][bucket]
						.exchange(0, std::memory_order_relaxed);
				result.count += values[bucket];
			}
			const UInt64 totalNanoseconds =
				GetPerformanceState().durationNanoseconds[
					static_cast<size_t>(phase)].exchange(
						0, std::memory_order_relaxed);
			const UInt64 maximumNanoseconds =
				GetPerformanceState().durationMaximumNanoseconds[
					static_cast<size_t>(phase)].exchange(
						0, std::memory_order_relaxed);
			if (!result.count)
				return result;
			result.meanMicroseconds = static_cast<double>(totalNanoseconds)
				/ static_cast<double>(result.count) / 1000.0;
			auto quantile = [&](UInt64 numerator,
				UInt64 denominator)
			{
				const UInt64 target = std::max<UInt64>(1,
					(result.count * numerator
						+ denominator - 1) / denominator);
				UInt64 cumulative = 0;
				for (size_t bucket = 0;
					bucket < values.size(); ++bucket)
				{
					cumulative += values[bucket];
					if (cumulative < target)
						continue;
					const size_t exponent = bucket / kDurationSubBuckets;
					const size_t subBucket = bucket % kDurationSubBuckets;
					long double upperNanoseconds = std::ldexp(
						1.0L, static_cast<int>(exponent));
					if (exponent >= 3)
					{
						const long double lowerNanoseconds =
							upperNanoseconds / 2.0L;
						const long double step =
							(upperNanoseconds - lowerNanoseconds)
							/ static_cast<long double>(kDurationSubBuckets);
						upperNanoseconds = lowerNanoseconds
							+ step * static_cast<long double>(subBucket + 1u);
					}
					return static_cast<double>(
						upperNanoseconds / 1000.0L);
				}
				return static_cast<double>(
					std::numeric_limits<UInt64>::max())
					/ 1000.0;
			};
			result.medianMicroseconds = quantile(50, 100);
			result.p95Microseconds = quantile(95, 100);
			result.p99Microseconds = quantile(99, 100);
			result.maximumMicroseconds =
				static_cast<double>(maximumNanoseconds) / 1000.0;
			return result;
		}

		DurationSummary ConsumeGpuEnvelopeSummary(
			std::array<std::atomic<UInt64>, kDurationBuckets>& durations,
			std::atomic<UInt64>& totalDuration,
			std::atomic<UInt64>& maximumDuration)
		{
			std::array<UInt64, kDurationBuckets> values = {};
			DurationSummary result;
			for (size_t bucket = 0; bucket < values.size(); ++bucket)
			{
				values[bucket] = durations[bucket]
					.exchange(0, std::memory_order_relaxed);
				result.count += values[bucket];
			}
			const UInt64 totalNanoseconds = totalDuration.exchange(
				0, std::memory_order_relaxed);
			const UInt64 maximumNanoseconds = maximumDuration.exchange(
				0, std::memory_order_relaxed);
			if (!result.count)
				return result;
			result.meanMicroseconds = static_cast<double>(totalNanoseconds)
				/ static_cast<double>(result.count) / 1000.0;
			const auto quantile = [&](UInt64 numerator, UInt64 denominator)
			{
				const UInt64 target = std::max<UInt64>(1,
					(result.count * numerator + denominator - 1)
						/ denominator);
				UInt64 cumulative = 0;
				for (size_t bucket = 0; bucket < values.size(); ++bucket)
				{
					cumulative += values[bucket];
					if (cumulative < target)
						continue;
					const size_t exponent = bucket / kDurationSubBuckets;
					const size_t subBucket = bucket % kDurationSubBuckets;
					long double upperNanoseconds = std::ldexp(
						1.0L, static_cast<int>(exponent));
					if (exponent >= 3)
					{
						const long double lowerNanoseconds =
							upperNanoseconds / 2.0L;
						const long double step =
							(upperNanoseconds - lowerNanoseconds)
							/ static_cast<long double>(kDurationSubBuckets);
						upperNanoseconds = lowerNanoseconds
							+ step * static_cast<long double>(subBucket + 1u);
					}
					return static_cast<double>(upperNanoseconds / 1000.0L);
				}
				return static_cast<double>(
					std::numeric_limits<UInt64>::max()) / 1000.0;
			};
			result.medianMicroseconds = quantile(50, 100);
			result.p95Microseconds = quantile(95, 100);
			result.p99Microseconds = quantile(99, 100);
			result.maximumMicroseconds =
				static_cast<double>(maximumNanoseconds) / 1000.0;
			return result;
		}

		DurationSummary ConsumeGpuAlphaEnvelopeSummary()
		{
			PerformanceState& state = GetPerformanceState();
			return ConsumeGpuEnvelopeSummary(
				state.gpuAlphaEnvelopeDurations,
				state.gpuAlphaEnvelopeNanoseconds,
				state.gpuAlphaEnvelopeMaximumNanoseconds);
		}

		DurationSummary ConsumeGpuVanillaLayoutEnvelopeSummary()
		{
			PerformanceState& state = GetPerformanceState();
			return ConsumeGpuEnvelopeSummary(
				state.gpuVanillaLayoutEnvelopeDurations,
				state.gpuVanillaLayoutEnvelopeNanoseconds,
				state.gpuVanillaLayoutEnvelopeMaximumNanoseconds);
		}

		GpuVanillaLayoutWorkloadAggregate
			ConsumeGpuVanillaLayoutWorkloadAggregate()
		{
			PerformanceState& state = GetPerformanceState();
			std::lock_guard<std::mutex> lock(
				state.gpuVanillaLayoutWorkloadMutex);
			const GpuVanillaLayoutWorkloadAggregate result =
				state.gpuVanillaLayoutWorkload;
			state.gpuVanillaLayoutWorkload = {};
			return result;
		}

		std::array<UInt64, kGpuTimingCounterCount>
			ConsumeGpuTimingCounters()
		{
			std::array<UInt64, kGpuTimingCounterCount> result = {};
			PerformanceState& state = GetPerformanceState();
			for (size_t index = 0; index < result.size(); ++index)
			{
				result[index] = state.gpuTimingCounters[index].exchange(
					0, std::memory_order_relaxed);
			}
			return result;
		}

		struct AccumulatorPrepTailSummary
		{
			std::array<UInt64, 4> counts = {};
			AccumulatorPrepTailWorst worst;
		};

		AccumulatorPrepTailSummary ConsumeAccumulatorPrepTailSummary()
		{
			PerformanceState& state = GetPerformanceState();
			AccumulatorPrepTailSummary result;
			for (size_t index = 0; index < result.counts.size(); ++index)
			{
				result.counts[index] = state.accumulatorPrepTailCounts[index]
					.exchange(0, std::memory_order_relaxed);
			}
			std::lock_guard<std::mutex> lock(state.accumulatorPrepTailMutex);
			result.worst = state.accumulatorPrepTailWorst;
			state.accumulatorPrepTailWorst = {};
			return result;
		}

	}

	void RecordFreeTypeGpuEnvelopeForeignPass()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->immediatePasses);
		IncrementEnvelopeCounter(workload->foreignPasses);
	}

	void RecordFreeTypeGpuEnvelopeNativeFacadePass()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->immediatePasses);
		IncrementEnvelopeCounter(workload->nativeFacadePasses);
	}

	void RecordFreeTypeGpuEnvelopeVanillaPass()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->immediatePasses);
		IncrementEnvelopeCounter(workload->vanillaPasses);
	}

	void RecordFreeTypeGpuEnvelopeVanillaCull(
		FreeTypeGpuEnvelopeCull cull)
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->vanillaCulls);
		switch (cull)
		{
		case FreeTypeGpuEnvelopeCull::App:
			IncrementEnvelopeCounter(workload->vanillaAppCulls);
			break;
		case FreeTypeGpuEnvelopeCull::Alpha:
			IncrementEnvelopeCounter(workload->vanillaAlphaCulls);
			break;
		case FreeTypeGpuEnvelopeCull::Clip:
			IncrementEnvelopeCounter(workload->vanillaClipCulls);
			break;
		case FreeTypeGpuEnvelopeCull::Scissor:
			IncrementEnvelopeCounter(workload->vanillaScissorCulls);
			break;
		}
	}

	void RecordFreeTypeGpuEnvelopeVanillaRuntimeFallback()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (workload)
		{
			IncrementEnvelopeCounter(
				workload->vanillaRuntimeFallbacks);
		}
	}

	void RecordFreeTypeGpuEnvelopeVanillaDraw(
		bool standardLite, UInt32 vertexCount,
		UInt32 triangleCount, bool useScissor,
		SInt32 scissorLeft, SInt32 scissorTop,
		SInt32 scissorRight, SInt32 scissorBottom)
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(standardLite
			? workload->vanillaStandardLiteDraws
			: workload->vanillaStockDraws);
		workload->vanillaVertices += vertexCount;
		workload->vanillaTriangles += triangleCount;

		const FreeTypeGpuEnvelopeViewport& viewport = workload->viewport;
		if (!viewport.width || !viewport.height)
		{
			IncrementEnvelopeCounter(
				workload->viewportUnavailableDraws);
			return;
		}
		const UInt64 viewportPixels = static_cast<UInt64>(viewport.width)
			* static_cast<UInt64>(viewport.height);
		if (!useScissor)
		{
			IncrementEnvelopeCounter(workload->unscissoredDraws);
			workload->effectiveClipRectPixels += viewportPixels;
			workload->viewportOpportunityPixels += viewportPixels;
			return;
		}

		IncrementEnvelopeCounter(workload->scissoredDraws);
		if (scissorRight <= scissorLeft || scissorBottom <= scissorTop)
		{
			IncrementEnvelopeCounter(workload->invalidScissorDraws);
			return;
		}
		const SInt64 viewportLeft = viewport.x;
		const SInt64 viewportTop = viewport.y;
		const SInt64 viewportRight = viewportLeft + viewport.width;
		const SInt64 viewportBottom = viewportTop + viewport.height;
		const SInt64 clippedLeft = std::max<SInt64>(
			scissorLeft, viewportLeft);
		const SInt64 clippedTop = std::max<SInt64>(
			scissorTop, viewportTop);
		const SInt64 clippedRight = std::min<SInt64>(
			scissorRight, viewportRight);
		const SInt64 clippedBottom = std::min<SInt64>(
			scissorBottom, viewportBottom);
		const UInt64 clippedPixels = clippedRight > clippedLeft
			&& clippedBottom > clippedTop
			? static_cast<UInt64>(clippedRight - clippedLeft)
				* static_cast<UInt64>(clippedBottom - clippedTop)
			: 0u;
		workload->effectiveClipRectPixels += clippedPixels;
		workload->viewportOpportunityPixels += viewportPixels;
	}

	FreeTypeGpuAlphaEnvelopeScope::FreeTypeGpuAlphaEnvelopeScope(
		IDirect3DDevice9* device, bool hasPreparedPayloads,
		bool hasVanillaLayout,
		const FreeTypeGpuEnvelopeViewport& viewport)
		: m_active(BeginGpuAlphaEnvelope(device, hasPreparedPayloads,
			hasVanillaLayout, viewport))
	{
	}

	FreeTypeGpuAlphaEnvelopeScope::~FreeTypeGpuAlphaEnvelopeScope()
	{
		if (m_active)
			EndGpuAlphaEnvelope();
	}

	void ResetFreeTypeGpuTiming()
	{
		ResetGpuQueryState(true);
	}

	FreeTypePerfScope::FreeTypePerfScope(
		FreeTypePerfPhase phase, bool enabled, SInt64* elapsedTicks)
		: m_phase(phase),
		m_elapsedTicks(elapsedTicks),
		m_active(enabled && g_bEnableFreeTypeFontRenderingLog)
	{
		if (!m_active)
			return;
		LARGE_INTEGER now = {};
		if (!QueryPerformanceCounter(&now))
		{
			m_active = false;
			return;
		}
		m_start = now.QuadPart;
	}

	FreeTypePerfScope::~FreeTypePerfScope()
	{
		Stop();
	}

	void FreeTypePerfScope::Stop()
	{
		if (!m_active)
			return;
		m_active = false;
		LARGE_INTEGER now = {};
		if (!QueryPerformanceCounter(&now))
			return;
		const SInt64 ticks = now.QuadPart - m_start;
		if (m_elapsedTicks)
			*m_elapsedTicks = ticks;
		RecordDuration(m_phase, ticks);
	}

	void RecordFreeTypePerf(FreeTypePerfCounter counter, UInt64 amount)
	{
		if (!g_bEnableFreeTypeFontRenderingLog || !amount)
			return;
		const size_t counterIndex = static_cast<size_t>(counter);
		PerfCounterBatchState& batch = s_perfCounterBatch;
		if (batch.depth)
		{
			if (!batch.values[counterIndex])
			{
				batch.touched[batch.touchedCount++] =
					static_cast<UInt16>(counterIndex);
			}
			batch.values[counterIndex] += amount;
			++batch.recordCount;
			return;
		}
		GetPerformanceState().counters[counterIndex].fetch_add(
			amount, std::memory_order_relaxed);
	}

	FreeTypePerfCounterBatchScope::FreeTypePerfCounterBatchScope(bool enabled)
		: m_active(enabled && g_bEnableFreeTypeFontRenderingLog)
	{
		if (!m_active)
			return;
		PerfCounterBatchState& batch = s_perfCounterBatch;
		if (!batch.depth)
		{
			batch.touchedCount = 0;
			batch.recordCount = 0;
			batch.scopeCount = 0;
		}
		++batch.depth;
		++batch.scopeCount;
	}

	FreeTypePerfCounterBatchScope::~FreeTypePerfCounterBatchScope()
	{
		if (!m_active)
			return;
		PerfCounterBatchState& batch = s_perfCounterBatch;
		if (!batch.depth || --batch.depth)
			return;

		PerformanceState& state = GetPerformanceState();
		const UInt64 atomicFlushes = static_cast<UInt64>(batch.touchedCount);
		for (size_t touchedIndex = 0;
			touchedIndex < batch.touchedCount; ++touchedIndex)
		{
			const size_t counterIndex = batch.touched[touchedIndex];
			const UInt64 amount = batch.values[counterIndex];
			batch.values[counterIndex] = 0;
			if (amount)
			{
				state.counters[counterIndex].fetch_add(
					amount, std::memory_order_relaxed);
			}
		}
		// Publishing the four batch diagnostics below also costs one atomic add
		// each. Report the net reduction rather than only the payload coalescing.
		const UInt64 atomicPublications = atomicFlushes + 4u;
		const UInt64 atomicsSaved = batch.recordCount > atomicPublications
			? batch.recordCount - atomicPublications : 0;
		state.counters[static_cast<size_t>(
			FreeTypePerfCounter::PerfCounterBatchScope)].fetch_add(
			batch.scopeCount, std::memory_order_relaxed);
		state.counters[static_cast<size_t>(
			FreeTypePerfCounter::PerfCounterBatchRecord)].fetch_add(
			batch.recordCount, std::memory_order_relaxed);
		state.counters[static_cast<size_t>(
			FreeTypePerfCounter::PerfCounterBatchAtomicFlush)].fetch_add(
			atomicFlushes, std::memory_order_relaxed);
		state.counters[static_cast<size_t>(
			FreeTypePerfCounter::PerfCounterBatchAtomicSaved)].fetch_add(
			atomicsSaved, std::memory_order_relaxed);
		batch.touchedCount = 0;
		batch.recordCount = 0;
		batch.scopeCount = 0;
	}

	SInt64 BeginFreeTypePerfSample()
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return 0;
		LARGE_INTEGER now = {};
		return QueryPerformanceCounter(&now) ? now.QuadPart : 0;
	}

	SInt64 EndFreeTypePerfSample(FreeTypePerfPhase phase, SInt64 start)
	{
		if (!start || !g_bEnableFreeTypeFontRenderingLog)
			return 0;
		LARGE_INTEGER now = {};
		if (!QueryPerformanceCounter(&now))
			return 0;
		const SInt64 ticks = now.QuadPart - start;
		RecordDuration(phase, ticks);
		return ticks;
	}

	void RecordFreeTypeAccumulatorPrepTailSample(
		const FreeTypeAccumulatorPrepTailSample& sample)
	{
		const SInt64 frequency = QueryPerfFrequency();
		if (!g_bEnableFreeTypeFontRenderingLog
			|| sample.totalTicks <= 0 || frequency <= 0)
			return;
		const auto toNanoseconds = [frequency](SInt64 ticks)
		{
			if (ticks <= 0)
				return UInt64{ 0 };
			const long double scaled = static_cast<long double>(ticks)
				* 1000000000.0L / static_cast<long double>(frequency);
			return static_cast<UInt64>(
				std::max<long double>(1.0L, scaled));
		};
		const UInt64 nanoseconds = toNanoseconds(sample.totalTicks);
		constexpr std::array<UInt64, 4> thresholds = {
			250000u, 500000u, 1000000u, 2000000u
		};
		PerformanceState& state = GetPerformanceState();
		for (size_t index = 0; index < thresholds.size(); ++index)
		{
			if (nanoseconds >= thresholds[index])
			{
				state.accumulatorPrepTailCounts[index].fetch_add(
					1, std::memory_order_relaxed);
			}
		}
		if (nanoseconds < thresholds.front())
			return;
		std::lock_guard<std::mutex> lock(state.accumulatorPrepTailMutex);
		if (nanoseconds <= state.accumulatorPrepTailWorst.nanoseconds)
			return;
		state.accumulatorPrepTailWorst = {
			nanoseconds,
			toNanoseconds(sample.resetTicks),
			toNanoseconds(sample.topologyTicks),
			toNanoseconds(sample.visibilityTicks),
			toNanoseconds(sample.metadataTicks),
			toNanoseconds(sample.readinessTicks),
			toNanoseconds(sample.lookupTicks),
			toNanoseconds(sample.facadeLoopTicks),
			toNanoseconds(sample.ringTicks),
			toNanoseconds(sample.singletonTicks),
			toNanoseconds(sample.commandTicks),
			toNanoseconds(sample.publishTicks),
			sample.itemCount,
			sample.facadeCount,
			sample.survivorCount,
			sample.payloadCount,
			sample.singletonCount,
			sample.commandFrameActive
		};
	}

	void ReportFreeTypePerf(bool force)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const ULONGLONG now = GetTickCount64();
		PerformanceState& state = GetPerformanceState();
		if (!force && state.lastReport && now - state.lastReport < 10000)
			return;
		state.lastReport = now;
		std::array<UInt64, kCounterCount> values = {};
		for (size_t i = 0; i < values.size(); ++i)
			values[i] = state.counters[i].exchange(
				0, std::memory_order_relaxed);
		const auto counterValue = [&values](FreeTypePerfCounter counter)
		{
			return values[static_cast<size_t>(counter)];
		};
		const DurationSummary gpuAlphaEnvelope =
			ConsumeGpuAlphaEnvelopeSummary();
		const DurationSummary gpuVanillaLayoutEnvelope =
			ConsumeGpuVanillaLayoutEnvelopeSummary();
		const GpuVanillaLayoutWorkloadAggregate gpuVanillaLayoutWorkload =
			ConsumeGpuVanillaLayoutWorkloadAggregate();
		const std::array<UInt64, kGpuTimingCounterCount> gpuTimingCounters =
			ConsumeGpuTimingCounters();
		const auto gpuCounterValue = [&gpuTimingCounters](
			GpuTimingCounter counter)
		{
			return gpuTimingCounters[static_cast<size_t>(counter)];
		};
		FreeTypeFontDebugLog(
			"tnvse_freetype_prepared_sidecar: capture_fallback=%llu rejected_fallback=%llu",
			counterValue(
				FreeTypePerfCounter::PreparedSidecarCaptureFallback),
			counterValue(
				FreeTypePerfCounter::PreparedSidecarRejectedFallback));
		const UInt64 vanillaConstantUpdates =
			values[static_cast<size_t>(
				FreeTypePerfCounter::VanillaConstantUpdate)];
		const UInt64 vanillaC0CompatibilityRepublishes =
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					VanillaPixelConstantCompatibilityRepublish)];
		const UInt64 vanillaC0RepublishElided =
			vanillaConstantUpdates >= vanillaC0CompatibilityRepublishes
				? vanillaConstantUpdates
					- vanillaC0CompatibilityRepublishes
				: 0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: bitmap_mem=%llu cross_font=%llu disk_hit=%llu miss=%llu write=%llu read_bytes=%llu write_bytes=%llu raster=%llu bitmap_batch_requests=%llu deduped=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu upload_rects=%llu text_artifact_hit=%llu miss=%llu hot=%llu bypass=%llu admitted=%llu evicted=%llu compiled_vertices=%llu vertex_init_bytes_avoided=%llu direct_vertex_init_bytes_avoided=%llu direct_range_init_bytes_avoided=%llu direct_common_resolves_saved=%llu direct_vertex_fill_failures=%llu composite_profile_vertices=%llu profile_scan_vertices_saved=%llu shader_batches=%llu cpu_effect_masks_avoided=%llu gpu_resident_glyph_hit=%llu miss=%llu atlas_snapshot_profile_reuse=%llu dynamic_vb_uploads=%llu bytes=%llu reuse=%llu discards=%llu static_vb_uploads=%llu bytes=%llu hits=%llu promotion_failed=%llu sorted_static_batches=%llu payloads=%llu bytes=%llu merged_packet_ranges=%llu metadata_hot=%llu locked=%llu sorted_facades=%llu unique_payloads=%llu frame_lookup_hits=%llu preflight_fast=%llu full=%llu direct_static=%llu direct_dynamic=%llu sorted_dynamic_batches=%llu payloads=%llu bytes=%llu lockless_packets=%llu visibility_checks=%llu culled=%llu app=%llu alpha=%llu clip=%llu scissor=%llu preflight_skipped=%llu packets_saved=%llu vertices_saved=%llu direct_shape_candidates=%llu direct_shape_draws=%llu direct_shape_vertices=%llu direct_shape_fallback=%llu constant_ownership_segments=%llu reuses=%llu releases=%llu snapshot_gets_elided=%llu restore_sets_elided=%llu vanilla_constant_updates=%llu reuses=%llu composite_constant_full=%llu private_reuses=%llu partial=%llu vanilla_c0_republish_elided=%llu compat_republishes=%llu private_registers_uploaded=%llu full_tail_elided=%llu foreign_pass_private_invalidations=%llu vanilla_layout_private_preserves=%llu sampler_sets=%llu reuses=%llu composite_onequad_single_page=%llu onequad_paged=%llu onequad_build_fallback=%llu legacy_multipage_fallback=%llu shader_fallback=%llu composite_draws=%llu tile_passes=%llu cache_hit=%llu miss=%llu state_changes=%llu generated=%llu evicted=%llu cache_bytes=%llu budget_reject=%llu rtt_fail=%llu restore_fail=%llu visual_validated=%llu rejected=%llu inconclusive=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapMemoryHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapCrossFontHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskWrite)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskReadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskWriteBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapRasterized)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapBatchRequest)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapBatchDedupe)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasCreated)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasGrown)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadRect)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactHotHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactAdmissionBypass)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactAdmission)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactEviction)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactCompiledVertex)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					TextArtifactVertexInitializationBytesAvoided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					DirectShapeVertexInitializationBytesAvoided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					DirectShapeRangeInitializationBytesAvoided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::DirectShapeCommonResolutionsSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::DirectShapeVertexCoverageFailure)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactCompositeProfileVertex)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					TextArtifactCompositeProfileVertexScanSaved)],
			values[static_cast<size_t>(FreeTypePerfCounter::ShaderEffectBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::CpuEffectMasksAvoided)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasSnapshotProfileReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexDiscard)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexPromotionFailed)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticPayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::MergedPacketRange)],
			values[static_cast<size_t>(FreeTypePerfCounter::MetadataHotHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::MetadataLockedLookup)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFrameFacade)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFramePayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFrameLookupHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreflightFastHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreflightFullValidation)],
			values[static_cast<size_t>(FreeTypePerfCounter::DirectStaticResidencyHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::DirectDynamicResidencyHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicPayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::LocklessPacketPrepare)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityCheck)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityCulled)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityAppCulled)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityZeroAlpha)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityClip)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityScissor)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityPreflightSkipped)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityPacketsSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityVerticesSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectCandidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectDraw)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectVertex)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipSegment)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipRelease)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantSnapshotGetElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantRestoreSetElided)],
			vanillaConstantUpdates,
			static_cast<UInt64>(0),
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantFullUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantPartialUpload)],
			vanillaC0RepublishElided,
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					VanillaPixelConstantCompatibilityRepublish)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantRegisterUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantFullTailElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					NativePrivateStateForeignRenderPassInvalidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePrivateStateVanillaLayoutPreserve)],
			values[static_cast<size_t>(FreeTypePerfCounter::SamplerStateSet)],
			values[static_cast<size_t>(FreeTypePerfCounter::SamplerStateReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeFusedEligible)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeOrderedEligible)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeOverlapFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeMultiPageFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeShaderFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeDraw)],
			values[static_cast<size_t>(FreeTypePerfCounter::TilePass)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheStateChange)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheGenerated)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheEvicted)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheBudgetReject)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheRttFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheRestoreFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualValidated)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualRejected)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualInconclusive)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_text_artifact_front: hot_buckets=%u hot_ways=%u hot_capacity=%u hot_hits=%llu hot_expired=%llu hot_live_replacements=%llu admission_buckets=%u admission_ways=%u admission_capacity=%u admission_history_hits=%llu admission_candidate_replacements=%llu admission_established_replacements=%llu",
			kTextArtifactHotBucketCount, kTextArtifactHotWays,
			kTextArtifactHotBucketCount * kTextArtifactHotWays,
			counterValue(FreeTypePerfCounter::TextArtifactHotHit),
			counterValue(FreeTypePerfCounter::TextArtifactHotEntryExpired),
			counterValue(FreeTypePerfCounter::TextArtifactHotEntryReplacement),
			kTextArtifactAdmissionBucketCount,
			kTextArtifactAdmissionWays,
			kTextArtifactAdmissionBucketCount
				* kTextArtifactAdmissionWays,
			counterValue(FreeTypePerfCounter::
				TextArtifactAdmissionHistoryHit),
			counterValue(FreeTypePerfCounter::
				TextArtifactAdmissionCandidateReplacement),
			counterValue(FreeTypePerfCounter::
				TextArtifactAdmissionEstablishedReplacement));
		FreeTypeFontDebugLog(
			"tnvse_freetype_preflight_clip_cull: checks=%llu culled=%llu viewport=%llu scissor=%llu fail_open=%llu honored=%llu camera_validations=%llu camera_run_reuses=%llu revoked=%llu revoke_invalid=%llu revoke_frame=%llu revoke_camera=%llu revoke_geometry=%llu revoke_transform=%llu revoke_bound=%llu revoke_scissor=%llu revoke_proof=%llu transform_hits=%llu cache_key_materializations_elided=%llu cache_route_validations_elided=%llu transform_misses=%llu transform_identity_misses=%llu transform_key_misses=%llu transform_unavailable=%llu vanilla_ui_ortho_translation=%llu generic_transforms=%llu rect_hot_hits=%llu rect_set_hits=%llu rect_builds=%llu sorted_tile_property_lookups_elided=%llu sorted_alpha_property_lookups_elided=%llu",
			counterValue(FreeTypePerfCounter::VisibilityPreflightClipCheck),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipCulled),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipViewport),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipScissor),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipFailOpen),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipHonored),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipCameraValidation),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipCameraRunReuse),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevoked),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeInvalid),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeFrame),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeCamera),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeGeometry),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeTransform),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeBound),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeScissor),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeProof),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformHit),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformHit),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformHit),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformMiss),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformIdentityMiss),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformKeyMiss),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipTransformUnavailable),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipVanillaUiOrthographicTranslation),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipGenericTransform),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRectHotHit),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRectSetHit),
			counterValue(FreeTypePerfCounter::
				VisibilityPreflightClipRectBuild),
			counterValue(FreeTypePerfCounter::
				VisibilitySortedTilePropertyLookupElided),
			counterValue(FreeTypePerfCounter::
				VisibilitySortedAlphaPropertyLookupElided));
		FreeTypeFontDebugLog(
			"tnvse_freetype_static_promotion: deferred_lifecycle=%llu deferred_upload_history=%llu deferred_budget=%llu deferred_retry=%llu cold_evictions=%llu cold_evicted_bytes=%llu all_static_fast_exit=%llu lease_payload_validations_elided=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredLifecycle)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredUploadHistory)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredBudget)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredRetry)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticResidentColdEviction)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticResidentColdEvictionBytes)],
			counterValue(FreeTypePerfCounter::SortedAllStaticFastExit),
			counterValue(FreeTypePerfCounter::
				SortedAllStaticPayloadValidationElided));
		FreeTypeFontDebugLog(
			"tnvse_freetype_native_registration: artifact_sealed=%llu artifact_fallback=%llu hook_fast=%llu hook_slow=%llu proxy_fast=%llu proxy_slow=%llu",
			counterValue(
				FreeTypePerfCounter::NativeRegistrationArtifactSealed),
			counterValue(
				FreeTypePerfCounter::NativeRegistrationArtifactFallback),
			counterValue(FreeTypePerfCounter::NativeRegistrationHookFast),
			counterValue(FreeTypePerfCounter::NativeRegistrationHookSlow),
			counterValue(FreeTypePerfCounter::NativeRegistrationProxyFast),
			counterValue(FreeTypePerfCounter::NativeRegistrationProxySlow));
		FreeTypeFontDebugLog(
			"tnvse_freetype_thin_registration: sample_rate=256 sampling=continuous_tls calls=%llu samples=%llu fast_forward=%llu hook_mismatch=%llu slow_audits=%llu suppressed=%llu metadata_batches=%llu metadata_shapes=%llu metadata_missing=%llu sorted_scan_fallback=%llu facade_topology=%llu facade_fallback=%llu occurrence_fallback=%llu",
			counterValue(FreeTypePerfCounter::ThinRegistrationCall),
			counterValue(FreeTypePerfCounter::ThinRegistrationTimingSample),
			counterValue(FreeTypePerfCounter::ThinRegistrationFastForward),
			counterValue(FreeTypePerfCounter::ThinRegistrationHookMismatch),
			counterValue(FreeTypePerfCounter::ThinRegistrationSlowAudit),
			counterValue(FreeTypePerfCounter::ThinRegistrationSuppressed),
			counterValue(FreeTypePerfCounter::ThinRegistrationMetadataBatch),
			counterValue(FreeTypePerfCounter::ThinRegistrationMetadataShape),
			counterValue(FreeTypePerfCounter::ThinRegistrationMetadataMissing),
			counterValue(FreeTypePerfCounter::
				ThinRegistrationSortedScanFallback),
			counterValue(FreeTypePerfCounter::ThinRegistrationFacadeTopology),
			counterValue(FreeTypePerfCounter::ThinRegistrationFacadeFallback),
			counterValue(FreeTypePerfCounter::ThinRegistrationOccurrenceFallback));
		FreeTypeFontDebugLog(
			"tnvse_freetype_structural_fastpaths: readiness_raw_hit=%llu full_audit=%llu hook_mismatch=%llu renderer_mismatch=%llu virtualquery_avoided=%llu tile_callback_mismatch=%llu render_alpha_mismatch=%llu immediate_mismatch=%llu atlas_mismatch=%llu singleton_inline=%llu singleton_heap=%llu child_allocations_avoided=%llu metadata_reserve=%llu metadata_rehash=%llu",
			counterValue(FreeTypePerfCounter::StructuralReadinessRawHit),
			counterValue(FreeTypePerfCounter::StructuralReadinessFullAudit),
			counterValue(FreeTypePerfCounter::StructuralReadinessHookMismatch),
			counterValue(FreeTypePerfCounter::StructuralReadinessRendererMismatch),
			counterValue(FreeTypePerfCounter::StructuralReadinessVirtualQueryAvoided),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessTileCallbackMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessRenderAlphaMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessImmediateMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessAtlasMismatch),
			counterValue(FreeTypePerfCounter::SingletonFacadeInlinePayload),
			counterValue(FreeTypePerfCounter::SingletonFacadeHeapPayload),
			counterValue(FreeTypePerfCounter::SingletonFacadeChildAllocationAvoided),
			counterValue(FreeTypePerfCounter::MetadataMapReserve),
			counterValue(FreeTypePerfCounter::MetadataMapRehash));
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: constant_capture_mirror=%llu driver=%llu state_shadow_driver_gets=%llu isolation_bypass=%llu vertex_aa_sets=%llu reuses=%llu vertex_aa_vanilla_preserved=%llu command_program_setups=%llu binds_elided=%llu texture_sets=%llu reuses=%llu command_packet_constant_full=%llu partial=%llu reuses=%llu registers_uploaded=%llu full_tail_elided=%llu",
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantVanillaPreserved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandProgramSetup)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandProgramBindElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTextureBindSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTextureBindReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantFullUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantPartialUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantRegisterUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantFullTailElided)]);
		const UInt64 directFacadeClassified =
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeModelData)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeBufferData)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeTileProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeStreamCount)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexStride)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexChip)];
		const UInt64 directFallbackClassified =
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackCommand)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackSubmission)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingInput)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTopology)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingAtlas)]
			+ directFacadeClassified
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTexture)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingShader)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackRuntime)];
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: direct_fallback_total=%llu classified=%llu command=%llu submission=%llu binding_input=%llu binding_topology=%llu binding_atlas=%llu binding_facade=%llu facade_classified=%llu facade_model_data=%llu facade_alpha_property=%llu facade_buffer_data=%llu facade_tile_property=%llu facade_stream_count=%llu facade_vertex_stride=%llu facade_vertex_chip_array=%llu facade_vertex_chip=%llu binding_property=%llu binding_texture=%llu binding_shader=%llu runtime=%llu synthetic_buffers=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallback)],
			directFallbackClassified,
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackCommand)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackSubmission)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingInput)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingAtlas)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingFacade)],
			directFacadeClassified,
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeModelData)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeBufferData)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeTileProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeStreamCount)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexStride)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexChip)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTexture)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingShader)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackRuntime)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectSyntheticBuffer)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_singleton_facade: candidates=%llu facades=%llu payload_packets=%llu single_packet_artifacts=%llu multi_packet_artifacts=%llu direct_frames=%llu span_frames=%llu packet_loop_frames=%llu topology_switches=%llu fallbacks=%llu partial_faults=%llu sibling_shapes=0 static_hits=%llu dynamic_hits=%llu rebinds=%llu revokes=%llu sorted_preflight_saved=%llu proxy_packets_saved=%llu",
			counterValue(FreeTypePerfCounter::SingletonFacadeCandidate),
			counterValue(FreeTypePerfCounter::SingletonFacadeCreated),
			counterValue(FreeTypePerfCounter::SingletonFacadePayloadPacket),
			counterValue(FreeTypePerfCounter::SingletonFacadeSinglePacketArtifact),
			counterValue(FreeTypePerfCounter::SingletonFacadeMultiPacketArtifact),
			counterValue(FreeTypePerfCounter::SingletonFacadeDirectFrame),
			counterValue(FreeTypePerfCounter::SingletonFacadeSpanFrame),
			counterValue(FreeTypePerfCounter::SingletonFacadePacketLoopFrame),
			counterValue(FreeTypePerfCounter::SingletonFacadeTopologySwitch),
			counterValue(FreeTypePerfCounter::SingletonFacadeFallback),
			counterValue(FreeTypePerfCounter::SingletonFacadePartialFault),
			counterValue(FreeTypePerfCounter::SingletonFacadeStaticHit),
			counterValue(FreeTypePerfCounter::SingletonFacadeDynamicHit),
			counterValue(FreeTypePerfCounter::SingletonFacadeRebind),
			counterValue(FreeTypePerfCounter::SingletonFacadeRevoke),
			counterValue(FreeTypePerfCounter::SingletonFacadeSortedPreflightSaved),
			counterValue(FreeTypePerfCounter::SingletonFacadeProxyPacketSaved));
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep: empty_fast=%llu metadata_cull_skipped=%llu metadata_owner_slots_avoided=%llu metadata_index_lookups_elided=%llu frame_entry_index_hits=%llu frame_entry_hash_lookups=%llu no_prepared_payload=%llu",
			counterValue(FreeTypePerfCounter::AccumulatorEmptyFastPath),
			counterValue(FreeTypePerfCounter::AccumulatorMetadataCullSkipped),
			counterValue(FreeTypePerfCounter::
				AccumulatorMetadataOwnerSlotAvoided),
			counterValue(FreeTypePerfCounter::
				AccumulatorMetadataIndexLookupElided),
			counterValue(FreeTypePerfCounter::
				SortedFrameItemIndexLookupHit),
			counterValue(FreeTypePerfCounter::
				SortedFrameFacadeHashLookup),
			counterValue(FreeTypePerfCounter::AccumulatorNoPreparedPayload));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_layout: eligible=%llu created=%llu create_fallback=%llu draws=%llu culls=%llu runtime_fallback=%llu vertices=%llu shifted_eligible=%llu shifted_created=%llu shifted_draws=%llu shifted_runtime_fallback=%llu precache_accepted=%llu precache_unavailable=%llu payload_upload_attempts=%llu success=%llu failure=%llu bytes=%llu native_pack_pending=%llu prior_generation_decl_uses=%llu private_state_carries=%llu private_state_carry_rejected=%llu draw_token_hits=%llu draw_token_slow_paths=%llu draw_token_uncertified=%llu draw_token_shape_shader_mismatches=%llu draw_token_generation_mismatches=%llu draw_token_geometry_mismatches=%llu draw_token_native_pack_mismatches=%llu draw_token_layout_mismatches=%llu draw_token_first_certifications=%llu draw_token_recertifications=%llu draw_token_rejected=%llu",
			counterValue(FreeTypePerfCounter::VanillaLayoutEligible),
			counterValue(FreeTypePerfCounter::VanillaLayoutCreated),
			counterValue(FreeTypePerfCounter::VanillaLayoutFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutDraw),
			counterValue(FreeTypePerfCounter::VanillaLayoutCull),
			counterValue(FreeTypePerfCounter::VanillaLayoutRuntimeFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutVertex),
			counterValue(FreeTypePerfCounter::VanillaLayoutShiftedEligible),
			counterValue(FreeTypePerfCounter::VanillaLayoutShiftedCreated),
			counterValue(FreeTypePerfCounter::VanillaLayoutShiftedDraw),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutShiftedRuntimeFallback),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPrecacheAccepted),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPrecacheUnavailable),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadAttempt),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadSuccess),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadFailure),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadBytes),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutNativePackPending),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutPriorGenerationDeclarationUse),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutPrivateStateCarry),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutPrivateStateCarryRejected),
			counterValue(FreeTypePerfCounter::VanillaLayoutDrawTokenHit),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenSlowPath),
			counterValue(FreeTypePerfCounter::VanillaLayoutDrawTokenUncertified),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenShapeShaderMismatch),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenGenerationMismatch),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenGeometryMismatch),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenNativePackMismatch),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenLayoutMismatch),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenFirstCertification),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenRecertification),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutDrawTokenRejected));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite: candidates=%llu replays=%llu current_declaration_replays=%llu compatible_declaration_replays=%llu fallbacks=%llu envelope=%llu program=%llu renderer=%llu geometry=%llu binding=%llu declaration=%llu prelude=%llu",
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteCandidate),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteReplay),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteCurrentDeclarationReplay),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteCompatibleDeclarationReplay),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallback),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackEnvelope),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackProgram),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackRenderer),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackGeometry),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackBinding),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackDeclaration),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallbackPrelude));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_binding_token: token_state=%llu packet_vertices=%llu packet_identity=%llu data_vertices=%llu token_stream=%llu declaration_identity=%llu",
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingTokenState),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingPacketVertexCount),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingPacketIdentity),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingDataVertexCount),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingTokenStream),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingDeclarationIdentity));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_binding_buffer: flags=%llu geometry_group=%llu fvf=%llu software_vp=%llu vertex_snapshot=%llu vertex_packet=%llu max_vertices=%llu stream_count=%llu stride_array=%llu stride_identity=%llu stride_value=%llu",
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBufferFlags),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingGeometryGroup),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingFvf),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingSoftwareVertexProcessing),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBufferVertexSnapshot),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBufferVertexPacket),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBufferMaxVertices),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBufferStreamCount),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingStrideArray),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingStrideIdentity),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingStrideValue));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_binding_resources: chip=%llu chip_identity=%llu chip_index=%llu vertex_buffer=%llu vertex_buffer_identity=%llu chip_offset=%llu chip_size=%llu chip_lock=%llu vertex_range=%llu index_buffer=%llu index_count=%llu index_size=%llu base_vertex=%llu primitive=%llu arrays=%llu submit_witness=%llu unclassified=%llu",
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChip),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChipIdentity),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChipIndex),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexBuffer),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexBufferIdentity),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChipOffset),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChipSize),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexChipLock),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingVertexRange),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingIndexBuffer),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingIndexCount),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingIndexSize),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingBaseVertex),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingPrimitiveTopology),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingArrayTopology),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingSubmissionWitness),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingUnclassified));
		const UInt64 adjacentBindingPairs = counterValue(
			FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentPair);
		const UInt64 adjacentBindingExact = counterValue(
			FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentExact);
		const double adjacentBindingExactPercent = adjacentBindingPairs
			? static_cast<double>(adjacentBindingExact)
				/ static_cast<double>(adjacentBindingPairs) * 100.0
			: 0.0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_binding_adjacency: pairs=%llu exact=%llu exact_pct=%.2f same_declaration=%llu same_vertex_buffer=%llu same_index_buffer=%llu same_stream_offset=%llu same_stride=%llu",
			adjacentBindingPairs, adjacentBindingExact,
			adjacentBindingExactPercent,
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentSameDeclaration),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentSameVertexBuffer),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentSameIndexBuffer),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentSameStreamOffset),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingAdjacentSameStride));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_binding_runs: runs=%llu draws=%llu len_1=%llu len_2=%llu len_3_4=%llu len_5_8=%llu len_9_16=%llu len_17_32=%llu len_33_plus=%llu",
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRun),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunDraw),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength1),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength2),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength3To4),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength5To8),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength9To16),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength17To32),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutStandardLiteBindingRunLength33Plus));
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: command_recorded=%llu single_packet_commands=%llu single_packet_build_fallbacks=%llu single_packet_hits=%llu single_packet_misses=%llu single_packet_replays=%llu single_packet_fallbacks=%llu spans=%llu packets=%llu span_hits=%llu span_misses=%llu retained_bridge_draws=%llu native_replays=%llu vanilla_bootstraps_saved=%llu direct_single_replays=%llu light_validations=%llu packet_epoch_guards=%llu packet_state_elisions=%llu render_target_validations=%llu execution_segments=%llu segment_full_validations=%llu segment_validation_reuses=%llu segment_invalidations=%llu retained_program_hits=%llu retained_program_misses=%llu fallback_token=%llu generation=%llu atlas=%llu resource=%llu topology=%llu hook=%llu nested=%llu render_target=%llu state=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketBuildFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedBridgeDraw)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandNativeReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandVanillaBootstrapSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandDirectRangeReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketLightValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketEpochGuard)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandPacketStateValidationElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRenderTargetValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandExecutionSegment)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentFullValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentValidationReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentInvalidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedProgramHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedProgramMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackToken)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackGeneration)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackAtlas)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackResource)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackHook)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackNested)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackRenderTarget)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackState)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: command_direct_facade_single_recorded=%llu hits=%llu misses=%llu replays=%llu fallbacks=%llu build_view_hits=%llu build_view_misses=%llu build_binding_reuses=%llu build_vector_growth=%llu deferred_render_target_captures=%llu tile_retained_builds=%llu refreshes=%llu hits=%llu misses=%llu packet_reuses=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildViewHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildViewMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildBindingReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildVectorGrowth)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDeferredRenderTargetCapture)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedBuild)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedRefresh)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandTileRetainedPacketReuse)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: standard_pass_lite_candidates=%llu retained_builds=%llu retained_reuses=%llu retained_hits=%llu retained_misses=%llu stage1_eligible=%llu stage2_resident=%llu stage3_replays=%llu standard_v2_replays=%llu standard_v2_compat=%llu vanilla_fallbacks=%llu fallback_envelope=%llu program=%llu renderer=%llu geometry=%llu binding=%llu prelude=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteCandidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedBuild)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage1Eligible)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage2Resident)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage3Replay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassV2Replay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					StandardPassV2CompatibilityReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteVanillaFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackEnvelope)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackProgram)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackRenderer)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackGeometry)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackBinding)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackPrelude)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_direct_draw_lite: candidates=%llu replays=%llu fallbacks=%llu program=%llu renderer=%llu geometry=%llu binding=%llu declaration=%llu binding_sets=%llu binding_reuses=%llu binding_device_failures=%llu draw_device_failures=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteCandidate)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackProgram)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackRenderer)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackGeometry)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackBinding)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackDeclaration)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingSet)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingDeviceFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteDrawDeviceFailure)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: segment_device_state_starts=%llu segment_device_state_reuses=%llu pass_sets=%llu pass_reuses=%llu constants_sets=%llu constants_reuses=%llu constants_lite_replays=%llu constants_lite_fallbacks=%llu constants_lite_scaled_fallbacks=%llu blend_sets=%llu blend_reuses=%llu alpha_test_sets=%llu alpha_test_reuses=%llu render_states_sets=%llu render_states_reuses=%llu post_calls=%llu post_elisions=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStateStart)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStateReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePassSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePassReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceConstantsSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceConstantsReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativeTileConstantsLiteReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativeTileConstantsLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsLiteScaledScissorFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceBlendSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceBlendReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceAlphaTestSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceAlphaTestReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceRenderStatesSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceRenderStatesReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePostSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePostElision)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_translation_lite: replays=%llu transient_replays=%llu fallbacks=%llu not_applicable=%llu scaled_scissor=%llu nonfinite=%llu device_failure=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteTransientReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteNotApplicableFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteScaledScissorFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteNonFiniteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteDeviceFailure)]);
		const UInt64 worldRotationOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationOnly);
		const UInt64 worldTranslationOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchTranslationOnly);
		const UInt64 worldScaleOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchScaleOnly);
		const UInt64 worldRotationTranslation = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationTranslation);
		const UInt64 worldRotationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationScale);
		const UInt64 worldTranslationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchTranslationScale);
		const UInt64 worldRotationTranslationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationTranslationScale);
		const UInt64 worldUnclassified = counterValue(
			FreeTypePerfCounter::SegmentDeviceConstantsFirstMismatchWorld);
		const UInt64 worldMismatchTotal = worldRotationOnly
			+ worldTranslationOnly + worldScaleOnly
			+ worldRotationTranslation + worldRotationScale
			+ worldTranslationScale + worldRotationTranslationScale
			+ worldUnclassified;
		const UInt64 worldRotation = worldRotationOnly
			+ worldRotationTranslation + worldRotationScale
			+ worldRotationTranslationScale;
		const UInt64 worldTranslation = worldTranslationOnly
			+ worldRotationTranslation + worldTranslationScale
			+ worldRotationTranslationScale;
		const UInt64 worldScale = worldScaleOnly + worldRotationScale
			+ worldTranslationScale + worldRotationTranslationScale;
		const UInt64 constantsFirstMismatchTotal =
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProgram)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchRotates)
			+ worldMismatchTotal
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchView)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProjection)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchViewProjection)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraRight)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraUp)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchOverlayColor)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTextureTransform)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTileAlpha)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchMaterialAlpha)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchNearDepth)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchDepthRange);
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_mismatch: total=%llu program=%llu rotates=%llu world=%llu view=%llu projection=%llu view_projection=%llu camera_right=%llu camera_up=%llu overlay_color=%llu texture_transform=%llu tile_alpha=%llu material_alpha=%llu near_depth=%llu depth_range=%llu",
			constantsFirstMismatchTotal,
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProgram),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchRotates),
			worldMismatchTotal,
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchView),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProjection),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchViewProjection),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraRight),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraUp),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchOverlayColor),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTextureTransform),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTileAlpha),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchMaterialAlpha),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchNearDepth),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchDepthRange));
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_world_mismatch: total=%llu rotation=%llu translation=%llu scale=%llu rotation_only=%llu translation_only=%llu scale_only=%llu rotation_translation=%llu rotation_scale=%llu translation_scale=%llu rotation_translation_scale=%llu unclassified=%llu",
			worldMismatchTotal, worldRotation, worldTranslation, worldScale,
			worldRotationOnly, worldTranslationOnly, worldScaleOnly,
			worldRotationTranslation, worldRotationScale,
			worldTranslationScale, worldRotationTranslationScale,
			worldUnclassified);
		const DurationSummary layout =
			ConsumeDurationSummary(FreeTypePerfPhase::Layout);
		const DurationSummary sidecar =
			ConsumeDurationSummary(FreeTypePerfPhase::Sidecar);
		const DurationSummary directCompile =
			ConsumeDurationSummary(FreeTypePerfPhase::DirectCompile);
		const DurationSummary textArtifactCompile =
			ConsumeDurationSummary(
				FreeTypePerfPhase::TextArtifactCompile);
		const DurationSummary nativeRegistration =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistration);
		const DurationSummary registrationReadiness =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationReadiness);
		const DurationSummary registrationShape =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationShape);
		const DurationSummary registrationBudget =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationBudget);
		const DurationSummary registrationAllocation =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationAllocation);
		const DurationSummary registrationPayload =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationPayload);
		const DurationSummary registrationAccounting =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationAccounting);
		const DurationSummary registrationPublish =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationPublish);
		const DurationSummary preflight =
			ConsumeDurationSummary(FreeTypePerfPhase::Preflight);
		const DurationSummary submit =
			ConsumeDurationSummary(FreeTypePerfPhase::Submit);
		const DurationSummary commandBuild =
			ConsumeDurationSummary(FreeTypePerfPhase::CommandBuild);
		const DurationSummary commandBuildStamp =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildStamp);
		const DurationSummary commandBuildDirectFacade =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildDirectFacade);
		const DurationSummary commandBuildOrdinary =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildOrdinary);
		const DurationSummary commandBuildFinalize =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildFinalize);
		const DurationSummary commandSubmit =
			ConsumeDurationSummary(FreeTypePerfPhase::CommandSubmit);
		const DurationSummary extendedFnt =
			ConsumeDurationSummary(
				FreeTypePerfPhase::ExtendedFntGeometry);
		const DurationSummary frameRouteTotal =
			ConsumeDurationSummary(FreeTypePerfPhase::FrameRouteTotal);
		const DurationSummary frameRoutePrep =
			ConsumeDurationSummary(FreeTypePerfPhase::FrameRoutePrep);
		const DurationSummary framePrepReset =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepReset);
		const DurationSummary framePrepTopology =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepTopology);
		const DurationSummary framePrepVisibility =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepVisibility);
		const DurationSummary framePrepMetadata =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepMetadata);
		const DurationSummary framePrepFacades =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepFacades);
		const DurationSummary framePrepReadiness =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepReadiness);
		const DurationSummary framePrepLookup =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepLookup);
		const DurationSummary framePrepFacadeLoop =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepFacadeLoop);
		const DurationSummary framePrepRing =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepRing);
		const DurationSummary framePrepRingInputScan =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingInputScan);
		const DurationSummary framePrepRingResource =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingResource);
		const DurationSummary framePrepRingStaticScan =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingStaticScan);
		const DurationSummary framePrepRingStaticLock =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingStaticLock);
		const DurationSummary framePrepRingStaticCopy =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingStaticCopy);
		const DurationSummary framePrepRingStaticUnlock =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingStaticUnlock);
		const DurationSummary framePrepRingStaticCommit =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingStaticCommit);
		const DurationSummary framePrepRingDynamicResolve =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingDynamicResolve);
		const DurationSummary framePrepRingDynamicLock =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingDynamicLock);
		const DurationSummary framePrepRingDynamicCopy =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingDynamicCopy);
		const DurationSummary framePrepRingDynamicUnlock =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingDynamicUnlock);
		const DurationSummary framePrepRingDynamicCommit =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingDynamicCommit);
		const DurationSummary framePrepRingLeasePublish =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FramePrepRingLeasePublish);
		const DurationSummary framePrepSingletons =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepSingletons);
		const DurationSummary framePrepPublish =
			ConsumeDurationSummary(FreeTypePerfPhase::FramePrepPublish);
		const DurationSummary frameRouteVanillaRender =
			ConsumeDurationSummary(
				FreeTypePerfPhase::FrameRouteVanillaRender);
		const DurationSummary registerRoute =
			ConsumeDurationSummary(FreeTypePerfPhase::RegisterRoute);
		const DurationSummary dispatchRoute =
			ConsumeDurationSummary(FreeTypePerfPhase::DispatchRoute);
		const DurationSummary preflightClipHonorGate =
			ConsumeDurationSummary(
				FreeTypePerfPhase::PreflightClipHonorGate);
		const DurationSummary vanillaStandardLiteState =
			ConsumeDurationSummary(
				FreeTypePerfPhase::VanillaLayoutStandardLiteState);
		const DurationSummary vanillaStandardLiteBinding =
			ConsumeDurationSummary(
				FreeTypePerfPhase::VanillaLayoutStandardLiteBinding);
		const DurationSummary vanillaStandardLiteDraw =
			ConsumeDurationSummary(
				FreeTypePerfPhase::VanillaLayoutStandardLiteDraw);
		const DurationSummary vanillaStandardLitePost =
			ConsumeDurationSummary(
				FreeTypePerfPhase::VanillaLayoutStandardLitePost);
		const AccumulatorPrepTailSummary accumulatorPrepTail =
			ConsumeAccumulatorPrepTailSummary();
		const AccumulatorPrepTailWorst& accumulatorPrepWorst =
			accumulatorPrepTail.worst;
		const UInt64 accumulatorPrepWorstAttributed =
			accumulatorPrepWorst.resetNanoseconds
			+ accumulatorPrepWorst.topologyNanoseconds
			+ accumulatorPrepWorst.visibilityNanoseconds
			+ accumulatorPrepWorst.metadataNanoseconds
			+ accumulatorPrepWorst.readinessNanoseconds
			+ accumulatorPrepWorst.lookupNanoseconds
			+ accumulatorPrepWorst.facadeLoopNanoseconds
			+ accumulatorPrepWorst.ringNanoseconds
			+ accumulatorPrepWorst.singletonNanoseconds
			+ accumulatorPrepWorst.commandNanoseconds
			+ accumulatorPrepWorst.publishNanoseconds;
		const UInt64 accumulatorPrepWorstResidual =
			accumulatorPrepWorst.nanoseconds >= accumulatorPrepWorstAttributed
				? accumulatorPrepWorst.nanoseconds
					- accumulatorPrepWorstAttributed
				: 0;
		const auto ringPerInvocationMean = [ringCount = framePrepRing.count](
			const DurationSummary& summary)
		{
			return ringCount
				? summary.meanMicroseconds
					* static_cast<double>(summary.count)
					/ static_cast<double>(ringCount)
				: 0.0;
		};
		const double framePrepRingAttributedMean =
			ringPerInvocationMean(framePrepRingInputScan)
			+ ringPerInvocationMean(framePrepRingResource)
			+ ringPerInvocationMean(framePrepRingStaticScan)
			+ ringPerInvocationMean(framePrepRingStaticLock)
			+ ringPerInvocationMean(framePrepRingStaticCopy)
			+ ringPerInvocationMean(framePrepRingStaticUnlock)
			+ ringPerInvocationMean(framePrepRingStaticCommit)
			+ ringPerInvocationMean(framePrepRingDynamicResolve)
			+ ringPerInvocationMean(framePrepRingDynamicLock)
			+ ringPerInvocationMean(framePrepRingDynamicCopy)
			+ ringPerInvocationMean(framePrepRingDynamicUnlock)
			+ ringPerInvocationMean(framePrepRingDynamicCommit)
			+ ringPerInvocationMean(framePrepRingLeasePublish);
		const double framePrepRingResidualMean = std::max(0.0,
			framePrepRing.meanMicroseconds - framePrepRingAttributedMean);
		const double framePrepRingCoveragePercent =
			framePrepRing.meanMicroseconds > 0.0
				? framePrepRingAttributedMean
					/ framePrepRing.meanMicroseconds * 100.0
				: 0.0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_timing: layout_n=%llu median_us=%.3f p95_us=%.3f sidecar_n=%llu median_us=%.3f p95_us=%.3f direct_compile_n=%llu median_us=%.3f p95_us=%.3f text_artifact_compile_n=%llu median_us=%.3f p95_us=%.3f native_registration_n=%llu median_us=%.3f p95_us=%.3f preflight_n=%llu median_us=%.3f p95_us=%.3f submit_n=%llu median_us=%.3f p95_us=%.3f command_build_n=%llu median_us=%.3f p95_us=%.3f command_submit_n=%llu median_us=%.3f p95_us=%.3f extended_fnt_geometry_n=%llu median_us=%.3f p95_us=%.3f",
			layout.count, layout.medianMicroseconds,
			layout.p95Microseconds,
			sidecar.count, sidecar.medianMicroseconds,
			sidecar.p95Microseconds,
			directCompile.count,
			directCompile.medianMicroseconds,
			directCompile.p95Microseconds,
			textArtifactCompile.count,
			textArtifactCompile.medianMicroseconds,
			textArtifactCompile.p95Microseconds,
			nativeRegistration.count,
			nativeRegistration.medianMicroseconds,
			nativeRegistration.p95Microseconds,
			preflight.count, preflight.medianMicroseconds,
			preflight.p95Microseconds,
			submit.count, submit.medianMicroseconds,
			submit.p95Microseconds,
			commandBuild.count, commandBuild.medianMicroseconds,
			commandBuild.p95Microseconds,
			commandSubmit.count, commandSubmit.medianMicroseconds,
			commandSubmit.p95Microseconds,
			extendedFnt.count, extendedFnt.medianMicroseconds,
			extendedFnt.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_standard_lite_cpu_timing: sample_rate=%u state_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f binding_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f draw_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f post_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f",
			kVanillaLayoutStandardLiteCpuSampleRate,
			vanillaStandardLiteState.count,
			vanillaStandardLiteState.meanMicroseconds,
			vanillaStandardLiteState.medianMicroseconds,
			vanillaStandardLiteState.p95Microseconds,
			vanillaStandardLiteState.p99Microseconds,
			vanillaStandardLiteState.maximumMicroseconds,
			vanillaStandardLiteBinding.count,
			vanillaStandardLiteBinding.meanMicroseconds,
			vanillaStandardLiteBinding.medianMicroseconds,
			vanillaStandardLiteBinding.p95Microseconds,
			vanillaStandardLiteBinding.p99Microseconds,
			vanillaStandardLiteBinding.maximumMicroseconds,
			vanillaStandardLiteDraw.count,
			vanillaStandardLiteDraw.meanMicroseconds,
			vanillaStandardLiteDraw.medianMicroseconds,
			vanillaStandardLiteDraw.p95Microseconds,
			vanillaStandardLiteDraw.p99Microseconds,
			vanillaStandardLiteDraw.maximumMicroseconds,
			vanillaStandardLitePost.count,
			vanillaStandardLitePost.meanMicroseconds,
			vanillaStandardLitePost.medianMicroseconds,
			vanillaStandardLitePost.p95Microseconds,
			vanillaStandardLitePost.p99Microseconds,
			vanillaStandardLitePost.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_command_build_timing: stamp_n=%llu median_us=%.3f p95_us=%.3f direct_facade_n=%llu median_us=%.3f p95_us=%.3f ordinary_n=%llu median_us=%.3f p95_us=%.3f finalize_n=%llu median_us=%.3f p95_us=%.3f",
			commandBuildStamp.count,
			commandBuildStamp.medianMicroseconds,
			commandBuildStamp.p95Microseconds,
			commandBuildDirectFacade.count,
			commandBuildDirectFacade.medianMicroseconds,
			commandBuildDirectFacade.p95Microseconds,
			commandBuildOrdinary.count,
			commandBuildOrdinary.medianMicroseconds,
			commandBuildOrdinary.p95Microseconds,
			commandBuildFinalize.count,
			commandBuildFinalize.medianMicroseconds,
			commandBuildFinalize.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_native_registration_timing: readiness_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f shape_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f budget_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f allocation_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f payload_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f accounting_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f publish_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f total_mean_us=%.3f",
			registrationReadiness.count, registrationReadiness.meanMicroseconds,
			registrationReadiness.medianMicroseconds,
			registrationReadiness.p95Microseconds,
			registrationShape.count, registrationShape.meanMicroseconds,
			registrationShape.medianMicroseconds,
			registrationShape.p95Microseconds,
			registrationBudget.count, registrationBudget.meanMicroseconds,
			registrationBudget.medianMicroseconds,
			registrationBudget.p95Microseconds,
			registrationAllocation.count, registrationAllocation.meanMicroseconds,
			registrationAllocation.medianMicroseconds,
			registrationAllocation.p95Microseconds,
			registrationPayload.count, registrationPayload.meanMicroseconds,
			registrationPayload.medianMicroseconds,
			registrationPayload.p95Microseconds,
			registrationAccounting.count, registrationAccounting.meanMicroseconds,
			registrationAccounting.medianMicroseconds,
			registrationAccounting.p95Microseconds,
			registrationPublish.count, registrationPublish.meanMicroseconds,
			registrationPublish.medianMicroseconds,
			registrationPublish.p95Microseconds,
			nativeRegistration.meanMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_frame_route_timing: route_total_n=%llu median_us=%.3f p95_us=%.3f prep_n=%llu median_us=%.3f p95_us=%.3f vanilla_render_n=%llu median_us=%.3f p95_us=%.3f",
			frameRouteTotal.count, frameRouteTotal.medianMicroseconds,
			frameRouteTotal.p95Microseconds,
			frameRoutePrep.count, frameRoutePrep.medianMicroseconds,
			frameRoutePrep.p95Microseconds,
			frameRouteVanillaRender.count,
			frameRouteVanillaRender.medianMicroseconds,
			frameRouteVanillaRender.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_gpu_timing: scope=tile_alpha_envelope valid_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f submitted=%llu completed=%llu in_flight=%llu ring_full=%llu nested=%llu create_fail=%llu issue_fail=%llu read_fail=%llu disjoint=%llu invalid_range=%llu reset_discarded=%llu async=1 flush=0",
			gpuAlphaEnvelope.count,
			gpuAlphaEnvelope.meanMicroseconds,
			gpuAlphaEnvelope.medianMicroseconds,
			gpuAlphaEnvelope.p95Microseconds,
			gpuAlphaEnvelope.p99Microseconds,
			gpuAlphaEnvelope.maximumMicroseconds,
			gpuCounterValue(GpuTimingCounter::Submitted),
			gpuCounterValue(GpuTimingCounter::Completed),
			state.gpuQueriesInFlight.load(std::memory_order_relaxed),
			gpuCounterValue(GpuTimingCounter::RingFull),
			gpuCounterValue(GpuTimingCounter::Nested),
			gpuCounterValue(GpuTimingCounter::CreateFailure),
			gpuCounterValue(GpuTimingCounter::IssueFailure),
			gpuCounterValue(GpuTimingCounter::ReadFailure),
			gpuCounterValue(GpuTimingCounter::Disjoint),
			gpuCounterValue(GpuTimingCounter::InvalidRange),
			gpuCounterValue(GpuTimingCounter::ResetDiscarded));
		FreeTypeFontDebugLog(
			"tnvse_freetype_gpu_vanilla_layout_timing: scope=tile_alpha_envelope qualifier=contains_preflight_surviving_vanilla_layout valid_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f eligible=%llu sample_attempts=%llu sample_skipped=%llu submitted=%llu completed=%llu in_flight=%llu prepared_payload_eligible=%llu vanilla_only_sample_rate=%u async=1 flush=0",
			gpuVanillaLayoutEnvelope.count,
			gpuVanillaLayoutEnvelope.meanMicroseconds,
			gpuVanillaLayoutEnvelope.medianMicroseconds,
			gpuVanillaLayoutEnvelope.p95Microseconds,
			gpuVanillaLayoutEnvelope.p99Microseconds,
			gpuVanillaLayoutEnvelope.maximumMicroseconds,
			gpuCounterValue(GpuTimingCounter::VanillaLayoutEligible),
			gpuCounterValue(GpuTimingCounter::VanillaLayoutSampleAttempt),
			gpuCounterValue(GpuTimingCounter::VanillaLayoutSampleSkipped),
			gpuCounterValue(GpuTimingCounter::VanillaLayoutSubmitted),
			gpuCounterValue(GpuTimingCounter::VanillaLayoutCompleted),
			state.gpuVanillaLayoutQueriesInFlight.load(
				std::memory_order_relaxed),
			gpuCounterValue(GpuTimingCounter::PreparedPayloadEligible),
			kVanillaLayoutGpuTimingSampleRate);

		const auto saturatingDifference = [](UInt64 total, UInt64 part)
		{
			return total > part ? total - part : UInt64{0};
		};
		const UInt64 gpuWorkloadClassifiedPasses =
			gpuVanillaLayoutWorkload.foreignPasses
				+ gpuVanillaLayoutWorkload.nativeFacadePasses
				+ gpuVanillaLayoutWorkload.vanillaPasses;
		const UInt64 gpuWorkloadVanillaDraws =
			gpuVanillaLayoutWorkload.vanillaStandardLiteDraws
				+ gpuVanillaLayoutWorkload.vanillaStockDraws;
		const UInt64 gpuWorkloadVanillaOutcomes =
			gpuWorkloadVanillaDraws
				+ gpuVanillaLayoutWorkload.vanillaCulls
				+ gpuVanillaLayoutWorkload.vanillaRuntimeFallbacks;
		const double gpuWorkloadMeanImmediatePasses =
			gpuVanillaLayoutWorkload.samples
				? static_cast<double>(
					gpuVanillaLayoutWorkload.immediatePasses)
					/ static_cast<double>(gpuVanillaLayoutWorkload.samples)
				: 0.0;
		const double gpuWorkloadMeanVanillaDraws =
			gpuVanillaLayoutWorkload.samples
				? static_cast<double>(gpuWorkloadVanillaDraws)
					/ static_cast<double>(gpuVanillaLayoutWorkload.samples)
				: 0.0;
		const double gpuWorkloadMeanVanillaTriangles =
			gpuVanillaLayoutWorkload.samples
				? static_cast<double>(
					gpuVanillaLayoutWorkload.vanillaTriangles)
					/ static_cast<double>(gpuVanillaLayoutWorkload.samples)
				: 0.0;
		const double gpuWorkloadClipRectUpperBoundPercent =
			gpuVanillaLayoutWorkload.viewportOpportunityPixels
				? static_cast<double>(
					gpuVanillaLayoutWorkload.effectiveClipRectPixels)
					/ static_cast<double>(gpuVanillaLayoutWorkload.
						viewportOpportunityPixels) * 100.0
				: 0.0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_gpu_vanilla_layout_envelope_workload: samples=%llu timing_valid_n=%llu immediate_passes=%llu classified_passes=%llu unclassified_passes=%llu immediate_passes_mean=%.3f foreign_immediate_passes=%llu native_facade_passes=%llu vanilla_passes=%llu vanilla_draws=%llu vanilla_draws_mean=%.3f standard_lite_draws=%llu stock_draws=%llu runtime_fallbacks=%llu culls=%llu app_culls=%llu alpha_culls=%llu clip_culls=%llu scissor_culls=%llu unresolved_vanilla_passes=%llu vanilla_vertices=%llu vanilla_triangles=%llu vanilla_triangles_mean=%.3f scissored_draws=%llu unscissored_draws=%llu invalid_scissor_draws=%llu viewport_unavailable_draws=%llu clip_rect_upper_bound_pct=%.3f",
			gpuVanillaLayoutWorkload.samples,
			gpuVanillaLayoutEnvelope.count,
			gpuVanillaLayoutWorkload.immediatePasses,
			gpuWorkloadClassifiedPasses,
			saturatingDifference(
				gpuVanillaLayoutWorkload.immediatePasses,
				gpuWorkloadClassifiedPasses),
			gpuWorkloadMeanImmediatePasses,
			gpuVanillaLayoutWorkload.foreignPasses,
			gpuVanillaLayoutWorkload.nativeFacadePasses,
			gpuVanillaLayoutWorkload.vanillaPasses,
			gpuWorkloadVanillaDraws,
			gpuWorkloadMeanVanillaDraws,
			gpuVanillaLayoutWorkload.vanillaStandardLiteDraws,
			gpuVanillaLayoutWorkload.vanillaStockDraws,
			gpuVanillaLayoutWorkload.vanillaRuntimeFallbacks,
			gpuVanillaLayoutWorkload.vanillaCulls,
			gpuVanillaLayoutWorkload.vanillaAppCulls,
			gpuVanillaLayoutWorkload.vanillaAlphaCulls,
			gpuVanillaLayoutWorkload.vanillaClipCulls,
			gpuVanillaLayoutWorkload.vanillaScissorCulls,
			saturatingDifference(
				gpuVanillaLayoutWorkload.vanillaPasses,
				gpuWorkloadVanillaOutcomes),
			gpuVanillaLayoutWorkload.vanillaVertices,
			gpuVanillaLayoutWorkload.vanillaTriangles,
			gpuWorkloadMeanVanillaTriangles,
			gpuVanillaLayoutWorkload.scissoredDraws,
			gpuVanillaLayoutWorkload.unscissoredDraws,
			gpuVanillaLayoutWorkload.invalidScissorDraws,
			gpuVanillaLayoutWorkload.viewportUnavailableDraws,
			gpuWorkloadClipRectUpperBoundPercent);

		const auto gpuBucketMeanMicroseconds = [](
			const GpuVanillaLayoutDrawBucket& bucket)
		{
			return bucket.samples
				? static_cast<double>(bucket.nanoseconds)
					/ static_cast<double>(bucket.samples) / 1000.0
				: 0.0;
		};
		const auto gpuBucketMaximumMicroseconds = [](
			const GpuVanillaLayoutDrawBucket& bucket)
		{
			return static_cast<double>(bucket.maximumNanoseconds) / 1000.0;
		};
		const auto& gpuDrawBuckets = gpuVanillaLayoutWorkload.drawBuckets;
		FreeTypeFontDebugLog(
			"tnvse_freetype_gpu_vanilla_layout_envelope_draw_buckets: draws_0_8_n=%llu mean_us=%.3f max_us=%.3f draws_9_16_n=%llu mean_us=%.3f max_us=%.3f draws_17_32_n=%llu mean_us=%.3f max_us=%.3f draws_33_64_n=%llu mean_us=%.3f max_us=%.3f draws_65_plus_n=%llu mean_us=%.3f max_us=%.3f",
			gpuDrawBuckets[0].samples,
			gpuBucketMeanMicroseconds(gpuDrawBuckets[0]),
			gpuBucketMaximumMicroseconds(gpuDrawBuckets[0]),
			gpuDrawBuckets[1].samples,
			gpuBucketMeanMicroseconds(gpuDrawBuckets[1]),
			gpuBucketMaximumMicroseconds(gpuDrawBuckets[1]),
			gpuDrawBuckets[2].samples,
			gpuBucketMeanMicroseconds(gpuDrawBuckets[2]),
			gpuBucketMaximumMicroseconds(gpuDrawBuckets[2]),
			gpuDrawBuckets[3].samples,
			gpuBucketMeanMicroseconds(gpuDrawBuckets[3]),
			gpuBucketMaximumMicroseconds(gpuDrawBuckets[3]),
			gpuDrawBuckets[4].samples,
			gpuBucketMeanMicroseconds(gpuDrawBuckets[4]),
			gpuBucketMaximumMicroseconds(gpuDrawBuckets[4]));

		const GpuEnvelopeWorkload& gpuWorstWorkload =
			gpuVanillaLayoutWorkload.worstWorkload;
		const UInt64 gpuWorstClassifiedPasses =
			static_cast<UInt64>(gpuWorstWorkload.foreignPasses)
				+ gpuWorstWorkload.nativeFacadePasses
				+ gpuWorstWorkload.vanillaPasses;
		const UInt64 gpuWorstVanillaDraws =
			static_cast<UInt64>(gpuWorstWorkload.vanillaStandardLiteDraws)
				+ gpuWorstWorkload.vanillaStockDraws;
		const UInt64 gpuWorstVanillaOutcomes = gpuWorstVanillaDraws
			+ gpuWorstWorkload.vanillaCulls
			+ gpuWorstWorkload.vanillaRuntimeFallbacks;
		const double gpuWorstClipRectUpperBoundPercent =
			gpuWorstWorkload.viewportOpportunityPixels
				? static_cast<double>(gpuWorstWorkload.effectiveClipRectPixels)
					/ static_cast<double>(
						gpuWorstWorkload.viewportOpportunityPixels) * 100.0
				: 0.0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_gpu_vanilla_layout_envelope_worst: duration_us=%.3f viewport=%ux%u+%u+%u immediate_passes=%u classified_passes=%llu unclassified_passes=%llu foreign_immediate_passes=%u native_facade_passes=%u vanilla_passes=%u vanilla_draws=%llu standard_lite_draws=%u stock_draws=%u runtime_fallbacks=%u culls=%u app_culls=%u alpha_culls=%u clip_culls=%u scissor_culls=%u unresolved_vanilla_passes=%llu vanilla_vertices=%llu vanilla_triangles=%llu scissored_draws=%u unscissored_draws=%u invalid_scissor_draws=%u viewport_unavailable_draws=%u clip_rect_upper_bound_pct=%.3f",
			static_cast<double>(gpuVanillaLayoutWorkload.worstNanoseconds)
				/ 1000.0,
			gpuWorstWorkload.viewport.width,
			gpuWorstWorkload.viewport.height,
			gpuWorstWorkload.viewport.x,
			gpuWorstWorkload.viewport.y,
			gpuWorstWorkload.immediatePasses,
			gpuWorstClassifiedPasses,
			saturatingDifference(
				gpuWorstWorkload.immediatePasses,
				gpuWorstClassifiedPasses),
			gpuWorstWorkload.foreignPasses,
			gpuWorstWorkload.nativeFacadePasses,
			gpuWorstWorkload.vanillaPasses,
			gpuWorstVanillaDraws,
			gpuWorstWorkload.vanillaStandardLiteDraws,
			gpuWorstWorkload.vanillaStockDraws,
			gpuWorstWorkload.vanillaRuntimeFallbacks,
			gpuWorstWorkload.vanillaCulls,
			gpuWorstWorkload.vanillaAppCulls,
			gpuWorstWorkload.vanillaAlphaCulls,
			gpuWorstWorkload.vanillaClipCulls,
			gpuWorstWorkload.vanillaScissorCulls,
			saturatingDifference(
				gpuWorstWorkload.vanillaPasses,
				gpuWorstVanillaOutcomes),
			gpuWorstWorkload.vanillaVertices,
			gpuWorstWorkload.vanillaTriangles,
			gpuWorstWorkload.scissoredDraws,
			gpuWorstWorkload.unscissoredDraws,
			gpuWorstWorkload.invalidScissorDraws,
			gpuWorstWorkload.viewportUnavailableDraws,
			gpuWorstClipRectUpperBoundPercent);
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep_phases: topology_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f metadata_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f facades_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f ring_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f singletons_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f command_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f publish_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f",
			framePrepTopology.count, framePrepTopology.meanMicroseconds,
			framePrepTopology.p95Microseconds,
			framePrepTopology.p99Microseconds,
			framePrepTopology.maximumMicroseconds,
			framePrepMetadata.count, framePrepMetadata.meanMicroseconds,
			framePrepMetadata.p95Microseconds,
			framePrepMetadata.p99Microseconds,
			framePrepMetadata.maximumMicroseconds,
			framePrepFacades.count, framePrepFacades.meanMicroseconds,
			framePrepFacades.p95Microseconds,
			framePrepFacades.p99Microseconds,
			framePrepFacades.maximumMicroseconds,
			framePrepRing.count, framePrepRing.meanMicroseconds,
			framePrepRing.p95Microseconds,
			framePrepRing.p99Microseconds,
			framePrepRing.maximumMicroseconds,
			framePrepSingletons.count,
			framePrepSingletons.meanMicroseconds,
			framePrepSingletons.p95Microseconds,
			framePrepSingletons.p99Microseconds,
			framePrepSingletons.maximumMicroseconds,
			commandBuild.count, commandBuild.meanMicroseconds,
			commandBuild.p95Microseconds,
			commandBuild.p99Microseconds,
			commandBuild.maximumMicroseconds,
			framePrepPublish.count, framePrepPublish.meanMicroseconds,
			framePrepPublish.p95Microseconds,
			framePrepPublish.p99Microseconds,
			framePrepPublish.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_ring_stage_timing: ring_n=%llu mean_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f attributed_per_ring_us=%.3f residual_per_ring_us=%.3f coverage_pct=%.2f input_scan_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f resource_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f static_scan_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f dynamic_resolve_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f lease_publish_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f",
			framePrepRing.count, framePrepRing.meanMicroseconds,
			framePrepRing.p95Microseconds,
			framePrepRing.p99Microseconds,
			framePrepRing.maximumMicroseconds,
			framePrepRingAttributedMean,
			framePrepRingResidualMean,
			framePrepRingCoveragePercent,
			framePrepRingInputScan.count,
			framePrepRingInputScan.meanMicroseconds,
			ringPerInvocationMean(framePrepRingInputScan),
			framePrepRingInputScan.p95Microseconds,
			framePrepRingInputScan.maximumMicroseconds,
			framePrepRingResource.count,
			framePrepRingResource.meanMicroseconds,
			ringPerInvocationMean(framePrepRingResource),
			framePrepRingResource.p95Microseconds,
			framePrepRingResource.maximumMicroseconds,
			framePrepRingStaticScan.count,
			framePrepRingStaticScan.meanMicroseconds,
			ringPerInvocationMean(framePrepRingStaticScan),
			framePrepRingStaticScan.p95Microseconds,
			framePrepRingStaticScan.maximumMicroseconds,
			framePrepRingDynamicResolve.count,
			framePrepRingDynamicResolve.meanMicroseconds,
			ringPerInvocationMean(framePrepRingDynamicResolve),
			framePrepRingDynamicResolve.p95Microseconds,
			framePrepRingDynamicResolve.maximumMicroseconds,
			framePrepRingLeasePublish.count,
			framePrepRingLeasePublish.meanMicroseconds,
			ringPerInvocationMean(framePrepRingLeasePublish),
			framePrepRingLeasePublish.p95Microseconds,
			framePrepRingLeasePublish.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_ring_upload_timing: ring_n=%llu static_lock_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f static_copy_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f static_unlock_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f static_commit_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f dynamic_lock_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f dynamic_copy_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f dynamic_unlock_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f dynamic_commit_n=%llu mean_us=%.3f per_ring_us=%.3f p95_us=%.3f max_us=%.3f",
			framePrepRing.count,
			framePrepRingStaticLock.count,
			framePrepRingStaticLock.meanMicroseconds,
			ringPerInvocationMean(framePrepRingStaticLock),
			framePrepRingStaticLock.p95Microseconds,
			framePrepRingStaticLock.maximumMicroseconds,
			framePrepRingStaticCopy.count,
			framePrepRingStaticCopy.meanMicroseconds,
			ringPerInvocationMean(framePrepRingStaticCopy),
			framePrepRingStaticCopy.p95Microseconds,
			framePrepRingStaticCopy.maximumMicroseconds,
			framePrepRingStaticUnlock.count,
			framePrepRingStaticUnlock.meanMicroseconds,
			ringPerInvocationMean(framePrepRingStaticUnlock),
			framePrepRingStaticUnlock.p95Microseconds,
			framePrepRingStaticUnlock.maximumMicroseconds,
			framePrepRingStaticCommit.count,
			framePrepRingStaticCommit.meanMicroseconds,
			ringPerInvocationMean(framePrepRingStaticCommit),
			framePrepRingStaticCommit.p95Microseconds,
			framePrepRingStaticCommit.maximumMicroseconds,
			framePrepRingDynamicLock.count,
			framePrepRingDynamicLock.meanMicroseconds,
			ringPerInvocationMean(framePrepRingDynamicLock),
			framePrepRingDynamicLock.p95Microseconds,
			framePrepRingDynamicLock.maximumMicroseconds,
			framePrepRingDynamicCopy.count,
			framePrepRingDynamicCopy.meanMicroseconds,
			ringPerInvocationMean(framePrepRingDynamicCopy),
			framePrepRingDynamicCopy.p95Microseconds,
			framePrepRingDynamicCopy.maximumMicroseconds,
			framePrepRingDynamicUnlock.count,
			framePrepRingDynamicUnlock.meanMicroseconds,
			ringPerInvocationMean(framePrepRingDynamicUnlock),
			framePrepRingDynamicUnlock.p95Microseconds,
			framePrepRingDynamicUnlock.maximumMicroseconds,
			framePrepRingDynamicCommit.count,
			framePrepRingDynamicCommit.meanMicroseconds,
			ringPerInvocationMean(framePrepRingDynamicCommit),
			framePrepRingDynamicCommit.p95Microseconds,
			framePrepRingDynamicCommit.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep_tail: prep_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f reset_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f visibility_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f",
			frameRoutePrep.count, frameRoutePrep.meanMicroseconds,
			frameRoutePrep.medianMicroseconds,
			frameRoutePrep.p95Microseconds,
			frameRoutePrep.p99Microseconds,
			frameRoutePrep.maximumMicroseconds,
			framePrepReset.count, framePrepReset.meanMicroseconds,
			framePrepReset.medianMicroseconds,
			framePrepReset.p95Microseconds,
			framePrepReset.p99Microseconds,
			framePrepReset.maximumMicroseconds,
			framePrepVisibility.count,
			framePrepVisibility.meanMicroseconds,
			framePrepVisibility.medianMicroseconds,
			framePrepVisibility.p95Microseconds,
			framePrepVisibility.p99Microseconds,
			framePrepVisibility.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep_tail_detail: readiness_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f lookup_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f facade_loop_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f",
			framePrepReadiness.count,
			framePrepReadiness.meanMicroseconds,
			framePrepReadiness.medianMicroseconds,
			framePrepReadiness.p95Microseconds,
			framePrepReadiness.p99Microseconds,
			framePrepReadiness.maximumMicroseconds,
			framePrepLookup.count, framePrepLookup.meanMicroseconds,
			framePrepLookup.medianMicroseconds,
			framePrepLookup.p95Microseconds,
			framePrepLookup.p99Microseconds,
			framePrepLookup.maximumMicroseconds,
			framePrepFacadeLoop.count,
			framePrepFacadeLoop.meanMicroseconds,
			framePrepFacadeLoop.medianMicroseconds,
			framePrepFacadeLoop.p95Microseconds,
			framePrepFacadeLoop.p99Microseconds,
			framePrepFacadeLoop.maximumMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep_tail_workload: over_250us=%llu over_500us=%llu over_1000us=%llu over_2000us=%llu worst_us=%.3f items=%u facades=%u survivors=%u culled=%u payloads=%u singletons=%u command=%u",
			accumulatorPrepTail.counts[0],
			accumulatorPrepTail.counts[1],
			accumulatorPrepTail.counts[2],
			accumulatorPrepTail.counts[3],
			static_cast<double>(accumulatorPrepTail.worst.nanoseconds) / 1000.0,
			accumulatorPrepTail.worst.itemCount,
			accumulatorPrepTail.worst.facadeCount,
			accumulatorPrepTail.worst.survivorCount,
			accumulatorPrepTail.worst.facadeCount
				>= accumulatorPrepTail.worst.survivorCount
				? accumulatorPrepTail.worst.facadeCount
					- accumulatorPrepTail.worst.survivorCount
				: 0,
			accumulatorPrepTail.worst.payloadCount,
			accumulatorPrepTail.worst.singletonCount,
			accumulatorPrepTail.worst.commandFrameActive ? 1u : 0u);
		FreeTypeFontDebugLog(
			"tnvse_freetype_accumulator_prep_tail_worst: total_us=%.3f reset_us=%.3f topology_us=%.3f visibility_us=%.3f metadata_us=%.3f readiness_us=%.3f lookup_us=%.3f facade_loop_us=%.3f ring_us=%.3f singletons_us=%.3f command_us=%.3f publish_us=%.3f residual_us=%.3f",
			static_cast<double>(accumulatorPrepWorst.nanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.resetNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.topologyNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.visibilityNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.metadataNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.readinessNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.lookupNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.facadeLoopNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.ringNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.singletonNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.commandNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorst.publishNanoseconds) / 1000.0,
			static_cast<double>(accumulatorPrepWorstResidual) / 1000.0);
		FreeTypeFontDebugLog(
			"tnvse_freetype_dispatch_timing: dispatch_sample_rate=%u dispatch_route_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f register_route_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f honor_sample_rate=%u honor_gate_n=%llu median_us=%.3f p95_us=%.3f",
			kNativeFontDispatchRouteCpuSampleRate,
			dispatchRoute.count, dispatchRoute.meanMicroseconds,
			dispatchRoute.medianMicroseconds,
			dispatchRoute.p95Microseconds,
			registerRoute.count, registerRoute.meanMicroseconds,
			registerRoute.medianMicroseconds,
			registerRoute.p95Microseconds,
			kNativeFontVisibilityHonorCpuSampleRate,
			preflightClipHonorGate.count,
			preflightClipHonorGate.medianMicroseconds,
			preflightClipHonorGate.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_counter_batch: scopes=%llu records=%llu atomic_flushes=%llu atomics_saved=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::PerfCounterBatchScope)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PerfCounterBatchRecord)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PerfCounterBatchAtomicFlush)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PerfCounterBatchAtomicSaved)]);
	}
}

namespace fonthook
{
	void PumpFreeTypeFontPerformance()
	{
		vectorfont::ReportFreeTypePerf(false);
	}

	void ReportFreeTypeFontPerformanceNow()
	{
		vectorfont::ReportFreeTypePerf(true);
	}
}
