CXX ?= c++
CXXFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
CXX_STANDARD = -std=c++20
COMPILE = $(CXX) $(CPPFLAGS) $(CXX_STANDARD) $(CXXFLAGS) -Wall -Wextra -pthread -fPIC -MMD -MP

OUT ?= out
OBJ = $(OUT)/obj
BIN = $(OUT)/bin
LIB = $(OUT)/lib
LIB_SRC = $(filter-out src/main.cpp,$(wildcard src/*.cpp))
LIB_OBJ = $(patsubst src/%.cpp,$(OBJ)/src/%.o,$(LIB_SRC))
TEST_SRC = $(filter-out tests/stress_test.cpp,$(wildcard tests/*.cpp))
TEST_OBJ = $(patsubst tests/%.cpp,$(OBJ)/tests/%.o,$(TEST_SRC))

ifeq ($(shell uname -s),Darwin)
SHARED_LIB = $(LIB)/libstorage_engine.dylib
SHARED_FLAGS = -dynamiclib -Wl,-install_name,@rpath/libstorage_engine.dylib
RPATH = -Wl,-rpath,@loader_path/../lib
else
SHARED_LIB = $(LIB)/libstorage_engine.so
SHARED_FLAGS = -shared
RPATH = -Wl,-rpath,'$$ORIGIN/../lib'
endif
LINK_ENGINE = -L$(LIB) -lstorage_engine $(RPATH) -pthread

# An existing GoogleTest source tree can also be used for offline builds.
GTEST_DIR ?= $(if $(wildcard $(OUT)/deps/googletest/src/gtest-all.cc),$(OUT)/deps/googletest)
ifeq ($(strip $(GTEST_DIR)),)
GTEST_CFLAGS ?= $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS ?= $(shell pkg-config --libs gtest 2>/dev/null || echo -lgtest)
else
GTEST_CFLAGS = -isystem $(GTEST_DIR)/include
GTEST_OBJ = $(OBJ)/gtest-all.o
$(GTEST_OBJ): $(GTEST_DIR)/src/gtest-all.cc
	@mkdir -p $(@D)
	$(COMPILE) $(GTEST_CFLAGS) -I$(GTEST_DIR) -c $< -o $@
endif

.DEFAULT_GOAL := all
.PHONY: all test tests stress clean
all: $(SHARED_LIB) $(BIN)/main $(BIN)/stress_test

$(OBJ)/src/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(COMPILE) -c $< -o $@

$(OBJ)/tests/%.o: tests/%.cpp
	@mkdir -p $(@D)
	$(COMPILE) $(GTEST_CFLAGS) -c $< -o $@

$(SHARED_LIB): $(LIB_OBJ)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $(SHARED_FLAGS) $^ -pthread $(LDLIBS) -o $@

$(BIN)/main: $(OBJ)/src/main.o $(SHARED_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $< $(LINK_ENGINE) $(LDLIBS) -o $@

$(BIN)/stress_test: $(OBJ)/tests/stress_test.o $(SHARED_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $< $(LINK_ENGINE) $(LDLIBS) -o $@

$(BIN)/tests: $(TEST_OBJ) $(GTEST_OBJ) $(SHARED_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $(TEST_OBJ) $(GTEST_OBJ) $(GTEST_LIBS) $(LINK_ENGINE) $(LDLIBS) -o $@

test tests: $(BIN)/tests
	$(BIN)/tests $(TEST_ARGS)

stress: $(BIN)/stress_test
	$(BIN)/stress_test $(STRESS_ARGS)

clean:
	$(RM) -r $(OBJ) $(BIN) $(LIB)

-include $(LIB_OBJ:.o=.d) $(OBJ)/src/main.d $(TEST_OBJ:.o=.d) $(OBJ)/tests/stress_test.d $(GTEST_OBJ:.o=.d)
