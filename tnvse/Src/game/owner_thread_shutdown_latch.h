#pragma once

#include <atomic>
#include <cstdint>

namespace fonthook::implementation::native_tile_overlay
{
	// Coordinates destruction of state that may only be touched by a dedicated
	// owner thread.  Acknowledgement is not quiescence until the owner has also
	// left its current critical work scope.
	class OwnerThreadShutdownLatch
	{
	public:
		bool Request(std::uint32_t sequence) noexcept
		{
			if (!sequence)
				return false;
			std::uint32_t expected = 0;
			return m_requestedSequence.compare_exchange_strong(
				expected, sequence, std::memory_order_acq_rel);
		}

		std::uint32_t RequestedSequence() const noexcept
		{
			return m_requestedSequence.load(std::memory_order_acquire);
		}

		void EnterOwnerWork() noexcept
		{
			m_ownerWorkInFlight.fetch_add(1, std::memory_order_acq_rel);
		}

		void LeaveOwnerWork() noexcept
		{
			m_ownerWorkInFlight.fetch_sub(1, std::memory_order_acq_rel);
		}

		void Acknowledge(std::uint32_t sequence) noexcept
		{
			if (sequence && RequestedSequence() == sequence)
			{
				m_acknowledgedSequence.store(
					sequence, std::memory_order_release);
			}
		}

		bool IsQuiesced(std::uint32_t sequence) const noexcept
		{
			return sequence
				&& m_acknowledgedSequence.load(std::memory_order_acquire)
					== sequence
				&& m_ownerWorkInFlight.load(std::memory_order_acquire) == 0;
		}

		std::uint32_t OwnerWorkInFlight() const noexcept
		{
			return m_ownerWorkInFlight.load(std::memory_order_acquire);
		}

	private:
		std::atomic<std::uint32_t> m_requestedSequence{ 0 };
		std::atomic<std::uint32_t> m_acknowledgedSequence{ 0 };
		std::atomic<std::uint32_t> m_ownerWorkInFlight{ 0 };
	};
}
