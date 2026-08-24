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

int main()
{
	std::printf("Praxsuite SDK for Unreal - offline conformance suite\n");
	std::printf("================================================\n");

	TestJson();

	return PraxTest::Report();
}
