# ============================================================
#  simplewebserver Makefile
# ============================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
          -Isrc
LDFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TARGET  = $(BIN_DIR)/simpleserver

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/log.c  \
       $(SRC_DIR)/threadpool.c \
       $(SRC_DIR)/http_handler.c \
       $(SRC_DIR)/ssh_exec.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# ──────────────────────────────────────────────
#  测试
# ──────────────────────────────────────────────
TEST_DIR  = tests
TCFLAGS   = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests
TLDFLAGS  = -lpthread
DBGFLAGS  = -g -O0 -DDEBUG

# Release test binaries
TEST_LOG        = $(TEST_DIR)/test_log
TEST_TP         = $(TEST_DIR)/test_threadpool
TEST_HELPERS    = $(TEST_DIR)/test_helpers

# Debug test binaries (for valgrind)
TEST_LOG_DBG     = $(TEST_DIR)/test_log_dbg
TEST_TP_DBG      = $(TEST_DIR)/test_threadpool_dbg
TEST_HELPERS_DBG = $(TEST_DIR)/test_helpers_dbg

.PHONY: test test-build test-unit test-integration \
        test-build-debug clean-tests

## Build all test binaries (release)
test-build: $(TEST_LOG) $(TEST_TP) $(TEST_HELPERS)

## Build all test binaries (debug, for valgrind)
test-build-debug: $(TEST_LOG_DBG) $(TEST_TP_DBG) $(TEST_HELPERS_DBG)

$(TEST_LOG): $(TEST_DIR)/test_log.c $(SRC_DIR)/log.c
	$(CC) $(TCFLAGS) -o $@ $^ $(TLDFLAGS)

$(TEST_TP): $(TEST_DIR)/test_threadpool.c $(SRC_DIR)/threadpool.c $(SRC_DIR)/log.c
	$(CC) $(TCFLAGS) -o $@ $^ $(TLDFLAGS)

# test_helpers.c #includes http_handler.c directly — do NOT also link it
$(TEST_HELPERS): $(TEST_DIR)/test_helpers.c
	$(CC) $(TCFLAGS) -Wno-unused-function -o $@ $< $(TLDFLAGS)

$(TEST_LOG_DBG): $(TEST_DIR)/test_log.c $(SRC_DIR)/log.c
	$(CC) $(TCFLAGS) $(DBGFLAGS) -o $@ $^ $(TLDFLAGS)

$(TEST_TP_DBG): $(TEST_DIR)/test_threadpool.c $(SRC_DIR)/threadpool.c $(SRC_DIR)/log.c
	$(CC) $(TCFLAGS) $(DBGFLAGS) -o $@ $^ $(TLDFLAGS)

$(TEST_HELPERS_DBG): $(TEST_DIR)/test_helpers.c
	$(CC) $(TCFLAGS) $(DBGFLAGS) -Wno-unused-function -o $@ $< $(TLDFLAGS)

## Run unit tests only
test-unit: test-build
	@echo "=== Unit Tests ==="; \
	rc=0; \
	for t in $(TEST_LOG) $(TEST_TP) $(TEST_HELPERS); do \
	    echo; $$t || rc=1; \
	done; \
	exit $$rc

## Run integration tests only (starts server on TEST_PORT, default 18881)
test-integration: all
	@bash $(TEST_DIR)/test_http_api.sh

## Run all tests (unit + integration)
test: test-build all
	@bash $(TEST_DIR)/run_tests.sh all

## Clean test binaries
clean-tests:
	rm -f $(TEST_DIR)/test_log $(TEST_DIR)/test_threadpool $(TEST_DIR)/test_helpers \
	      $(TEST_DIR)/test_log_dbg $(TEST_DIR)/test_threadpool_dbg $(TEST_DIR)/test_helpers_dbg

# ──────────────────────────────────────────────
#  默认目标
# ──────────────────────────────────────────────
.PHONY: all clean run debug memcheck init

all: init $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful: $@"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ──────────────────────────────────────────────
#  初始化目录结构
# ──────────────────────────────────────────────
init:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) logs html

# ──────────────────────────────────────────────
#  运行（默认端口 8881）
# ──────────────────────────────────────────────
run: all
	./$(TARGET) -p 8881

# ──────────────────────────────────────────────
#  调试版本（带 gdb 符号）
# ──────────────────────────────────────────────
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

# ──────────────────────────────────────────────
#  内存检查（需要 valgrind）
# ──────────────────────────────────────────────
memcheck: debug
	valgrind --leak-check=full --show-leak-kinds=all \
	         --track-origins=yes ./$(TARGET) -p 8881

# ──────────────────────────────────────────────
#  清理
# ──────────────────────────────────────────────
clean: clean-tests
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Cleaned."
