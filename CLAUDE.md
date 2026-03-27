# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make              # 编译（输出 bin/simpleserver）
make run          # 编译后以默认端口 8081 运行
make clean        # 清除 obj/ 和 bin/
make debug        # 带 -g -O0 的调试构建
make memcheck     # Valgrind 内存检查（需先 make debug）
```

自定义启动：

```bash
./bin/simpleserver -p 9000 -t 8 -q 256 -l /tmp/logs
```

## 架构概览

四个模块，职责分明，依赖方向单向：

```
main.c
  ├─ threadpool.c/h   线程池（循环队列 + mutex/cond）
  ├─ http_handler.c/h HTTP 解析与文件服务
  └─ log.c/h          滚动日志（线程安全）
```

**main.c**：创建 TCP socket → 初始化线程池 → accept 循环 → 将 `client_task_t`（含 fd 和客户端地址）提交到线程池。收到 SIGINT/SIGTERM 后优雅关闭。

**threadpool.c**：生产者-消费者模型。固定大小循环队列；`not_empty` / `not_full` 两个条件变量控制阻塞；`shutdown` 标志让所有工作线程在队列排空后退出。

**http_handler.c**：仅支持 GET。解析请求行，构造 `html/<path>` 文件路径，检查 `..` 路径遍历，读文件后通过 MIME 表选类型输出 HTTP 响应。404 返回内联 HTML。

**log.c**：全局互斥锁保护文件句柄；单文件写满 100 MB 后自动切换到下一序号（`server_N.log`）；超过 10 个文件时将最旧的删除后整体前移序号（rotate_files）。使用 `_IONBF` 关闭用户空间缓冲。

## 关键常量位置

| 常量 | 文件 | 说明 |
|------|------|------|
| `WEB_ROOT` | `http_handler.h` | 静态文件根目录，默认 `"html"` |
| `LOG_MAX_SIZE` | `log.h` | 单日志文件上限，默认 100 MB |
| `LOG_MAX_FILES` | `log.h` | 最多保留日志文件数，默认 10 |
| `DEFAULT_PORT` | `main.c` | 默认监听端口 8081 |
| `MAX_THREADS` | `main.c` | 线程数上限 64 |

## 平台说明

- 目标平台：Linux / macOS（或 WSL）；`_mkdir` 有条件编译分支支持 Windows。
- 编译标志：`-D_GNU_SOURCE`（启用 `strdup` 等扩展）、`-std=c11`、`-lpthread`。
