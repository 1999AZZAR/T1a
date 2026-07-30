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

SRCS = $(filter-out $(SRC_DIR)/test_runner.c, $(wildcard $(SRC_DIR)/*.c))
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

TARGET = t1a

TEST_FLAGS   = -g -O0
TEST_OBJ_DIR = obj_test
# Test binary links against production .o files (minus main.o) + test_runner.c
PROD_OBJS_NO_MAIN = $(filter-out $(OBJ_DIR)/main.o, $(OBJS))
DEPS = $(OBJS:.o=.d)

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

test: release $(TARGET)_test
	./$(TARGET)_test

$(TARGET)_test: $(PROD_OBJS_NO_MAIN) $(SRC_DIR)/test_runner.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -g -O0 $(INCLUDES) -o $@ $(PROD_OBJS_NO_MAIN) $(SRC_DIR)/test_runner.c $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TARGET)_test

-include $(DEPS)
