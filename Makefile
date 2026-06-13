CXX := g++
INCLUDES := -Iinclude

# Detect CPU core count to determine performance tier
NUM_CORES := $(shell nproc)
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -MMD -MP

ifeq ($(shell test $(NUM_CORES) -ge 8; echo $$?),0)
    # Enable busy-waiting and native microarchitecture tuning on systems with >= 8 cores
    CXXFLAGS += -march=native -DPRODUCTION_MODE
endif

# Automatically determine default affinity profile based on CPU core count,
# unless it is already overridden on the command line.
ifndef AFFINITY_PROFILE
    ifeq ($(shell test $(NUM_CORES) -ge 6; echo $$?),0)
        AFFINITY_PROFILE := AFFINITY_PROFILE_ISOLATED
    else ifeq ($(shell test $(NUM_CORES) -ge 4; echo $$?),0)
        AFFINITY_PROFILE := AFFINITY_PROFILE_COMPACT
    else
        AFFINITY_PROFILE := AFFINITY_PROFILE_SHARED
    endif
endif

# If AFFINITY_PROFILE is defined (either automatically or manually), compile it in
ifdef AFFINITY_PROFILE
    CXXFLAGS += -D$(AFFINITY_PROFILE)
endif

BUILD_DIR := build
SRC_DIR := src
SERVICE_DIR := app/services
AGENT_DIR := app/client-agents
EXAMPLE_DIR := app/client-examples
CLIENT_PERF_DIR := app/client-perf
TEST_DIR := tests
FBS_DIR := fbs
FBS_OUT := include/fbs
LDLIBS := -lgtest -lgtest_main -pthread -lrt -lpqxx -lpq
TEST_LDLIBS := -lgtest -lgtest_main -pthread -lrt

# -----------------------------------------------------------------------------
# FlatBuffers
# -----------------------------------------------------------------------------

