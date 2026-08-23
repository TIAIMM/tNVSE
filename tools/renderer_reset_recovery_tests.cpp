#include "font_reset_recovery_latch.h"

#include <iostream>
#include <string_view>

namespace
{
	using fonthook::vectorfont::implementation::RendererResetRecoveryLatch;

	int s_failures = 0;

	void Expect(bool condition, std::string_view message)
	{
		if (condition)
			return;
		std::cerr << "FAIL " << message << '\n';
		++s_failures;
	}
}

int main()
{
	RendererResetRecoveryLatch latch;
	Expect(!latch.InProgress(), "new latch must be idle");
	Expect(!latch.ClaimForMainLoop().recoveryPending,
		"idle latch must not request recovery");

	latch.Begin();
	Expect(latch.InProgress(), "before phase must close the renderer fast path");
	latch.Complete();
	const auto completed = latch.ClaimForMainLoop();
	Expect(!completed.orphanedBefore && !completed.recoveryPending,
		"paired after phase must not be recovered again");

	latch.Begin();
	Expect(latch.DeferInterruptedRecovery(),
		"known callback cancellation must defer one rebuild");
	Expect(!latch.InProgress(),
		"known callback cancellation must reopen the fast path");
	Expect(!latch.DeferInterruptedRecovery(),
		"the same interrupted before phase must not be deferred twice");
	const auto deferred = latch.ClaimForMainLoop();
	Expect(!deferred.orphanedBefore && deferred.recoveryPending,
		"deferred cancellation must force the next main-loop rebuild");
	Expect(!latch.ClaimForMainLoop().recoveryPending,
		"deferred recovery must be claimed exactly once");

	latch.Begin();
	const auto orphaned = latch.ClaimForMainLoop();
	Expect(orphaned.orphanedBefore && orphaned.recoveryPending,
		"an unpaired before phase must be recognized as interrupted");
	Expect(!latch.InProgress(),
		"claiming an orphaned before phase must reopen the fast path");
	Expect(!latch.ClaimForMainLoop().recoveryPending,
		"orphaned recovery must be claimed exactly once");

	latch.Begin();
	Expect(latch.DeferInterruptedRecovery(),
		"first reset attempt must be deferrable");
	latch.Begin();
	latch.Complete();
	Expect(!latch.ClaimForMainLoop().recoveryPending,
		"a later complete reset must supersede an older deferred rebuild");

	if (s_failures)
	{
		std::cerr << s_failures << " renderer reset recovery test(s) failed\n";
		return 1;
	}
	std::cout << "renderer reset recovery tests passed\n";
	return 0;
}
