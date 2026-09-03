# leanoff C++17 port — bit-exact with the Python/Go references.
# make / make all   build ../dist/cpp/leanoff(.exe)
# make test         build and run the golden-table tests (frozen argparse
#                   surface + report formatting) against tests/refdata
# make clean        remove build artifacts
#
# Directory commands are shell-guarded because mingw32-make without a POSIX
# shell cannot exec `mkdir`/`rm` directly; `cmd /c` is a real executable, so it
# works through CreateProcess as well as through any sh.

CXX ?= g++
CXXSTD ?= -std=c++17
WARN ?= -Wall -Wextra -O2
BUILD := build
DIST := ../dist/cpp

SRCS := pycompat.cpp pyunicode.cpp pyre.cpp module.cpp toolchain.cpp \
        runlean.cpp engine.cpp cli.cpp main.cpp
LIB_SRCS := pycompat.cpp pyunicode.cpp pyre.cpp module.cpp toolchain.cpp \
            runlean.cpp engine.cpp cli.cpp
OBJS := $(addprefix $(BUILD)/,$(SRCS:.cpp=.o))
LIB_OBJS := $(addprefix $(BUILD)/,$(LIB_SRCS:.cpp=.o))

BIN := $(DIST)/leanoff$(if $(filter $(OS),Windows_NT),.exe,)
TEST_BIN := $(BUILD)/test_refdata$(if $(filter $(OS),Windows_NT),.exe,)

ifeq ($(OS),Windows_NT)
MD := cmd /d /c md
RMRF := cmd /d /c rmdir /s /q
else
MD := mkdir -p
RMRF := rm -rf
endif

.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJS) | $(DIST)
	$(CXX) $(CXXSTD) $(WARN) -o $@ $^

$(DIST) $(BUILD):
	-$(MD) $(subst /,\,$@)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) -c -o $@ $<

test: $(BIN) $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): test_refdata.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) -o $@ $^

clean:
	-$(RMRF) $(subst /,\,$(BUILD))
	-$(RMRF) $(subst /,\,$(DIST))
