#include "font_native_ring_detail.h"

#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	using namespace implementation::font_native_ring;

	namespace implementation::font_native_ring
	{
		NativeFontRingThreadState& RingThread()
		{
			thread_local NativeFontRingThreadState state;
			return state;
		}

		NativeFontSortedRingLease& SortedRingLease()
		{
			thread_local NativeFontSortedRingLease lease;
			return lease;
		}

		NativeFontRingState& RingState()
		{
			// Renderer generations are process-lifetime objects. Keep the equally small
			// proxy pool alive for the same interval so raw geometry/shader links cannot
			// be torn down during late engine shutdown.
			static NativeFontRingState* state = new NativeFontRingState();
			return *state;
		}
	}

	void EndNativeFontSortedRingFrame()
	{
		if (!SortedRingLease().active)
			return;
		NativeFontRingState* state = SortedRingLease().state;
		if (state && state->activeSubmissions.load(std::memory_order_acquire))
			return;
		SortedRingLease() = {};
		if (!state)
			return;
		const UInt32 previous = state->sortedFrameLeases.fetch_sub(
			1, std::memory_order_acq_rel);
		if (previous <= 1
			&& state->releasePending.load(std::memory_order_acquire))
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->sortedFrameLeases.load(std::memory_order_acquire)
				&& !state->activeSubmissions.load(std::memory_order_acquire))
			{
				if (state->releasePending.load(std::memory_order_acquire))
					ReleaseRingResourcesLocked(*state);
			}
		}
	}
	void ReleaseNativeFontRingResources()
	{
		NativeFontRingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sortedFrameLeases.load(std::memory_order_acquire)
			|| state.activeSubmissions.load(std::memory_order_acquire))
		{
			if (!state.releasePending.exchange(true,
				std::memory_order_acq_rel))
				AdvanceResourceSerialLocked(state);
			return;
		}
		ReleaseRingResourcesLocked(state);
	}
}
