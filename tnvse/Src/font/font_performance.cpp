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
			Count,
		};
		constexpr size_t kGpuTimingCounterCount =
			static_cast<size_t>(GpuTimingCounter::Count);
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
			std::array<std::atomic<UInt64>, kGpuTimingCounterCount>
				gpuTimingCounters = {};
			std::atomic<UInt64> gpuQueriesInFlight = 0;
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

		struct GpuQuerySlot
		{
			IDirect3DQuery9* disjoint = nullptr;
			IDirect3DQuery9* frequency = nullptr;
			IDirect3DQuery9* beginTimestamp = nullptr;
			IDirect3DQuery9* endTimestamp = nullptr;
			bool pending = false;
		};

		struct GpuQueryState
		{
			IDirect3DDevice9* device = nullptr;
			std::array<GpuQuerySlot, kGpuQueryRingSize> slots = {};
			size_t nextSlot = 0;
			size_t activeSlot = std::numeric_limits<size_t>::max();
			bool unavailable = false;
			bool loggedReady = false;
		};

		GpuQueryState& GetGpuQueryState()
		{
			static GpuQueryState* state = new GpuQueryState();
			return *state;
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
		}

		void ResetGpuQueryState(bool countDiscarded)
		{
			GpuQueryState& state = GetGpuQueryState();
			UInt64 discarded = 0;
			UInt64 pendingDiscarded = 0;
			for (size_t index = 0; index < state.slots.size(); ++index)
			{
				GpuQuerySlot& slot = state.slots[index];
				if (slot.pending)
					++pendingDiscarded;
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
			state.device = nullptr;
			state.nextSlot = 0;
			state.activeSlot = std::numeric_limits<size_t>::max();
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
					GetPerformanceState().gpuQueriesInFlight.fetch_sub(
						1, std::memory_order_relaxed);
					RecordGpuTimingCounter(GpuTimingCounter::ReadFailure);
					ReleaseGpuQuerySlot(slot);
					continue;
				}
				slot.pending = false;
				GetPerformanceState().gpuQueriesInFlight.fetch_sub(
					1, std::memory_order_relaxed);
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
			}
		}

		bool BeginGpuAlphaEnvelope(IDirect3DDevice9* device)
		{
			if (!g_bEnableFreeTypeFontRenderingLog || !device)
				return false;
			GpuQueryState& state = GetGpuQueryState();
			if (state.device != device)
			{
				ResetGpuQueryState(true);
				state.device = device;
			}
			if (state.unavailable)
				return false;
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
						"tnvse_freetype_gpu_timing: enabled scope=tile_alpha_envelope async=1 flush=0 ring=%u disjoint_validation=1",
						static_cast<UInt32>(state.slots.size()));
				}
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

		DurationSummary ConsumeGpuAlphaEnvelopeSummary()
		{
			PerformanceState& state = GetPerformanceState();
			std::array<UInt64, kDurationBuckets> values = {};
			DurationSummary result;
			for (size_t bucket = 0; bucket < values.size(); ++bucket)
			{
				values[bucket] = state.gpuAlphaEnvelopeDurations[bucket]
					.exchange(0, std::memory_order_relaxed);
				result.count += values[bucket];
			}
			const UInt64 totalNanoseconds =
				state.gpuAlphaEnvelopeNanoseconds.exchange(
					0, std::memory_order_relaxed);
			const UInt64 maximumNanoseconds =
				state.gpuAlphaEnvelopeMaximumNanoseconds.exchange(
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

	FreeTypeGpuAlphaEnvelopeScope::FreeTypeGpuAlphaEnvelopeScope(
		IDirect3DDevice9* device, bool enabled)
		: m_active(enabled && BeginGpuAlphaEnvelope(device))
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
		if (!m_active)
			return;
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
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			GetPerformanceState().counters[
				static_cast<size_t>(counter)].fetch_add(
					amount, std::memory_order_relaxed);
		}
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
			"tnvse_freetype_perf: bitmap_mem=%llu cross_font=%llu disk_hit=%llu miss=%llu write=%llu read_bytes=%llu write_bytes=%llu raster=%llu bitmap_batch_requests=%llu deduped=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu upload_rects=%llu text_artifact_hit=%llu miss=%llu hot=%llu bypass=%llu admitted=%llu evicted=%llu shader_batches=%llu cpu_effect_masks_avoided=%llu gpu_resident_glyph_hit=%llu miss=%llu atlas_snapshot_profile_reuse=%llu dynamic_vb_uploads=%llu bytes=%llu reuse=%llu discards=%llu static_vb_uploads=%llu bytes=%llu hits=%llu promotion_failed=%llu sorted_static_batches=%llu payloads=%llu bytes=%llu merged_packet_ranges=%llu metadata_hot=%llu locked=%llu sorted_facades=%llu unique_payloads=%llu frame_lookup_hits=%llu preflight_fast=%llu full=%llu direct_static=%llu direct_dynamic=%llu sorted_dynamic_batches=%llu payloads=%llu bytes=%llu lockless_packets=%llu visibility_checks=%llu culled=%llu app=%llu alpha=%llu clip=%llu scissor=%llu preflight_skipped=%llu packets_saved=%llu vertices_saved=%llu direct_shape_candidates=%llu direct_shape_draws=%llu direct_shape_vertices=%llu direct_shape_fallback=%llu constant_ownership_segments=%llu reuses=%llu releases=%llu snapshot_gets_elided=%llu restore_sets_elided=%llu vanilla_constant_updates=%llu reuses=%llu composite_constant_full=%llu private_reuses=%llu partial=%llu vanilla_c0_republish_elided=%llu compat_republishes=%llu private_registers_uploaded=%llu full_tail_elided=%llu foreign_pass_private_invalidations=%llu vanilla_layout_private_preserves=%llu sampler_sets=%llu reuses=%llu composite_onequad_single_page=%llu onequad_paged=%llu onequad_build_fallback=%llu legacy_multipage_fallback=%llu shader_fallback=%llu composite_draws=%llu tile_passes=%llu cache_hit=%llu miss=%llu state_changes=%llu generated=%llu evicted=%llu cache_bytes=%llu budget_reject=%llu rtt_fail=%llu restore_fail=%llu visual_validated=%llu rejected=%llu inconclusive=%llu",
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
			"tnvse_freetype_preflight_clip_cull: checks=%llu culled=%llu viewport=%llu scissor=%llu fail_open=%llu honored=%llu revoked=%llu transform_hits=%llu transform_misses=%llu transform_identity_misses=%llu transform_key_misses=%llu transform_unavailable=%llu vanilla_ui_ortho_translation=%llu generic_transforms=%llu",
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
				VisibilityPreflightClipRevoked),
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
				VisibilityPreflightClipGenericTransform));
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
			"tnvse_freetype_accumulator_prep: empty_fast=%llu metadata_cull_skipped=%llu no_prepared_payload=%llu",
			counterValue(FreeTypePerfCounter::AccumulatorEmptyFastPath),
			counterValue(FreeTypePerfCounter::AccumulatorMetadataCullSkipped),
			counterValue(FreeTypePerfCounter::AccumulatorNoPreparedPayload));
		FreeTypeFontDebugLog(
			"tnvse_freetype_vanilla_layout_sdf: eligible=%llu created=%llu create_fallback=%llu draws=%llu culls=%llu runtime_fallback=%llu vertices=%llu shifted_eligible=%llu shifted_created=%llu shifted_draws=%llu shifted_runtime_fallback=%llu precache_accepted=%llu precache_immediate=%llu precache_deferred=%llu precache_rejected=%llu payload_upload_attempts=%llu success=%llu failure=%llu bytes=%llu native_pack_pending=%llu postpack_completion_ready_checks=%llu prior_generation_decl_ready_checks=%llu private_state_carries=%llu private_state_carry_rejected=%llu draw_token_hits=%llu draw_token_full_validations=%llu draw_token_cold=%llu draw_token_shape_shader_invalidations=%llu draw_token_generation_invalidations=%llu draw_token_geometry_invalidations=%llu draw_token_native_pack_invalidations=%llu draw_token_layout_invalidations=%llu draw_token_first_certifications=%llu draw_token_recertifications=%llu draw_token_rejected=%llu",
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfCandidate),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfCreated),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfDraw),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfCull),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfRuntimeFallback),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfVertex),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfShiftedCandidate),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfShiftedCreated),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfShiftedDraw),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfShiftedRuntimeFallback),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPrecacheAccepted),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPrecacheImmediate),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPrecacheDeferred),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPrecacheRejected),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadAttempt),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadSuccess),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadFailure),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadBytes),
			counterValue(
				FreeTypePerfCounter::VanillaLayoutSdfNativePackPending),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfPostpackCompletionReady),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfPriorGenerationDeclarationReady),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfPrivateStateCarry),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfPrivateStateCarryRejected),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfDrawTokenHit),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenFullValidation),
			counterValue(FreeTypePerfCounter::VanillaLayoutSdfDrawTokenCold),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenShapeShaderInvalidation),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenGenerationInvalidation),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenGeometryInvalidation),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenNativePackInvalidation),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenLayoutInvalidation),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenFirstCertification),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenRecertification),
			counterValue(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenRejected));
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
			"tnvse_freetype_perf: segment_device_state_starts=%llu segment_device_state_reuses=%llu pass_sets=%llu pass_reuses=%llu constants_sets=%llu constants_reuses=%llu constants_lite_replays=%llu constants_lite_fallbacks=%llu constants_lite_scaled_fallbacks=%llu blend_sets=%llu blend_reuses=%llu alpha_test_sets=%llu alpha_test_reuses=%llu drawmode_sets=%llu drawmode_reuses=%llu post_calls=%llu post_elisions=%llu",
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
				FreeTypePerfCounter::SegmentDeviceDrawmodeSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceDrawmodeReuse)],
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
			"tnvse_freetype_perf_timing: layout_n=%llu median_us=%.3f p95_us=%.3f sidecar_n=%llu median_us=%.3f p95_us=%.3f direct_compile_n=%llu median_us=%.3f p95_us=%.3f native_registration_n=%llu median_us=%.3f p95_us=%.3f preflight_n=%llu median_us=%.3f p95_us=%.3f submit_n=%llu median_us=%.3f p95_us=%.3f command_build_n=%llu median_us=%.3f p95_us=%.3f command_submit_n=%llu median_us=%.3f p95_us=%.3f extended_fnt_geometry_n=%llu median_us=%.3f p95_us=%.3f",
			layout.count, layout.medianMicroseconds,
			layout.p95Microseconds,
			sidecar.count, sidecar.medianMicroseconds,
			sidecar.p95Microseconds,
			directCompile.count,
			directCompile.medianMicroseconds,
			directCompile.p95Microseconds,
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
			"tnvse_freetype_dispatch_timing: dispatch_route_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f register_route_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f honor_gate_n=%llu median_us=%.3f p95_us=%.3f",
			dispatchRoute.count, dispatchRoute.meanMicroseconds,
			dispatchRoute.medianMicroseconds,
			dispatchRoute.p95Microseconds,
			registerRoute.count, registerRoute.meanMicroseconds,
			registerRoute.medianMicroseconds,
			registerRoute.p95Microseconds,
			preflightClipHonorGate.count,
			preflightClipHonorGate.medianMicroseconds,
			preflightClipHonorGate.p95Microseconds);
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
