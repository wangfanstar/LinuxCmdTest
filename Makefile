# ============================================================
#  simplewebserver Makefile
# ============================================================

# 明确指定默认目标，防止 test-build 等测试目标因排在前面而被 make 误选为默认
.DEFAULT_GOAL := all

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
          -Isrc
LDFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TARGET  = $(BIN_DIR)/simplewebserver

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/log.c  \
       $(SRC_DIR)/threadpool.c \
       $(SRC_DIR)/http_handler.c \
       $(SRC_DIR)/ssh_exec.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

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
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Cleaned."
