#pragma once

#include <atomic>

namespace fonthook::vectorfont::implementation
{
	struct RendererResetRecoveryClaim
	{
		bool orphanedBefore = false;
		bool recoveryPending = false;
	};

	// NiDX9Renderer does not guarantee that a successful before callback will
	// receive its matching after callback.  This latch keeps the renderer-facing
	// fast path closed during a live reset, but lets a later main-loop pump claim
	// and recover an interrupted transaction exactly once.
	class RendererResetRecoveryLatch
	{
	public:
		void Begin() noexcept
		{
			m_recoveryPending.store(false, std::memory_order_release);
			m_inProgress.store(true, std::memory_order_release);
		}

		void Complete() noexcept
		{
			m_recoveryPending.store(false, std::memory_order_release);
			m_inProgress.store(false, std::memory_order_release);
		}

		bool DeferInterruptedRecovery() noexcept
		{
			if (!m_inProgress.exchange(false, std::memory_order_acq_rel))
				return false;
			m_recoveryPending.store(true, std::memory_order_release);
			return true;
		}

		RendererResetRecoveryClaim ClaimForMainLoop() noexcept
		{
			RendererResetRecoveryClaim claim;
			claim.orphanedBefore = m_inProgress.exchange(
				false, std::memory_order_acq_rel);
			if (claim.orphanedBefore)
			{
				m_recoveryPending.store(true, std::memory_order_release);
			}
			claim.recoveryPending = m_recoveryPending.exchange(
				false, std::memory_order_acq_rel);
			return claim;
		}

		bool InProgress() const noexcept
		{
			return m_inProgress.load(std::memory_order_acquire);
		}

	private:
		std::atomic<bool> m_inProgress{ false };
		std::atomic<bool> m_recoveryPending{ false };
	};
}
