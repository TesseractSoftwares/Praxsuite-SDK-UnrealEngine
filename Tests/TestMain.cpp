// Entry point for the offline conformance suite.
//
// Offline means offline: no workspace, no network, no credentials. Everything asserted here is about
// the shape of what the SDK would send and how it reads what comes back, which is where the
// conformance contract's rules live and where getting it wrong fails silently.
//
// This binary is built WITHOUT Unreal, by an ordinary compiler, which is the whole reason the core
// is free of engine types. See Source/Praxsuite/Public/Core/PraxJson.h for that argument.

#include "PraxTestHarness.h"

void TestJson();
void TestErrors();
void TestKeyGuard();
void TestRoutes();
void TestEnvelopes();
void TestFilters();
void TestQueryBuilder();
void TestMutations();

int main()
{
	// Unbuffered, deliberately. stdout to a CI log is a pipe, so it is block-buffered, and a crash
	// discards whatever had not been flushed - which produced a first sanitizer run reporting only
	// "Segmentation fault" with not one line of test output to say where it got to. Losing the
	// evidence of a crash is worse than the crash.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("Praxsuite SDK for Unreal - offline conformance suite\n");
	std::printf("================================================\n");

	TestJson();
	TestErrors();
	TestKeyGuard();
	TestRoutes();
	TestEnvelopes();
	TestFilters();
	TestQueryBuilder();
	TestMutations();

	return PraxTest::Report();
}
