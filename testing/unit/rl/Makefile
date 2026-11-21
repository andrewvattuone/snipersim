# Unit tests for the simulator-independent RL replacement cores in
# common/core/memory_subsystem/cache/rl/. They do not build or link Sniper.
#
#   make -C testing/unit/rl test        build and run all tests
#   ./testing/unit/rl/rl_unit_tests reward   run only test cases whose name contains "reward"
#
# Same language level and warnings as the Sniper build (CMakeLists.txt: cxx_std_20, -Wall -Wextra -Wcast-align),
# plus -Werror and -Wshadow -Wconversion so the headers stay clean when Sniper includes them.

CXX ?= g++
CORE_DIR := ../../../common/core/memory_subsystem/cache/rl
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wcast-align -Wshadow -Wconversion -Werror -I$(CORE_DIR) -I.

SRCS := test_main.cc test_common.cc test_paper_policy.cc test_reward_policy.cc test_scenarios.cc test_event_order.cc test_diagnostics.cc
HDRS := $(wildcard $(CORE_DIR)/*.h) rl_test.h rl_set_model.h
BIN := rl_unit_tests

all: $(BIN)

$(BIN): $(SRCS) $(HDRS) Makefile
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all test clean
