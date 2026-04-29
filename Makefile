# ============================================================
#  simplewebserver Makefile
# ============================================================

# 明确指定默认目标，防止 test-build 等测试目标因排在前面而被 make 误选为默认
.DEFAULT_GOAL := all

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
          -Isrc
LDFLAGS = -lpthread
SQLITE3 ?= 0

# SQLite3（SQLITE3=1）：需要 sqlite3.h 与 libsqlite3。
# - Linux: sudo apt install libsqlite3-dev 等
# - 自定义安装前缀（头文件在 $(PREFIX)/include/sqlite3.h ，库在 $(PREFIX)/lib/）：
#     make SQLITE3=1 SQLITE3_PREFIX=$(HOME)/local/sqlite3
# - 头文件与库路径不一致时再单独指定：
#     make SQLITE3=1 SQLITE3_INCLUDE=$(HOME)/local/sqlite3/include SQLITE3_LIBDIR=$(HOME)/local/sqlite3/lib
# - MSYS2: pacman -S mingw-w64-x86_64-sqlite3 ，并在「MSYS2 MinGW 64-bit」终端编译（会自动带上 MINGW_PREFIX）
# - 纯 WinLibs / 无头文件时：
#     mingw32-make SQLITE3=1 SQLITE3_PREFIX=C:/msys64/mingw64
#     mingw32-make SQLITE3=1 SQLITE3_CFLAGS=-IC:/msys64/mingw64/include SQLITE3_LDFLAGS=-LC:/msys64/mingw64/lib
SQLITE3_CFLAGS  ?=
SQLITE3_LDFLAGS ?=

# 启用 SQLITE3=1 但未指定包含路径时：若存在 ~/local/sqlite3/include/sqlite3.h 则自动作为前缀
#（编译服务器上常把自编译 SQLite 装在此路径；亦可通过 SQLITE3_PREFIX / SQLITE3_INCLUDE 手动指定）
ifeq ($(SQLITE3),1)
ifeq ($(strip $(SQLITE3_PREFIX))$(strip $(SQLITE3_INCLUDE)),)
SQLITE3_AUTO_PREFIX := $(shell home='$(HOME)'; \
  [ -z "$$home" ] && home="$$HOME"; \
  [ -n "$$home" ] && [ -f "$$home/local/sqlite3/include/sqlite3.h" ] && printf '%s' "$$home/local/sqlite3")
ifneq ($(strip $(SQLITE3_AUTO_PREFIX)),)
SQLITE3_PREFIX := $(SQLITE3_AUTO_PREFIX)
endif
endif
endif

ifneq ($(strip $(SQLITE3_PREFIX)),)
SQLITE3_CFLAGS  += -I$(SQLITE3_PREFIX)/include
SQLITE3_LDFLAGS += -L$(SQLITE3_PREFIX)/lib
endif
ifneq ($(strip $(SQLITE3_INCLUDE)),)
SQLITE3_CFLAGS += -I$(SQLITE3_INCLUDE)
endif
ifneq ($(strip $(SQLITE3_LIBDIR)),)
SQLITE3_LDFLAGS += -L$(SQLITE3_LIBDIR)
endif
ifneq ($(strip $(MINGW_PREFIX)),)
SQLITE3_CFLAGS  += -I$(MINGW_PREFIX)/include
SQLITE3_LDFLAGS += -L$(MINGW_PREFIX)/lib
endif

ifeq ($(SQLITE3),1)
CFLAGS  += -DENABLE_SQLITE3 $(SQLITE3_CFLAGS)
LDFLAGS += -lsqlite3 $(SQLITE3_LDFLAGS)
endif

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
       $(SRC_DIR)/ssh_exec.c \
       $(SRC_DIR)/auth_db.c \
       $(SRC_DIR)/webdata.c

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
