// A minimal test harness.
//
// No GoogleTest, no Catch2. This SDK's core promises zero dependencies, and a test suite that needs
// a package manager to run is a suite that stops being run - especially here, where the point is
// that CI can compile and execute the core WITHOUT Unreal. `g++ *.cpp && ./a.out` has to be the
// whole story.
//
// Failures print the file, line, and both values, then keep going. A harness that stops at the first
// failure turns one bug into one round trip; running the rest costs nothing and often shows that
// five failures share one cause.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace PraxTest
{
	struct FState
	{
		int Checks = 0;
		int Failures = 0;
		std::string CurrentSection;
	};

	inline FState& State()
	{
		static FState Instance;
		return Instance;
	}

	inline void Section(const char* Name)
	{
		State().CurrentSection = Name;
		std::printf("\n-- %s\n", Name);
	}

	inline void Pass(const char* What)
	{
		++State().Checks;
		std::printf("   ok   %s\n", What);
	}

	inline void Fail(const char* What, const std::string& Expected, const std::string& Actual,
					 const char* File, int Line)
	{
		++State().Checks;
		++State().Failures;
		std::printf("   FAIL %s\n", What);
		std::printf("        expected: %s\n", Expected.c_str());
		std::printf("        actual:   %s\n", Actual.c_str());
		std::printf("        at %s:%d\n", File, Line);
	}

	inline std::string Quote(const std::string& In) { return "\"" + In + "\""; }

	// A template for the numeric types rather than one overload per width. int64_t is `long` on
	// Linux and `long long` on Windows, so a hand-written overload set is ambiguous on one platform
	// or the other - which is exactly what the first CI run caught. std::to_string covers every
	// arithmetic type, and the non-template overloads below win the tiebreak on an exact match.
	template <typename T>
	inline std::string Show(const T& In) { return std::to_string(In); }

	inline std::string Show(bool In) { return In ? "true" : "false"; }
	inline std::string Show(const std::string& In) { return Quote(In); }
	inline std::string Show(const char* In) { return Quote(In ? In : "(null)"); }

	inline int Report()
	{
		const FState& S = State();
		std::printf("\n================================================\n");
		if (S.Failures == 0)
		{
			std::printf("%d checks, all passed\n", S.Checks);
			return 0;
		}
		std::printf("%d checks, %d FAILED\n", S.Checks, S.Failures);
		return 1;
	}
}

#define PRAX_CHECK(Condition, What)                                                       \
	do {                                                                                  \
		if (Condition) { PraxTest::Pass(What); }                                          \
		else { PraxTest::Fail(What, "true", "false", __FILE__, __LINE__); }                \
	} while (false)

#define PRAX_EQ(Actual, Expected, What)                                                   \
	do {                                                                                  \
		const auto ActualValue_ = (Actual);                                               \
		const auto ExpectedValue_ = (Expected);                                           \
		if (ActualValue_ == ExpectedValue_) { PraxTest::Pass(What); }                      \
		else {                                                                            \
			PraxTest::Fail(What, PraxTest::Show(ExpectedValue_),                           \
						   PraxTest::Show(ActualValue_), __FILE__, __LINE__);              \
		}                                                                                 \
	} while (false)

#define PRAX_STR_EQ(Actual, Expected, What)                                               \
	do {                                                                                  \
		const std::string ActualValue_ = (Actual);                                        \
		const std::string ExpectedValue_ = (Expected);                                    \
		if (ActualValue_ == ExpectedValue_) { PraxTest::Pass(What); }                      \
		else {                                                                            \
			PraxTest::Fail(What, PraxTest::Quote(ExpectedValue_),                          \
						   PraxTest::Quote(ActualValue_), __FILE__, __LINE__);             \
		}                                                                                 \
	} while (false)
