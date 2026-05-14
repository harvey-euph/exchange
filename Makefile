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

# 自動收集所有 .cpp 檔案
MAIN_SRC     := main.cpp
SRCS         := $(MAIN_SRC) $(wildcard $(SRC_DIR)/*.cpp)

# Includes
INCLUDES     := -I$(INCLUDE_DIR) -I$(FBS_OUT) -I/usr/include -I/usr/local/include

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

# 編譯規則
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@
	@echo "Build completed: $@"

clean:
	rm -rf $(OBJS) $(DEPS) $(TARGET) $(FBS_OUT)

run: all
	./$(TARGET)

.PHONY: all clean run fbs