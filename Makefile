# ================================================
# 交易所 OrderBook Makefile
# ================================================

PROJECT_NAME := exchange_orderbook
CXX          := g++
CXXFLAGS     := -std=c++20 -Wall -Wextra -O3 -march=native -flto=auto -g
CXXFLAGS     += -DNDEBUG

FBS_DIR      := fbs
FBS_OUT      := include/generated
SRC_DIR      := src
INCLUDE_DIR  := include
TEST_DIR     := tests

MAIN_SRC     := main.cpp
SRCS         := $(MAIN_SRC) $(wildcard $(SRC_DIR)/*.cpp)
TEST_SRC     := $(TEST_DIR)/OrderBookTest.cpp

INCLUDES     := -I$(INCLUDE_DIR) -I$(FBS_OUT) -I/usr/include -I/usr/local/include

OBJS         := $(SRCS:.cpp=.o)
TEST_OBJS    := $(TEST_SRC:.cpp=.o) $(filter-out main.o, $(OBJS))

TARGET       := $(PROJECT_NAME)
TEST_TARGET  := orderbook_test

# ==================== FlatBuffers ====================
FBS_SOURCES := $(wildcard $(FBS_DIR)/*.fbs)
FBS_HEADER  := $(FBS_OUT)/order_generated.h

# FlatBuffers 生成規則
$(FBS_HEADER): $(FBS_SOURCES)
	@mkdir -p $(FBS_OUT)
	flatc --cpp --gen-mutable --gen-object-api -o $(FBS_OUT) $(FBS_SOURCES)
	@echo "FlatBuffers regenerated."

# ===========================================================

all: $(TARGET)

# 主執行檔
$(TARGET): $(FBS_HEADER) $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@
	@echo "Build completed: $@"

# 測試
test: $(TEST_TARGET)
	@echo "Running tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(FBS_HEADER) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lgtest -lgtest_main -pthread

# 一般編譯規則
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# 清理
clean:
	rm -rf $(OBJS) *.d $(TARGET) $(TEST_TARGET)
	rm -rf $(FBS_OUT)/*.h

run: all
	./$(TARGET)

fbs:
	@rm -f $(FBS_HEADER)
	@$(MAKE) $(FBS_HEADER)

.PHONY: all test clean run fbs