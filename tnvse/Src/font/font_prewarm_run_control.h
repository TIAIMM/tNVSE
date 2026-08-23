#pragma once

#include <atomic>
#include <cstdint>

namespace fonthook::vectorfont::implementation::font_prewarm
{
	// A prewarm run owns every asynchronous request that it creates.  Closing the
	// commit token makes late completions observable but unable to publish into a
	// newer run (or into normal runtime-demand rendering after a watchdog abort).
	class PrewarmRunControl final
	{
	public:
		void Begin(std::uint64_t token) noexcept
		{
			if (!token)
				token = 1;
			m_activeToken.store(0, std::memory_order_release);
			m_stopToken.store(0, std::memory_order_relaxed);
			m_exitedToken.store(0, std::memory_order_relaxed);
			m_heartbeat.store(1, std::memory_order_relaxed);
			m_commitToken.store(token, std::memory_order_relaxed);
			m_activeToken.store(token, std::memory_order_release);
		}

		[[nodiscard]] std::uint64_t ActiveToken() const noexcept
		{
			return m_activeToken.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool IsCurrent(std::uint64_t token) const noexcept
		{
			return token && ActiveToken() == token;
		}

		[[nodiscard]] bool StopRequested(std::uint64_t token) const noexcept
		{
			return !IsCurrent(token)
				|| m_stopToken.load(std::memory_order_acquire) == token;
		}

		[[nodiscard]] bool CanCommit(std::uint64_t token) const noexcept
		{
			return IsCurrent(token)
				&& m_stopToken.load(std::memory_order_acquire) != token
				&& m_commitToken.load(std::memory_order_acquire) == token;
		}

		void Beat(std::uint64_t token) noexcept
		{
			if (IsCurrent(token))
				m_heartbeat.fetch_add(1, std::memory_order_release);
		}

		[[nodiscard]] std::uint64_t Heartbeat() const noexcept
		{
			return m_heartbeat.load(std::memory_order_acquire);
		}

		void RequestStop(std::uint64_t token) noexcept
		{
			if (!IsCurrent(token))
				return;
			CloseCommit(token);
			m_stopToken.store(token, std::memory_order_release);
		}

		void CloseCommit(std::uint64_t token) noexcept
		{
			if (!IsCurrent(token))
				return;
			std::uint64_t expected = token;
			m_commitToken.compare_exchange_strong(expected, 0,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}

		void MarkExited(std::uint64_t token) noexcept
		{
			if (IsCurrent(token))
				m_exitedToken.store(token, std::memory_order_release);
		}

		[[nodiscard]] bool HasExited(std::uint64_t token) const noexcept
		{
			return token
				&& m_exitedToken.load(std::memory_order_acquire) == token;
		}

	private:
		std::atomic<std::uint64_t> m_activeToken{ 0 };
		std::atomic<std::uint64_t> m_commitToken{ 0 };
		std::atomic<std::uint64_t> m_heartbeat{ 0 };
		std::atomic<std::uint64_t> m_exitedToken{ 0 };
		std::atomic<std::uint64_t> m_stopToken{ 0 };
	};

	enum class PrewarmWatchdogReason : std::uint8_t
	{
		None,
		NoProgress,
		OverallDeadline,
	};

	// Tick values use unsigned subtraction so callers can pass GetTickCount64()
	// directly.  A heartbeat is progress even when the public phase is unchanged.
	class PrewarmProgressWatchdog final
	{
	public:
		PrewarmProgressWatchdog(std::uint64_t startedAt,
			std::uint64_t heartbeat, std::uint64_t noProgressMs,
			std::uint64_t overallMs) noexcept
			: m_startedAt(startedAt), m_lastProgressAt(startedAt),
			m_observedHeartbeat(heartbeat), m_noProgressMs(noProgressMs),
			m_overallMs(overallMs)
		{}

		[[nodiscard]] PrewarmWatchdogReason Observe(std::uint64_t now,
			std::uint64_t heartbeat) noexcept
		{
			if (heartbeat != m_observedHeartbeat)
			{
				m_observedHeartbeat = heartbeat;
				m_lastProgressAt = now;
			}
			if (m_overallMs && now - m_startedAt >= m_overallMs)
				return PrewarmWatchdogReason::OverallDeadline;
			if (m_noProgressMs && now - m_lastProgressAt >= m_noProgressMs)
				return PrewarmWatchdogReason::NoProgress;
			return PrewarmWatchdogReason::None;
		}

		[[nodiscard]] std::uint64_t LastProgressAt() const noexcept
		{
			return m_lastProgressAt;
		}

	private:
		std::uint64_t m_startedAt = 0;
		std::uint64_t m_lastProgressAt = 0;
		std::uint64_t m_observedHeartbeat = 0;
		std::uint64_t m_noProgressMs = 0;
		std::uint64_t m_overallMs = 0;
	};
}
