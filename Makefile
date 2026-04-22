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
# Windows/MinGW 会生成 .exe
ifeq ($(OS),Windows_NT)
EXEEXT  = .exe
else
EXEEXT  =
endif
TARGET  = $(BIN_DIR)/simplewebserver$(EXEEXT)

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/log.c  \
       $(SRC_DIR)/threadpool.c \
       $(SRC_DIR)/http_handler.c \
       $(SRC_DIR)/http_utils.c \
       $(SRC_DIR)/platform.c \
       $(SRC_DIR)/report_api.c \
       $(SRC_DIR)/register_api.c \
       $(SRC_DIR)/wiki.c \
       $(SRC_DIR)/svn_api.c \
       $(SRC_DIR)/ssh_api.c \
       $(SRC_DIR)/monitor.c \
       $(SRC_DIR)/ssh_exec.c

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
SRCS := $(filter-out $(SRC_DIR)/monitor.c $(SRC_DIR)/ssh_exec.c,$(SRCS)) \
         $(SRC_DIR)/monitor_win.c $(SRC_DIR)/ssh_exec_win_stub.c
endif

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
#（Windows 上 mingw32-make 常配合 cmd，无 mkdir -p / rm -rf）  
# ──────────────────────────────────────────────
ifeq ($(OS),Windows_NT)
init:
	@if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)
	@if not exist $(BIN_DIR) mkdir $(BIN_DIR)
	@if not exist logs mkdir logs
	@if not exist html mkdir html
else
init:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) logs html
endif

# ──────────────────────────────────────────────
#  运行（默认端口 8881）
# ──────────────────────────────────────────────
ifeq ($(OS),Windows_NT)
run: all
	"$(TARGET)" -p 8881
else
run: all
	./$(TARGET) -p 8881
endif

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
ifeq ($(OS),Windows_NT)
clean:
	-@rmdir /s /q $(OBJ_DIR) 2>nul
	-@if exist $(TARGET) del /f /q $(TARGET) 2>nul
	@echo "Cleaned."
else
clean:
	-rm -rf $(OBJ_DIR)
	-rm -f $(TARGET)
	@echo "Cleaned."
endif
