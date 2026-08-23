#include "owner_thread_shutdown_latch.h"

#include <iostream>
#include <string_view>

namespace
{
	using fonthook::implementation::native_tile_overlay::
		OwnerThreadShutdownLatch;

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
	OwnerThreadShutdownLatch latch;
	Expect(!latch.Request(0), "zero must not be accepted as a shutdown token");
	Expect(latch.Request(41), "first shutdown token must be accepted");
	Expect(latch.RequestedSequence() == 41,
		"the accepted shutdown token must remain stable");
	Expect(!latch.Request(42), "a later request must not replace the owner token");

	latch.Acknowledge(42);
	Expect(!latch.IsQuiesced(41), "a mismatched acknowledgement must be ignored");
	latch.EnterOwnerWork();
	latch.Acknowledge(41);
	Expect(!latch.IsQuiesced(41),
		"acknowledgement must not publish quiescence while owner work is active");
	Expect(latch.OwnerWorkInFlight() == 1,
		"owner work must be visible to the requesting thread");
	latch.LeaveOwnerWork();
	Expect(latch.IsQuiesced(41),
		"matching acknowledgement and zero in-flight work must quiesce");

	OwnerThreadShutdownLatch racing;
	racing.EnterOwnerWork();
	Expect(racing.Request(77),
		"shutdown must be requestable while an older owner command is active");
	racing.Acknowledge(77);
	Expect(!racing.IsQuiesced(77),
		"an active owner command must keep the request non-quiescent");
	racing.LeaveOwnerWork();
	Expect(racing.IsQuiesced(77),
		"quiescence must become visible after the owner leaves its work scope");

	if (s_failures)
	{
		std::cerr << s_failures << " owner shutdown latch test(s) failed\n";
		return 1;
	}
	std::cout << "owner shutdown latch tests passed\n";
	return 0;
}
