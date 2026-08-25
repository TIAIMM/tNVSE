#pragma once

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace fonthook::implementation::native_tile_overlay
{
	enum class PrewarmOverlayAction : std::uint8_t
	{
		Present,
		Suspend,
		Close,
	};

	enum class PrewarmOverlayCloseReason : std::uint8_t
	{
		Completed,
		Cancelled,
		Failed,
		Watchdog,
		Shutdown,
	};

	struct PrewarmOverlayCommand
	{
		std::uint64_t runToken = 0;
		std::uint32_t sequence = 0;
		float progress = 0.0f;
		PrewarmOverlayAction action = PrewarmOverlayAction::Suspend;
		PrewarmOverlayCloseReason closeReason =
			PrewarmOverlayCloseReason::Completed;
	};

	struct PrewarmOverlayPublishResult
	{
		std::uint32_t sequence = 0;
		float progress = 0.0f;
		std::uint8_t milestones = 0;
		bool openedRun = false;
		bool resumedRun = false;

		explicit operator bool() const noexcept
		{
			return sequence != 0;
		}
	};

	enum class PrewarmOverlayInstanceState : std::uint8_t
	{
		Detached,
		Attached,
		Suspended,
		Disabled,
	};

	enum class PrewarmOverlayHostResult : std::uint8_t
	{
		Ready,
		HostChanged,
		Disabled,
	};

	template <class TilePointer>
	struct BasicPrewarmOverlayInstance
	{
		std::uint64_t runToken = 0;
		TilePointer loadingRoot = nullptr;
		TilePointer componentRoot = nullptr;
		std::uint32_t progressTrait = 0;
		PrewarmOverlayInstanceState state =
			PrewarmOverlayInstanceState::Detached;

		PrewarmOverlayHostResult BeginCommand(
			std::uint64_t commandRunToken,
			TilePointer currentLoadingRoot) noexcept
		{
			if (runToken != commandRunToken)
				Forget(commandRunToken, PrewarmOverlayInstanceState::Detached);
			if (state == PrewarmOverlayInstanceState::Disabled)
				return PrewarmOverlayHostResult::Disabled;
			if (loadingRoot && loadingRoot != currentLoadingRoot)
			{
				Forget(commandRunToken, PrewarmOverlayInstanceState::Disabled);
				return PrewarmOverlayHostResult::HostChanged;
			}
			return PrewarmOverlayHostResult::Ready;
		}

		void Attach(
			TilePointer host, TilePointer component,
			std::uint32_t trait) noexcept
		{
			loadingRoot = host;
			componentRoot = component;
			progressTrait = trait;
			state = PrewarmOverlayInstanceState::Attached;
		}

		void Suspend() noexcept
		{
			state = PrewarmOverlayInstanceState::Suspended;
		}

		void Disable(std::uint64_t commandRunToken) noexcept
		{
			Forget(commandRunToken, PrewarmOverlayInstanceState::Disabled);
		}

		void Close() noexcept
		{
			Forget(0, PrewarmOverlayInstanceState::Detached);
		}

		void Forget(
			std::uint64_t commandRunToken,
			PrewarmOverlayInstanceState nextState) noexcept
		{
			runToken = commandRunToken;
			loadingRoot = nullptr;
			componentRoot = nullptr;
			progressTrait = 0;
			state = nextState;
		}
	};

	// A latest-state mailbox deliberately coalesces intermediate progress. The
	// SRW lock protects the complete snapshot while the two atomics make the
	// LoadingMenu Update hot path a single acquire comparison when idle.
	class PrewarmOverlayMailbox final
	{
	public:
		PrewarmOverlayPublishResult PublishProgress(
			std::uint64_t runToken, float progress) noexcept
		{
			PrewarmOverlayPublishResult result;
			if (!runToken)
				return result;

			AcquireSRWLockExclusive(&lock_);
			if (currentRunToken_ && runToken < currentRunToken_)
			{
				ReleaseSRWLockExclusive(&lock_);
				return result;
			}

			if (runToken != currentRunToken_)
			{
				currentRunToken_ = runToken;
				maximumProgress_ = 0.0f;
				milestones_ = 0;
				terminal_ = false;
				disabled_ = false;
				opened_ = false;
			}
			if (terminal_ || disabled_)
			{
				ReleaseSRWLockExclusive(&lock_);
				return result;
			}

			if (!std::isfinite(progress))
				progress = maximumProgress_;
			progress = std::clamp(progress, 0.0f, 1.0f);
			progress = std::max(progress, maximumProgress_);
			const bool openedRun = !opened_;
			const bool resumedRun = opened_
				&& command_.action == PrewarmOverlayAction::Suspend;
			if (openedRun)
				opened_ = true;

			if (!openedRun
				&& command_.runToken == runToken
				&& command_.action == PrewarmOverlayAction::Present
				&& command_.progress == progress)
			{
				presentationRequested_.store(true, std::memory_order_release);
				ReleaseSRWLockExclusive(&lock_);
				return result;
			}

			maximumProgress_ = progress;
			result.sequence = NextSequenceLocked();
			result.progress = progress;
			result.openedRun = openedRun;
			result.resumedRun = resumedRun;
			result.milestones = CollectMilestonesLocked(progress);
			command_ = {
				runToken,
				result.sequence,
				progress,
				PrewarmOverlayAction::Present,
				PrewarmOverlayCloseReason::Completed,
			};
			presentationRequested_.store(true, std::memory_order_release);
			publishedSequence_.store(result.sequence, std::memory_order_release);
			ReleaseSRWLockExclusive(&lock_);
			return result;
		}

		std::uint32_t PublishSuspend(std::uint64_t runToken) noexcept
		{
			if (!runToken)
				return 0;
			AcquireSRWLockExclusive(&lock_);
			if (runToken != currentRunToken_ || !opened_
				|| terminal_ || disabled_
				|| command_.action == PrewarmOverlayAction::Suspend)
			{
				ReleaseSRWLockExclusive(&lock_);
				return 0;
			}
			const std::uint32_t sequence = NextSequenceLocked();
			command_ = {
				runToken,
				sequence,
				maximumProgress_,
				PrewarmOverlayAction::Suspend,
				PrewarmOverlayCloseReason::Completed,
			};
			presentationRequested_.store(false, std::memory_order_release);
			publishedSequence_.store(sequence, std::memory_order_release);
			ReleaseSRWLockExclusive(&lock_);
			return sequence;
		}

		std::uint32_t PublishClose(
			std::uint64_t runToken,
			PrewarmOverlayCloseReason reason) noexcept
		{
			if (!runToken)
				return 0;
			AcquireSRWLockExclusive(&lock_);
			if (runToken != currentRunToken_ || !opened_ || terminal_)
			{
				ReleaseSRWLockExclusive(&lock_);
				return 0;
			}
			terminal_ = true;
			const std::uint32_t sequence = NextSequenceLocked();
			command_ = {
				runToken,
				sequence,
				maximumProgress_,
				PrewarmOverlayAction::Close,
				reason,
			};
			presentationRequested_.store(false, std::memory_order_release);
			publishedSequence_.store(sequence, std::memory_order_release);
			ReleaseSRWLockExclusive(&lock_);
			return sequence;
		}

		void DisableRun(std::uint64_t runToken) noexcept
		{
			AcquireSRWLockExclusive(&lock_);
			if (runToken == currentRunToken_)
			{
				disabled_ = true;
				presentationRequested_.store(false, std::memory_order_release);
			}
			ReleaseSRWLockExclusive(&lock_);
		}

		bool HasPending() const noexcept
		{
			return publishedSequence_.load(std::memory_order_acquire)
				!= consumedSequence_.load(std::memory_order_acquire);
		}

		PrewarmOverlayCommand ReadLatest() const noexcept
		{
			PrewarmOverlayCommand command;
			AcquireSRWLockShared(&lock_);
			command = command_;
			ReleaseSRWLockShared(&lock_);
			return command;
		}

		void MarkConsumed(std::uint32_t sequence) noexcept
		{
			if (sequence)
				consumedSequence_.store(sequence, std::memory_order_release);
		}

		bool IsPresentationRequested() const noexcept
		{
			return presentationRequested_.load(std::memory_order_acquire);
		}

		std::uint32_t PublishedSequence() const noexcept
		{
			return publishedSequence_.load(std::memory_order_acquire);
		}

		std::uint32_t ConsumedSequence() const noexcept
		{
			return consumedSequence_.load(std::memory_order_acquire);
		}

	private:
		std::uint32_t NextSequenceLocked() noexcept
		{
			++nextSequence_;
			if (!nextSequence_)
				++nextSequence_;
			return nextSequence_;
		}

		std::uint8_t CollectMilestonesLocked(float progress) noexcept
		{
			std::uint8_t newlyReached = 0;
			constexpr float thresholds[] = { 0.25f, 0.50f, 0.75f, 1.0f };
			for (std::uint8_t i = 0; i < 4; ++i)
			{
				const std::uint8_t bit = static_cast<std::uint8_t>(1u << i);
				if (progress >= thresholds[i] && !(milestones_ & bit))
				{
					milestones_ |= bit;
					newlyReached |= bit;
				}
			}
			return newlyReached;
		}

		mutable SRWLOCK lock_ = SRWLOCK_INIT;
		PrewarmOverlayCommand command_;
		std::uint64_t currentRunToken_ = 0;
		std::uint32_t nextSequence_ = 0;
		float maximumProgress_ = 0.0f;
		std::uint8_t milestones_ = 0;
		bool opened_ = false;
		bool terminal_ = false;
		bool disabled_ = false;
		std::atomic<std::uint32_t> publishedSequence_{ 0 };
		std::atomic<std::uint32_t> consumedSequence_{ 0 };
		std::atomic_bool presentationRequested_{ false };
	};
}
