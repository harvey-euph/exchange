# ================================================
# 交易所 OrderBook Makefile - 自動掃描 src/
# ================================================

PROJECT_NAME := exchange_orderbook
CXX          := g++
CXXFLAGS     := -std=c++20 -Wall -Wextra -O3 -march=native -flto -g
CXXFLAGS     += -DNDEBUG

# 目錄設定
FBS_DIR      := fbs
FBS_OUT      := include/generated
SRC_DIR      := src
INCLUDE_DIR  := include
TEST_DIR     := tests

# 主程式
MAIN_SRC     := main.cpp
SRCS         := $(MAIN_SRC) $(wildcard $(SRC_DIR)/*.cpp)

# 測試相關
TEST_TARGET  := orderbook_test
TEST_SRC     := $(TEST_DIR)/OrderBookTest.cpp
TEST_OBJS    := $(TEST_SRC:.cpp=.o) $(filter-out main.o, $(SRCS:.cpp=.o))

# Includes
INCLUDES     := -I$(INCLUDE_DIR) -I$(FBS_OUT) -I/usr/include -I/usr/local/include

# Google Test
GTEST_LIBS   := -lgtest -lgtest_main -pthread

OBJS         := $(SRCS:.cpp=.o)
DEPS         := $(OBJS:.o=.d)
TARGET       := $(PROJECT_NAME)

all: fbs $(TARGET)

# FlatBuffers 編譯
.PHONY: fbs
fbs:
	@mkdir -p $(FBS_OUT)
	@flatc --cpp --gen-mutable --gen-object-api -o $(FBS_OUT) $(FBS_DIR)/*.fbs
	@echo "FlatBuffers generated successfully."

# 主程式
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@
	@echo "Build completed: $@"

# ==================== 測試 ====================
.PHONY: test
test: fbs $(TEST_TARGET)
	@echo "Running OrderBook tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(GTEST_LIBS)
	@echo "Test binary built: $@"

# 清理
clean:
	rm -rf $(OBJS) $(DEPS) $(TARGET) $(FBS_OUT) \
	       $(TEST_OBJS) $(TEST_DIR)/*.d $(TEST_TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run fbs test