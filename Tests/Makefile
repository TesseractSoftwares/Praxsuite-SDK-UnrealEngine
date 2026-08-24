# Builds and runs the offline conformance suite WITHOUT Unreal.
#
# A plain Makefile rather than CMake, and no test framework, because the whole point is that this
# needs nothing installed beyond a C++17 compiler. `make -C Tests` has to be the entire story - if
# verifying the core ever required a package manager or an engine install, it would stop happening,
# which is how the rest of the portfolio ended up with claims nobody had tested.
#
# Warnings are errors. The core handles network input and untrusted strings, and the classes of
# mistake -Wall catches here (sign comparisons walking off a buffer, an unhandled switch case in a
# type dispatch) are exactly the ones that turn into silent corruption rather than a crash.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g \
            -Wall -Wextra -Werror \
            -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast \
            -Wnon-virtual-dtor -Woverloaded-virtual \
            -fno-omit-frame-pointer

CORE_DIR := ../Source/Praxsuite/Private/Core
INC      := -I../Source/Praxsuite/Public -I.

CORE_SRC := $(wildcard $(CORE_DIR)/*.cpp)
TEST_SRC := $(wildcard *.cpp)

BIN := prax-tests

.PHONY: all test clean sanitize sanitize-address sanitize-undefined build-address build-undefined

all: test

test: $(BIN)
	./$(BIN)

$(BIN): $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@

# The sanitizers run as TWO SEPARATE passes, not one combined -fsanitize=address,undefined build.
#
# That is not a style preference, it is measured. The combined build segfaulted under Debian's clang
# before reaching main, while address-only, undefined-only, and no-sanitizer builds of the identical
# sources all ran the full suite clean. So the fault was in combining the two runtimes, not in the
# code - and a combined build was never necessary to get the coverage. Two passes exercise both
# sanitizers and avoid the fragile combination entirely.
#
# Why bother at all: the codec does pointer arithmetic over attacker-shaped strings - truncated
# escapes, unpaired surrogates, deeply nested input - so an out-of-bounds read is a realistic bug
# here, and it is precisely the class -Wall cannot see.
#
# -fno-sanitize-recover=all so UBSan aborts on the first violation rather than printing and carrying
# on. A warning in a log that still exits 0 is a finding nobody acts on.
sanitize: sanitize-address sanitize-undefined

# Build-only targets, separate from the run, so CI can run the binary itself with retries and
# inspect its output. That distinction matters here: clang's ASan runtime in the CI container
# INTERMITTENTLY fails to map its shadow memory and dies before main, and telling that apart from a
# genuine finding requires looking at whether the suite produced any output at all - which `make`
# running the binary for you cannot do.
$(BIN)-asan: $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=address -fno-sanitize-recover=all $(INC) $^ -o $@

$(BIN)-ubsan: $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=undefined -fno-sanitize-recover=all $(INC) $^ -o $@

build-address: $(BIN)-asan
build-undefined: $(BIN)-ubsan

sanitize-address: $(BIN)-asan
	@echo "=== ASan pass ==="
	./$(BIN)-asan

sanitize-undefined: $(BIN)-ubsan
	@echo "=== UBSan pass ==="
	./$(BIN)-ubsan

clean:
	rm -f $(BIN) $(BIN)-asan $(BIN)-ubsan $(BIN)-san
