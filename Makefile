# 编译器及编译选项
CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -g --coverage -fPIC -pg -I$(INCLUDE_DIR) -I/usr/local/include
LDFLAGS = -L/usr/local/lib -lpthread -lgtest -lgtest_main -pg --coverage

# 项目目录结构
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
TEST_DIR = tests
LIB_DIR = lib
BIN_DIR = bin

# 源文件
SRC = $(wildcard $(SRC_DIR)/*.cpp)
SRC_NO_MAIN = $(filter-out $(SRC_DIR)/main.cpp, $(SRC))
OBJ = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRC_NO_MAIN))

# 业务主程序文件
MAIN_OBJ = $(BUILD_DIR)/main.o

# 测试文件
TEST_SRC = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJ = $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(TEST_SRC))
TEST_BIN = $(BIN_DIR)/tests
STRESS_TEST_OBJ = $(BUILD_DIR)/stress_test.o
STRESS_TEST_BIN = $(BIN_DIR)/stress_test

# 动态库与静态库
LIB_SO = $(LIB_DIR)/libstorage_engine.so
LIB_A = $(LIB_DIR)/libstorage_engine.a

# 默认目标
all: $(BIN_DIR)/main $(LIB_SO) $(TEST_BIN) $(STRESS_TEST_BIN)

# 编译源文件为对象文件（不包含main.cpp）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 编译main.cpp
$(MAIN_OBJ): $(SRC_DIR)/main.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 编译测试文件为对象文件
$(BUILD_DIR)/%.o: $(TEST_DIR)/%.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 生成业务主程序
$(BIN_DIR)/main: $(MAIN_OBJ) $(OBJ) $(LIB_SO)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(LDFLAGS)

# 生成动态库
$(LIB_SO): $(OBJ)
	mkdir -p $(LIB_DIR)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

# 生成静态库
$(LIB_A): $(OBJ)
	mkdir -p $(LIB_DIR)
	ar rcs $@ $^

# 生成单元测试可执行文件
$(TEST_BIN): $(filter-out $(STRESS_TEST_OBJ),$(TEST_OBJ)) $(OBJ) $(LIB_SO)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(LDFLAGS)

# 生成压力测试可执行文件 stress_test
$(STRESS_TEST_BIN): $(STRESS_TEST_OBJ) $(OBJ) $(LIB_SO)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $(STRESS_TEST_OBJ) $(LIB_SO) $(LDFLAGS)

# 运行单元测试程序
test: $(TEST_BIN)
	@echo "Running tests..."
	$(TEST_BIN)

# 运行压力测试程序（独立于单元测试）
stress: $(STRESS_TEST_BIN)
	@echo "Running stress test..."
	$(STRESS_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR) $(BIN_DIR)

distclean: clean
	rm -f $(TEST_BIN) $(STRESS_TEST_BIN)
	