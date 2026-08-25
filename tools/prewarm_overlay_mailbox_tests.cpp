#include "prewarm_overlay_mailbox.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{
	using namespace fonthook::implementation::native_tile_overlay;

	bool NearlyEqual(float lhs, float rhs)
	{
		return std::fabs(lhs - rhs) < 0.00001f;
	}

	void TestLatestStateAndTerminalClose()
	{
		PrewarmOverlayMailbox mailbox;
		assert(!mailbox.HasPending());
		assert(!mailbox.IsPresentationRequested());

		const PrewarmOverlayPublishResult opened =
			mailbox.PublishProgress(10, 0.0f);
		assert(opened && opened.openedRun);
		assert(mailbox.HasPending());
		assert(mailbox.IsPresentationRequested());
		assert(!mailbox.PublishProgress(10, 0.0f));

		const PrewarmOverlayPublishResult half =
			mailbox.PublishProgress(10, 0.5f);
		assert(half && NearlyEqual(half.progress, 0.5f));
		assert((half.milestones & 0x3u) == 0x3u);
		assert(!mailbox.PublishProgress(10, 0.25f));
		assert(NearlyEqual(mailbox.ReadLatest().progress, 0.5f));

		const std::uint32_t suspended = mailbox.PublishSuspend(10);
		assert(suspended);
		assert(!mailbox.IsPresentationRequested());
		assert(mailbox.ReadLatest().action == PrewarmOverlayAction::Suspend);
		assert(!mailbox.PublishSuspend(10));

		const PrewarmOverlayPublishResult resumed =
			mailbox.PublishProgress(10, 0.4f);
		assert(resumed && resumed.resumedRun);
		assert(NearlyEqual(resumed.progress, 0.5f));

		const std::uint32_t closed = mailbox.PublishClose(
			10, PrewarmOverlayCloseReason::Completed);
		assert(closed);
		assert(!mailbox.IsPresentationRequested());
		assert(mailbox.ReadLatest().action == PrewarmOverlayAction::Close);
		assert(!mailbox.PublishClose(
			10, PrewarmOverlayCloseReason::Completed));
		assert(!mailbox.PublishProgress(10, 1.0f));
		assert(!mailbox.PublishSuspend(10));
		assert(!mailbox.PublishProgress(9, 1.0f));

		const PrewarmOverlayPublishResult next =
			mailbox.PublishProgress(11, 0.0f);
		assert(next && next.openedRun);
	}

	void TestCloseCoalescesUnconsumedPresent()
	{
		PrewarmOverlayMailbox mailbox;
		BasicPrewarmOverlayInstance<void*> owner;
		assert(mailbox.PublishProgress(20, 0.0f));
		assert(mailbox.PublishClose(
			20, PrewarmOverlayCloseReason::Cancelled));

		const PrewarmOverlayCommand latest = mailbox.ReadLatest();
		assert(latest.action == PrewarmOverlayAction::Close);
		if (latest.action == PrewarmOverlayAction::Present)
		{
			owner.Attach(
				reinterpret_cast<void*>(1),
				reinterpret_cast<void*>(2), 3);
		}
		assert(owner.componentRoot == nullptr);
		mailbox.MarkConsumed(latest.sequence);
		assert(!mailbox.HasPending());
	}

	void TestOwnerPointerPolicy()
	{
		BasicPrewarmOverlayInstance<void*> owner;
		void* hostA = reinterpret_cast<void*>(1);
		void* component = reinterpret_cast<void*>(2);
		void* hostB = reinterpret_cast<void*>(3);

		assert(owner.BeginCommand(30, hostA)
			== PrewarmOverlayHostResult::Ready);
		owner.Attach(hostA, component, 7);
		owner.Suspend();
		assert(owner.componentRoot == component);
		assert(owner.state == PrewarmOverlayInstanceState::Suspended);
		assert(owner.BeginCommand(30, hostB)
			== PrewarmOverlayHostResult::HostChanged);
		assert(owner.loadingRoot == nullptr);
		assert(owner.componentRoot == nullptr);
		assert(owner.state == PrewarmOverlayInstanceState::Disabled);
		assert(owner.BeginCommand(30, hostB)
			== PrewarmOverlayHostResult::Disabled);

		assert(owner.BeginCommand(31, hostB)
			== PrewarmOverlayHostResult::Ready);
		owner.Attach(hostB, component, 9);
		owner.Disable(31); // XML load/validation failure: no retry this run.
		assert(owner.BeginCommand(31, hostB)
			== PrewarmOverlayHostResult::Disabled);
		assert(owner.componentRoot == nullptr);
		owner.Close();
		assert(owner.runToken == 0);
		assert(owner.state == PrewarmOverlayInstanceState::Detached);
	}

	void TestConcurrentSnapshotIntegrity()
	{
		PrewarmOverlayMailbox mailbox;
		std::atomic_bool finished{ false };
		std::atomic_bool torn{ false };
		constexpr std::uint64_t firstToken = 1000;
		constexpr std::uint32_t iterations = 100000;

		std::thread consumer([&]
		{
			while (!finished.load(std::memory_order_acquire)
				|| mailbox.HasPending())
			{
				if (!mailbox.HasPending())
				{
					SwitchToThread();
					continue;
				}
				const PrewarmOverlayCommand command = mailbox.ReadLatest();
				if (command.action != PrewarmOverlayAction::Present
					|| command.runToken < firstToken)
				{
					torn.store(true, std::memory_order_release);
					break;
				}
				const std::uint64_t index = command.runToken - firstToken;
				const float expected =
					static_cast<float>(index % 100u) / 100.0f;
				if (!command.sequence || !NearlyEqual(command.progress, expected))
				{
					torn.store(true, std::memory_order_release);
					break;
				}
				mailbox.MarkConsumed(command.sequence);
			}
		});

		for (std::uint32_t i = 0; i < iterations; ++i)
		{
			const float progress = static_cast<float>(i % 100u) / 100.0f;
			assert(mailbox.PublishProgress(firstToken + i, progress));
		}
		finished.store(true, std::memory_order_release);
		consumer.join();
		assert(!torn.load(std::memory_order_acquire));
	}

	void TestConcurrentPublishOrder()
	{
		PrewarmOverlayMailbox mailbox;
		assert(mailbox.PublishProgress(200000, 0.0f));
		std::atomic_bool stop{ false };
		std::atomic_bool regressed{ false };
		std::thread observer([&]
		{
			while (!stop.load(std::memory_order_acquire))
			{
				const PrewarmOverlayCommand command = mailbox.ReadLatest();
				const std::uint32_t published = mailbox.PublishedSequence();
				if (command.sequence > published)
				{
					regressed.store(true, std::memory_order_release);
					break;
				}
			}
		});

		std::thread low([&]
		{
			for (std::uint32_t i = 1; i <= 50000; ++i)
				mailbox.PublishProgress(200000, i / 100000.0f);
		});
		std::thread high([&]
		{
			for (std::uint32_t i = 50001; i <= 100000; ++i)
				mailbox.PublishProgress(200000, i / 100000.0f);
		});
		low.join();
		high.join();
		stop.store(true, std::memory_order_release);
		observer.join();
		assert(!regressed.load(std::memory_order_acquire));
		assert(NearlyEqual(mailbox.ReadLatest().progress, 1.0f));
	}
}

int main()
{
	TestLatestStateAndTerminalClose();
	TestCloseCoalescesUnconsumedPresent();
	TestOwnerPointerPolicy();
	TestConcurrentSnapshotIntegrity();
	TestConcurrentPublishOrder();
	std::cout << "prewarm overlay mailbox tests passed\n";
	return 0;
}
