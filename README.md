# simplewebserver

一个用 **C 语言**编写的轻量级 HTTP 静态文件服务器，具备以下特性：

- 多线程线程池（生产者-消费者模型）负载分担
- 滚动日志（最多 10 个文件，单文件 100 MB 上限）
- 自定义监听端口、线程数、队列大小
- 目录遍历防御、SIGINT/SIGTERM 优雅关闭

---

## 快速开始

```bash
# 编译
make

# 运行（默认端口 8081）
make run

# 自定义参数
./bin/simpleserver -p 9000 -t 8 -q 256 -l /var/log/simpleserver
```

浏览器访问 `http://localhost:8081`

---

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <port>` | 监听端口 | `8081` |
| `-t <threads>` | 工作线程数 | CPU 核数 × 1.5 |
| `-q <size>` | 任务队列长度 | `128` |
| `-l <dir>` | 日志目录 | `logs/` |
| `-h` | 显示帮助 | — |

---

## 构建命令

```bash
make          # 编译
make run      # 编译并在 8081 端口运行
make debug    # 带调试符号编译
make memcheck # Valgrind 内存检查
make clean    # 清理构建产物
```

---

## 目录结构

```
simplewebserver/
├── src/
│   ├── main.c           # 入口：参数解析、socket 监听、accept 循环
│   ├── threadpool.c/h   # 线程池：循环队列 + mutex/cond 同步
│   ├── http_handler.c/h # HTTP 解析、文件服务、MIME 映射
│   └── log.c/h          # 线程安全的滚动日志
├── html/                # Web 根目录（静态文件放这里）
├── logs/                # 运行时日志（自动创建）
├── bin/                 # 可执行文件（构建产物）
├── obj/                 # 目标文件（构建产物）
└── Makefile
```

---

## 系统要求

- Linux / macOS（或 WSL）
- GCC 7+，支持 C11
- pthread 库

---

## 许可证

MIT