FBS_SOURCES := $(wildcard $(FBS_DIR)/*.fbs)
FBS_TS_OUT := web/src/fbs
FBS_TS_GENERATED := $(patsubst $(FBS_DIR)/%.fbs,$(FBS_TS_OUT)/%.ts,$(FBS_SOURCES))
FBS_GENERATED := $(patsubst $(FBS_DIR)/%.fbs,$(FBS_OUT)/%_generated.h,$(FBS_SOURCES)) $(FBS_TS_GENERATED)

# -----------------------------------------------------------------------------
# Source Objects
# -----------------------------------------------------------------------------

SRC_SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
SRC_OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRC_SOURCES))
SRC_DEPS := $(SRC_OBJECTS:.o=.d)

# -----------------------------------------------------------------------------
# Service Executables
# -----------------------------------------------------------------------------

SERVICE_SOURCES := $(wildcard $(SERVICE_DIR)/*.cpp)
SERVICE_TARGETS := $(patsubst $(SERVICE_DIR)/%.cpp,$(BUILD_DIR)/services/%,$(SERVICE_SOURCES))

# -----------------------------------------------------------------------------
# Agent Executables
# -----------------------------------------------------------------------------

AGENT_SOURCES := $(wildcard $(AGENT_DIR)/*.cpp)
AGENT_TARGETS := $(patsubst $(AGENT_DIR)/%.cpp,$(BUILD_DIR)/client-agents/%,$(AGENT_SOURCES))

# -----------------------------------------------------------------------------
# Example Executables
# -----------------------------------------------------------------------------

EXAMPLE_SOURCES := $(wildcard $(EXAMPLE_DIR)/*.cpp)
EXAMPLE_TARGETS := $(patsubst $(EXAMPLE_DIR)/%.cpp,$(BUILD_DIR)/client-examples/%,$(EXAMPLE_SOURCES))

# -----------------------------------------------------------------------------
# Client Perf Executables
# -----------------------------------------------------------------------------

CLIENT_PERF_SOURCES := $(wildcard $(CLIENT_PERF_DIR)/*.cpp)
CLIENT_PERF_TARGETS := $(patsubst $(CLIENT_PERF_DIR)/%.cpp,$(BUILD_DIR)/client-perf/%,$(CLIENT_PERF_SOURCES))

# -----------------------------------------------------------------------------
# Observability Executables
# -----------------------------------------------------------------------------

OBS_DIR := app/perf
OBS_SOURCES := $(wildcard $(OBS_DIR)/*.cpp)
OBS_TARGETS := $(patsubst $(OBS_DIR)/%.cpp,$(BUILD_DIR)/app/perf/%,$(OBS_SOURCES))
EBPF_DIR := app/perf/ebpf

# -----------------------------------------------------------------------------
# Test Executables
# tests/foo.cpp -> build/tests/foo
# -----------------------------------------------------------------------------

TEST_SOURCES := $(wildcard $(TEST_DIR)/*.cpp)
TEST_TARGETS := $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/tests/%,$(TEST_SOURCES))

# -----------------------------------------------------------------------------
# Default Target
# -----------------------------------------------------------------------------

WEB_DIR := web

.PHONY: all
all: $(FBS_GENERATED) $(SERVICE_TARGETS) $(AGENT_TARGETS) $(EXAMPLE_TARGETS) $(CLIENT_PERF_TARGETS) $(OBS_TARGETS) ebpf web_target

.PHONY: fbs
fbs: $(FBS_GENERATED)

.PHONY: ebpf
ebpf: $(FBS_GENERATED)
	$(MAKE) -C $(EBPF_DIR)

.PHONY: web_target
web_target: $(FBS_GENERATED)
	$(MAKE) -C $(WEB_DIR)

# -----------------------------------------------------------------------------
# Build Services
# -----------------------------------------------------------------------------

$(BUILD_DIR)/services/%: $(SERVICE_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/services
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(SRC_OBJECTS) $(LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Build Agents
# -----------------------------------------------------------------------------

$(BUILD_DIR)/client-agents/%: $(AGENT_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/client-agents
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(filter-out $(BUILD_DIR)/PostgresClientDatabase.o $(BUILD_DIR)/ClientManager.o,$(SRC_OBJECTS)) $(TEST_LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Build Examples
# -----------------------------------------------------------------------------

$(BUILD_DIR)/client-examples/%: $(EXAMPLE_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/client-examples
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(filter-out $(BUILD_DIR)/PostgresClientDatabase.o $(BUILD_DIR)/ClientManager.o,$(SRC_OBJECTS)) $(TEST_LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Build Client Perf
# -----------------------------------------------------------------------------

$(BUILD_DIR)/client-perf/%: $(CLIENT_PERF_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/client-perf
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(filter-out $(BUILD_DIR)/PostgresClientDatabase.o $(BUILD_DIR)/ClientManager.o,$(SRC_OBJECTS)) $(TEST_LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Build Observabilities
# -----------------------------------------------------------------------------

$(BUILD_DIR)/app/perf/%: $(OBS_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/app/perf
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(SRC_OBJECTS) $(LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Build Tests
# -----------------------------------------------------------------------------

$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(filter-out $(BUILD_DIR)/PostgresClientDatabase.o $(BUILD_DIR)/ClientManager.o $(BUILD_DIR)/AlgoTradingClient.o,$(SRC_OBJECTS)) $(TEST_LDLIBS) -o $@

# -----------------------------------------------------------------------------
# Run Tests
# -----------------------------------------------------------------------------

.PHONY: test
test: $(TEST_TARGETS)
	@for test_bin in $(TEST_TARGETS); do \
		echo "Running $$test_bin"; \
		$$test_bin || exit $$?; \
	done

# -----------------------------------------------------------------------------
# Compile Source Objects
# -----------------------------------------------------------------------------

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(FBS_GENERATED)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# -----------------------------------------------------------------------------
# Generate FlatBuffers Headers
# -----------------------------------------------------------------------------

$(FBS_OUT)/%_generated.h: $(FBS_DIR)/%.fbs
	@mkdir -p $(FBS_OUT)
	flatc --cpp --gen-mutable --gen-object-api -o $(FBS_OUT) $<

$(FBS_TS_OUT)/%.ts: $(FBS_DIR)/%.fbs
	@mkdir -p $(FBS_TS_OUT)
	flatc --ts -o $(FBS_TS_OUT) $<

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(FBS_OUT)
	rm -rf $(FBS_TS_OUT)
	$(MAKE) -C $(EBPF_DIR) clean
	$(MAKE) -C $(WEB_DIR) clean

# -----------------------------------------------------------------------------
# Include Dependency Files
# -----------------------------------------------------------------------------

-include $(SRC_DEPS)