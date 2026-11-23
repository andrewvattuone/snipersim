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

SRCS := test_main.cc test_common.cc test_paper_policy.cc test_reward_policy.cc test_scenarios.cc test_event_order.cc test_diagnostics.cc \
        test_controls.cc test_estimator.cc test_trace.cc
HDRS := $(wildcard $(CORE_DIR)/*.h) rl_test.h rl_set_model.h
BIN := rl_unit_tests

all: $(BIN)

$(BIN): $(SRCS) $(HDRS) Makefile
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

# Differential check against another tree's cores, e.g. the commit before a change to them:
#   make -C testing/unit/rl diff-cores OLD=/path/to/other/snipersim
# Builds diff_cores.cc against each tree's headers and compares the two outputs byte for byte.
diff-cores: diff_cores.cc
	@test -n "$(OLD)" || { echo "usage: make diff-cores OLD=<snipersim tree>"; exit 2; }
	$(CXX) -std=c++20 -O2 -I$(OLD)/common/core/memory_subsystem/cache/rl -I$(OLD)/testing/unit/rl -o diff_cores_old diff_cores.cc
	$(CXX) -std=c++20 -O2 -I$(CORE_DIR) -I. -o diff_cores_new diff_cores.cc
	./diff_cores_old > diff_cores_old.txt
	./diff_cores_new > diff_cores_new.txt
	cmp diff_cores_old.txt diff_cores_new.txt && echo "diff-cores: identical, $$(wc -l < diff_cores_new.txt) configurations"

clean:
	rm -f $(BIN) diff_cores_old diff_cores_new diff_cores_old.txt diff_cores_new.txt

.PHONY: all test clean diff-cores
