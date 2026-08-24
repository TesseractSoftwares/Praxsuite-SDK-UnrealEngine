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

.PHONY: all test clean sanitize

all: test

test: $(BIN)
	./$(BIN)

$(BIN): $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@

# A second pass under the address and undefined-behaviour sanitizers. The codec does pointer
# arithmetic over attacker-shaped strings (surrogate escapes, truncated input), so an out-of-bounds
# read is a realistic bug here and one that -Wall cannot see. Separate target because it is slower
# and not every compiler ships the runtime.
sanitize: $(CORE_SRC) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		$(INC) $^ -o $(BIN)-san
	./$(BIN)-san

clean:
	rm -f $(BIN) $(BIN)-san
