#include "font_prewarm_run_control.h"

#include <iostream>
#include <string_view>

namespace
{
	using fonthook::vectorfont::implementation::font_prewarm::
		PrewarmProgressWatchdog;
	using fonthook::vectorfont::implementation::font_prewarm::
		PrewarmRunControl;
	using fonthook::vectorfont::implementation::font_prewarm::
		PrewarmWatchdogReason;

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
	PrewarmRunControl run;
	run.Begin(41);
	Expect(run.IsCurrent(41), "new run token must become current");
	Expect(run.CanCommit(41), "new run must initially be allowed to commit");
	const std::uint64_t heartbeat = run.Heartbeat();
	run.Beat(41);
	Expect(run.Heartbeat() == heartbeat + 1,
		"current worker progress must advance the heartbeat");

	run.RequestStop(40);
	Expect(run.CanCommit(41), "a stale cancellation must not close the current run");
	run.RequestStop(41);
	Expect(run.StopRequested(41), "matching cancellation must stop the run");
	Expect(!run.CanCommit(41),
		"matching cancellation must close the late-completion commit gate");
	run.MarkExited(40);
	Expect(!run.HasExited(41), "a stale worker must not acknowledge current exit");
	run.MarkExited(41);
	Expect(run.HasExited(41), "the current worker must acknowledge its exit");

	run.Begin(42);
	Expect(!run.IsCurrent(41), "a superseded token must become stale");
	Expect(!run.CanCommit(41), "a superseded worker must never commit");
	run.RequestStop(41);
	Expect(run.CanCommit(42), "the replacement run must own the commit gate");
	run.CloseCommit(42);
	Expect(!run.CanCommit(42), "closing publication must reject late commits");
	Expect(!run.StopRequested(42),
		"normal publication closure must not misreport a watchdog cancellation");

	PrewarmProgressWatchdog watchdog(1000, 7, 300, 1000);
	Expect(watchdog.Observe(1200, 7) == PrewarmWatchdogReason::None,
		"a run inside both deadlines must continue");
	Expect(watchdog.Observe(1250, 8) == PrewarmWatchdogReason::None,
		"a heartbeat must refresh the no-progress deadline");
	Expect(watchdog.Observe(1549, 8) == PrewarmWatchdogReason::None,
		"no-progress timeout must not fire early");
	Expect(watchdog.Observe(1550, 8) == PrewarmWatchdogReason::NoProgress,
		"a stalled heartbeat must trigger the no-progress watchdog");

	PrewarmProgressWatchdog overall(2000, 1, 5000, 1000);
	Expect(overall.Observe(2999, 20) == PrewarmWatchdogReason::None,
		"overall deadline must not fire early even with progress");
	Expect(overall.Observe(3000, 21)
		== PrewarmWatchdogReason::OverallDeadline,
		"overall deadline must cap a continuously progressing run");

	if (s_failures)
	{
		std::cerr << s_failures << " prewarm run control test(s) failed\n";
		return 1;
	}
	std::cout << "prewarm run control tests passed\n";
	return 0;
}
