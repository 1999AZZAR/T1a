CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter -MMD -MP
INCLUDES = -Isrc -I/usr/local/include
LDFLAGS = -L/usr/local/lib -lbearssl -lpthread

# Optimization: small binary, fast code
RELEASE_FLAGS = -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
RELEASE_LDFLAGS = -flto -Wl,--gc-sections

DEBUG_FLAGS = -g -O0 -DDEBUG

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

TARGET = noclaw

TEST_FLAGS = -g -O0 -DNC_TEST -DNC_TEST_MAIN
TEST_OBJ_DIR = obj_test
TEST_OBJS = $(SRCS:$(SRC_DIR)/%.c=$(TEST_OBJ_DIR)/%.o)
DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all clean debug release test

all: release

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(OPT_FLAGS) $(INCLUDES) -c $< -o $@

debug: OPT_FLAGS = $(DEBUG_FLAGS)
debug: $(TARGET)

release: OPT_FLAGS = $(RELEASE_FLAGS)
release: LDFLAGS += $(RELEASE_LDFLAGS)
release: $(TARGET)
	@echo "Binary: $$(du -h $(TARGET) | cut -f1)"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OPT_FLAGS) -o $@ $^ $(LDFLAGS)

test: $(TARGET)_test
	./$(TARGET)_test

$(TEST_OBJ_DIR):
	mkdir -p $(TEST_OBJ_DIR)

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TEST_OBJ_DIR)
	$(CC) $(CFLAGS) $(TEST_FLAGS) $(INCLUDES) -c $< -o $@

$(TARGET)_test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(TEST_FLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(TEST_OBJ_DIR) $(TARGET) $(TARGET)_test

-include $(DEPS)
