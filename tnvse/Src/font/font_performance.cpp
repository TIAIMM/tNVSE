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

		struct PerfCounterBatchState
		{
			std::array<UInt64, kCounterCount> values = {};
			std::array<UInt16, kCounterCount> touched = {};
			size_t touchedCount = 0;
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
			UInt32 foreignPasses = 0;
			UInt32 vanillaPasses = 0;
			UInt32 vanillaCulls = 0;
			UInt32 vanillaStandardLiteDraws = 0;
			UInt32 vanillaStockDraws = 0;
		};

		struct GpuVanillaLayoutWorkloadAggregate
		{
			UInt64 samples = 0;
			UInt64 foreignPasses = 0;
			UInt64 vanillaPasses = 0;
			UInt64 vanillaCulls = 0;
			UInt64 vanillaStandardLiteDraws = 0;
			UInt64 vanillaStockDraws = 0;
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

		void RecordGpuVanillaLayoutEnvelopeWorkload(
			const GpuEnvelopeWorkload& workload)
		{
			PerformanceState& state = GetPerformanceState();
			std::lock_guard<std::mutex> lock(
				state.gpuVanillaLayoutWorkloadMutex);
			GpuVanillaLayoutWorkloadAggregate& aggregate =
				state.gpuVanillaLayoutWorkload;
			++aggregate.samples;
			aggregate.foreignPasses += workload.foreignPasses;
			aggregate.vanillaPasses += workload.vanillaPasses;
			aggregate.vanillaCulls += workload.vanillaCulls;
			aggregate.vanillaStandardLiteDraws +=
				workload.vanillaStandardLiteDraws;
			aggregate.vanillaStockDraws += workload.vanillaStockDraws;
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
					RecordGpuVanillaLayoutEnvelopeWorkload(workload);
					RecordGpuTimingCounter(
						GpuTimingCounter::VanillaLayoutCompleted);
				}
			}
		}

		bool BeginGpuAlphaEnvelope(IDirect3DDevice9* device,
			bool hasPreparedPayloads, bool hasVanillaLayout)
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
			result.maximumMicroseconds =
				static_cast<double>(maximumNanoseconds) / 1000.0;
			// Histogram quantiles use the selected bucket's upper bound. Clamp that
			// estimate to the exact observed maximum so the published summary keeps
			// the usual quantile <= maximum invariant.
			result.medianMicroseconds = std::min(
				quantile(50, 100), result.maximumMicroseconds);
			result.p95Microseconds = std::min(
				quantile(95, 100), result.maximumMicroseconds);
			result.p99Microseconds = std::min(
				quantile(99, 100), result.maximumMicroseconds);
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
			result.maximumMicroseconds =
				static_cast<double>(maximumNanoseconds) / 1000.0;
			result.medianMicroseconds = std::min(
				quantile(50, 100), result.maximumMicroseconds);
			result.p95Microseconds = std::min(
				quantile(95, 100), result.maximumMicroseconds);
			result.p99Microseconds = std::min(
				quantile(99, 100), result.maximumMicroseconds);
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

	}

	void RecordFreeTypeGpuEnvelopeForeignPass()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->foreignPasses);
	}

	void RecordFreeTypeGpuEnvelopeVanillaPass()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->vanillaPasses);
	}

	void RecordFreeTypeGpuEnvelopeVanillaCull()
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(workload->vanillaCulls);
	}

	void RecordFreeTypeGpuEnvelopeVanillaDraw(bool standardLite)
	{
		GpuEnvelopeWorkload* workload = GetActiveGpuEnvelopeWorkload();
		if (!workload)
			return;
		IncrementEnvelopeCounter(standardLite
			? workload->vanillaStandardLiteDraws
			: workload->vanillaStockDraws);
	}

	FreeTypeGpuAlphaEnvelopeScope::FreeTypeGpuAlphaEnvelopeScope(
		IDirect3DDevice9* device, bool hasPreparedPayloads,
		bool hasVanillaLayout)
		: m_active(BeginGpuAlphaEnvelope(device, hasPreparedPayloads,
			hasVanillaLayout))
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
		FreeTypePerfPhase phase, bool enabled)
		: m_phase(phase),
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
			batch.touchedCount = 0;
		++batch.depth;
	}

	FreeTypePerfCounterBatchScope::~FreeTypePerfCounterBatchScope()
	{
		if (!m_active)
			return;
		PerfCounterBatchState& batch = s_perfCounterBatch;
		if (!batch.depth || --batch.depth)
			return;

		PerformanceState& state = GetPerformanceState();
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
		batch.touchedCount = 0;
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
		for (size_t index = 0; index < values.size(); ++index)
		{
			values[index] = state.counters[index].exchange(
				0, std::memory_order_relaxed);
		}
		const auto counterValue = [&values](FreeTypePerfCounter counter)
		{
			return values[static_cast<size_t>(counter)];
		};

		std::array<DurationSummary, kPhaseCount> phaseSummaries = {};
		for (size_t index = 0; index < phaseSummaries.size(); ++index)
		{
			phaseSummaries[index] = ConsumeDurationSummary(
				static_cast<FreeTypePerfPhase>(index));
		}
		const auto& phaseValue =
			[&phaseSummaries](FreeTypePerfPhase phase) -> const DurationSummary&
		{
			return phaseSummaries[static_cast<size_t>(phase)];
		};

		const DurationSummary gpuAlphaEnvelope =
			ConsumeGpuAlphaEnvelopeSummary();
		const DurationSummary gpuVanillaLayoutEnvelope =
			ConsumeGpuVanillaLayoutEnvelopeSummary();
		const GpuVanillaLayoutWorkloadAggregate gpuWorkload =
			ConsumeGpuVanillaLayoutWorkloadAggregate();
		const std::array<UInt64, kGpuTimingCounterCount> gpuTimingCounters =
			ConsumeGpuTimingCounters();
		const auto gpuCounterValue = [&gpuTimingCounters](
			GpuTimingCounter counter)
		{
			return gpuTimingCounters[static_cast<size_t>(counter)];
		};
		const DurationSummary& layout =
			phaseValue(FreeTypePerfPhase::Layout);
		const DurationSummary& artifactCompile =
			phaseValue(FreeTypePerfPhase::TextArtifactCompile);
		const DurationSummary& frameRoute =
			phaseValue(FreeTypePerfPhase::FrameRouteTotal);
		const DurationSummary& framePrep =
			phaseValue(FreeTypePerfPhase::FrameRoutePrep);
		const DurationSummary& vanillaRender =
			phaseValue(FreeTypePerfPhase::FrameRouteVanillaRender);
		const DurationSummary& dispatch =
			phaseValue(FreeTypePerfPhase::DispatchRoute);

		const UInt64 structuralMismatches =
			counterValue(FreeTypePerfCounter::StructuralReadinessHookMismatch)
			+ counterValue(
				FreeTypePerfCounter::StructuralReadinessRendererMismatch)
			+ counterValue(
				FreeTypePerfCounter::StructuralReadinessTileCallbackMismatch)
			+ counterValue(
				FreeTypePerfCounter::StructuralReadinessRenderAlphaMismatch)
			+ counterValue(
				FreeTypePerfCounter::StructuralReadinessImmediateMismatch)
			+ counterValue(
				FreeTypePerfCounter::StructuralReadinessAtlasMismatch)
			+ counterValue(
				FreeTypePerfCounter::ThinRegistrationHookMismatch);
		const UInt64 gpuFailures =
			gpuCounterValue(GpuTimingCounter::CreateFailure)
			+ gpuCounterValue(GpuTimingCounter::IssueFailure)
			+ gpuCounterValue(GpuTimingCounter::ReadFailure)
			+ gpuCounterValue(GpuTimingCounter::Disjoint)
			+ gpuCounterValue(GpuTimingCounter::InvalidRange)
			+ gpuCounterValue(GpuTimingCounter::ResetDiscarded);

		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_summary: text_artifacts=%llu rasterized=%llu atlas_uploads=%llu visibility_checks=%llu visibility_culled=%llu vanilla_draws=%llu dispatch_local_vanilla_culls=%llu standard_lite_replays=%llu direct_draw_lite_replays=%llu command_replays=%llu",
			counterValue(FreeTypePerfCounter::TextArtifactCompile),
			counterValue(FreeTypePerfCounter::BitmapRasterized),
			counterValue(FreeTypePerfCounter::AtlasUpload),
			counterValue(FreeTypePerfCounter::VisibilityCheck),
			counterValue(FreeTypePerfCounter::VisibilityCulled),
			counterValue(FreeTypePerfCounter::VanillaLayoutDraw),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutDispatchLocalCull),
			counterValue(FreeTypePerfCounter::VanillaLayoutStandardLiteReplay),
			counterValue(FreeTypePerfCounter::NativeDirectDrawLiteReplay),
			counterValue(FreeTypePerfCounter::CommandNativeReplay));
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_cpu: layout_n=%llu mean_us=%.3f p95_us=%.3f artifact_n=%llu mean_us=%.3f p95_us=%.3f frame_n=%llu mean_us=%.3f p95_us=%.3f prep_mean_us=%.3f vanilla_mean_us=%.3f dispatch_n=%llu mean_us=%.3f p95_us=%.3f",
			layout.count, layout.meanMicroseconds, layout.p95Microseconds,
			artifactCompile.count, artifactCompile.meanMicroseconds,
			artifactCompile.p95Microseconds,
			frameRoute.count, frameRoute.meanMicroseconds,
			frameRoute.p95Microseconds,
			framePrep.meanMicroseconds, vanillaRender.meanMicroseconds,
			dispatch.count, dispatch.meanMicroseconds,
			dispatch.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_gpu: envelope_n=%llu p95_us=%.3f max_us=%.3f vanilla_n=%llu p95_us=%.3f max_us=%.3f samples=%llu foreign_passes=%llu vanilla_passes=%llu standard_lite_draws=%llu stock_draws=%llu culls=%llu submitted=%llu completed=%llu in_flight=%llu",
			gpuAlphaEnvelope.count, gpuAlphaEnvelope.p95Microseconds,
			gpuAlphaEnvelope.maximumMicroseconds,
			gpuVanillaLayoutEnvelope.count,
			gpuVanillaLayoutEnvelope.p95Microseconds,
			gpuVanillaLayoutEnvelope.maximumMicroseconds,
			gpuWorkload.samples, gpuWorkload.foreignPasses,
			gpuWorkload.vanillaPasses,
			gpuWorkload.vanillaStandardLiteDraws,
			gpuWorkload.vanillaStockDraws, gpuWorkload.vanillaCulls,
			gpuCounterValue(GpuTimingCounter::Submitted),
			gpuCounterValue(GpuTimingCounter::Completed),
			state.gpuQueriesInFlight.load(std::memory_order_relaxed));
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_health: standard_lite_fallbacks=%llu runtime_fallbacks=%llu draw_token_rejected=%llu sidecar_fallbacks=%llu structural_mismatches=%llu gpu_failures=%llu ring_full=%llu",
			counterValue(FreeTypePerfCounter::VanillaLayoutStandardLiteFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutRuntimeFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutDrawTokenRejected),
			counterValue(FreeTypePerfCounter::PreparedSidecarCaptureFallback)
				+ counterValue(
					FreeTypePerfCounter::PreparedSidecarRejectedFallback),
			structuralMismatches, gpuFailures,
			gpuCounterValue(GpuTimingCounter::RingFull));
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
