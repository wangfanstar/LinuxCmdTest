# Project Rebuild Prompt

> 本文档描述精确，AI 可在空文件夹下按此文档完整重建项目，保持架构与功能一致性。

---

## 项目概述

一个轻量级 C 语言 HTTP 服务器，用于 Linux 命令行批量测试。核心功能：通过 Web 界面向远程 Linux 主机执行 SSH 命令（批量/持久会话/实时流式），并展示实时系统监控信息。

**特点：**
- 无需 sshpass，使用 SSH_ASKPASS 机制传递密码
- 支持持久 SSH 会话（cd/环境变量跨命令保持）
- Server-Sent Events (SSE) 实时推流
- 线程池并发处理 HTTP 请求
- 旋转日志（线程安全）
- /proc 读取的 CPU/内存/进程实时监控
- 路径遍历防护，SSH 输入白名单校验

**目标平台：** Linux（RHEL/CentOS/Ubuntu）；开发环境 Windows（WSL/交叉编译）

---

## 目录结构

```
wfwebserver/
├── src/
│   ├── main.c
│   ├── threadpool.c
│   ├── threadpool.h
│   ├── log.c
│   ├── log.h
│   ├── http_handler.c
│   ├── http_handler.h
│   ├── ssh_exec.c
│   └── ssh_exec.h
├── html/
│   ├── index.html
│   ├── linux_cmd_test.html
│   ├── reports.html     （列表浏览 report/ 下已存档 HTML，依赖 GET /api/reports）
│   ├── report/          （可选；存档报告写入此目录下按年月子目录）
│   ├── logviewer.html
│   ├── monitor.html
│   ├── extract.html
│   └── test_plan.html
├── Makefile
└── CLAUDE.md
```

编译时自动创建：`obj/`（.o 文件），`bin/`（可执行文件），`logs/`（运行日志）

---

## Makefile

```makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc
LDFLAGS = -lpthread

SRCDIR  = src
OBJDIR  = obj
BINDIR  = bin
TARGET  = $(BINDIR)/simpleserver
SRCS    = $(SRCDIR)/main.c $(SRCDIR)/log.c $(SRCDIR)/threadpool.c \
          $(SRCDIR)/http_handler.c $(SRCDIR)/ssh_exec.c
OBJS    = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all run clean debug memcheck

all: $(BINDIR) $(OBJDIR) $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: all
	$(TARGET) -p 8881

clean:
	rm -rf $(OBJDIR) $(BINDIR)

debug: CFLAGS += -g -O0
debug: all

memcheck: debug
	valgrind --leak-check=full --track-origins=yes $(TARGET) -p 8881
```

---

## src/log.h

```c
#ifndef LOG_H
#define LOG_H

#define LOG_DIR          "logs"
#define LOG_FILE_PREFIX  "server"
#define LOG_MAX_FILES    10
#define LOG_MAX_SIZE     (100 * 1024 * 1024)   /* 100 MB */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

int  log_init(const char *log_dir);
void log_close(void);
void log_write(log_level_t level, const char *fmt, ...);

#define LOG_DEBUG(fmt, ...) log_write(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_write(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_write(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
```

---

## src/log.c

```c
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static FILE            *g_fp       = NULL;
static long             g_cur_size = 0;
static int              g_file_idx = 0;
static char             g_log_dir[256] = LOG_DIR;
static pthread_mutex_t  g_mutex    = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };

static int make_log_path(int idx, char *buf, size_t len) {
    return snprintf(buf, len, "%s/%s_%d.log", g_log_dir, LOG_FILE_PREFIX, idx);
}

static void rotate_files(void) {
    char src[512], dst[512];
    /* delete file 0 */
    make_log_path(0, dst, sizeof(dst));
    unlink(dst);
    /* rename 1..g_file_idx → 0..g_file_idx-1 */
    for (int i = 1; i <= g_file_idx; i++) {
        make_log_path(i, src, sizeof(src));
        make_log_path(i - 1, dst, sizeof(dst));
        rename(src, dst);
    }
    g_file_idx--;
}

static int open_log_file(void) {
    char path[512];
    make_log_path(g_file_idx, path, sizeof(path));
    g_fp = fopen(path, "a");
    if (!g_fp) return -1;
    setvbuf(g_fp, NULL, _IONBF, 0);
    /* measure current size */
    fseek(g_fp, 0, SEEK_END);
    g_cur_size = ftell(g_fp);
    return 0;
}

int log_init(const char *log_dir) {
    if (log_dir && *log_dir) {
        strncpy(g_log_dir, log_dir, sizeof(g_log_dir) - 1);
        g_log_dir[sizeof(g_log_dir) - 1] = '\0';
    }
    /* create directory if missing */
#ifdef _WIN32
    _mkdir(g_log_dir);
#else
    mkdir(g_log_dir, 0755);
#endif
    /* find next available index */
    char path[512];
    g_file_idx = 0;
    while (g_file_idx < LOG_MAX_FILES) {
        make_log_path(g_file_idx, path, sizeof(path));
        if (access(path, F_OK) != 0) break;
        g_file_idx++;
    }
    if (g_file_idx >= LOG_MAX_FILES) {
        g_file_idx = LOG_MAX_FILES - 1;
    }
    return open_log_file();
}

void log_close(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    pthread_mutex_unlock(&g_mutex);
}

void log_write(log_level_t level, const char *fmt, ...) {
    pthread_mutex_lock(&g_mutex);
    if (!g_fp) { pthread_mutex_unlock(&g_mutex); return; }

    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int written = fprintf(g_fp, "[%s] [%s] [pid:%d] %s\n",
                          ts, level_str[level], (int)getpid(), msg);
    if (written > 0) g_cur_size += written;

    if (g_cur_size >= LOG_MAX_SIZE) {
        fclose(g_fp); g_fp = NULL;
        g_file_idx++;
        if (g_file_idx >= LOG_MAX_FILES) {
            rotate_files();
            g_file_idx = LOG_MAX_FILES - 1;
        }
        open_log_file();
    }
    pthread_mutex_unlock(&g_mutex);
}
```

---

## src/threadpool.h

```c
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <netinet/in.h>

typedef struct {
    int                client_fd;
    struct sockaddr_in client_addr;
} client_task_t;

typedef struct {
    pthread_t        *threads;
    int               thread_count;
    client_task_t    *queue;        /* circular queue */
    int               queue_size;
    int               head, tail, count;
    pthread_mutex_t   mutex;
    pthread_cond_t    not_empty;
    pthread_cond_t    not_full;
    int               shutdown;
} threadpool_t;

threadpool_t *threadpool_create(int thread_count, int queue_size);
int           threadpool_submit(threadpool_t *pool, client_task_t task);
void          threadpool_destroy(threadpool_t *pool);

#endif /* THREADPOOL_H */
```

---

## src/threadpool.c

```c
#include "threadpool.h"
#include "http_handler.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

static void *worker_thread(void *arg) {
    threadpool_t *pool = (threadpool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        client_task_t task = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        handle_client(&task);
    }
    return NULL;
}

threadpool_t *threadpool_create(int thread_count, int queue_size) {
    threadpool_t *pool = calloc(1, sizeof(threadpool_t));
    if (!pool) return NULL;
    pool->queue = calloc(queue_size, sizeof(client_task_t));
    pool->threads = calloc(thread_count, sizeof(pthread_t));
    if (!pool->queue || !pool->threads) { free(pool->queue); free(pool->threads); free(pool); return NULL; }
    pool->thread_count = thread_count;
    pool->queue_size   = queue_size;
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);
    for (int i = 0; i < thread_count; i++)
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    return pool;
}

int threadpool_submit(threadpool_t *pool, client_task_t task) {
    pthread_mutex_lock(&pool->mutex);
    while (pool->count == pool->queue_size && !pool->shutdown)
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    if (pool->shutdown) { pthread_mutex_unlock(&pool->mutex); return -1; }
    pool->queue[pool->tail] = task;
    pool->tail = (pool->tail + 1) % pool->queue_size;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void threadpool_destroy(threadpool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}
```

---

## src/ssh_exec.h

```c
#ifndef SSH_EXEC_H
#define SSH_EXEC_H

#include <stddef.h>

#define SSH_MAX_CMDS   64
#define SSH_OUTPUT_MAX (512 * 1024)   /* 512 KB per command output */

typedef struct {
    char *cmd;
    char *output;
    char *workdir;
    int   exit_code;
} ssh_cmd_result_t;

typedef struct {
    ssh_cmd_result_t *results;
    int               count;
    char              error[512];
} ssh_batch_t;

typedef void (*ssh_stream_cb_t)(int idx, const char *cmd,
                                const char *output, int exit_code,
                                void *ud);

/* 批量模式：每条命令独立 SSH 连接（无持久 cd/env）*/
ssh_batch_t *ssh_batch_exec(const char *host, int port,
                            const char *user, const char *pass,
                            char **commands, int cmd_count);

/* 会话模式：单连接 bash -s，cd/env 跨命令保持 */
ssh_batch_t *ssh_session_exec(const char *host, int port,
                              const char *user, const char *pass,
                              char **commands, int cmd_count,
                              int idle_timeout_sec);

/* 流式会话：逐命令回调，实时推送 */
void ssh_session_exec_stream(const char *host, int port,
                             const char *user, const char *pass,
                             char **commands, int cmd_count,
                             ssh_stream_cb_t cb, void *ud,
                             char *error_buf, size_t error_buf_sz,
                             int idle_timeout_sec,
                             int *out_timed_out,
                             int *out_timeout_cmd_idx,
                             char *out_partial_buf, size_t out_partial_sz,
                             int net_device_mode);

/* 取消当前正在执行的 SSH 会话（发送 SIGKILL） */
void ssh_cancel_current(void);

/* 释放 ssh_batch_exec / ssh_session_exec 返回的结构 */
void ssh_batch_free(ssh_batch_t *b);

#define SESSION_IDLE_TIMEOUT_DEFAULT 300

#endif /* SSH_EXEC_H */
```

---

## src/ssh_exec.c — 核心逻辑说明（完整实现要求）

### 全局会话 PID 跟踪

```c
static volatile pid_t  g_session_pid   = (pid_t)-1;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;
```

### 辅助函数

**`validate_safe(const char *s)`**
白名单校验：只允许字母数字及 `. - _ @ [ ] :`，返回 1 合法，0 非法。

**`create_pass_file(const char *pass, char *path, size_t len)`**
创建 `/tmp/.swps_XXXXXX` 临时文件（mode 0600），写入密码，返回文件路径。

**`create_askpass_script(const char *pass_file, char *script_path, size_t len)`**
创建 `/tmp/.swpa_XXXXXX` 脚本（mode 0700），内容：
```sh
#!/bin/sh
cat '/tmp/.swps_XXXXXX'
```
供 SSH_ASKPASS 调用。

**`build_session_script(const char *boundary, char **commands, int count)`**
返回堆分配字符串（shell 脚本），格式：
```sh
trap 'trap "" HUP EXIT INT TERM; kill -- -$$ 2>/dev/null; kill -9 -- -$$ 2>/dev/null' HUP EXIT INT TERM
set +e
<command1>
printf '<BOUNDARY>:%d\n' $?
<command2>
printf '<BOUNDARY>:%d\n' $?
...
```
`<BOUNDARY>` 是 `SWBND` + 8 位随机十六进制（`/dev/urandom` 或 `rand()`）。

### ssh_batch_exec 实现要点

1. 校验 host、user、port（port 1~65535）
2. 创建 pass file + askpass script
3. 对每条命令：
   - `pipe()` 创建 stdout/stderr 管道
   - `fork()`：子进程 `dup2()` stdout/stderr 到管道写端，stdin 重定向 `/dev/null`
   - 子进程调用 `setsid()`（脱离控制终端，SSH_ASKPASS 才能生效）
   - 子进程设置环境变量：`DISPLAY=:0`，`SSH_ASKPASS=<script_path>`，`SSH_ASKPASS_REQUIRE=force`
   - 子进程 `execvpe("ssh", args, envp)`，SSH 参数见下方
   - 父进程读管道，累积到 buffer，上限 `SSH_OUTPUT_MAX`（超限截断）
   - 父进程 `waitpid()` 获取退出码，`WEXITSTATUS(status)` 提取
4. 所有命令执行完后 `unlink()` 临时文件
5. 返回 `ssh_batch_t`

SSH 参数（每条命令）：
```
ssh
  -o StrictHostKeyChecking=no
  -o ConnectTimeout=15
  -o BatchMode=no
  -o PasswordAuthentication=yes
  -o PubkeyAuthentication=no
  -o GSSAPIAuthentication=no
  -o PreferredAuthentications=password
  -o NumberOfPasswordPrompts=1
  -o LogLevel=ERROR
  -p <port>
  <user@host>
  <command>
```

### ssh_session_exec 实现要点

1. 创建 pass file + askpass script
2. 生成 boundary 标记（唯一随机 token）
3. 调用 `build_session_script()` 生成脚本
4. 创建两个管道：`stdin_pipe`（向 SSH 写脚本），`stdout_pipe`（读输出）
5. `fork()`：子进程 dup2 管道，setsid，设置环境变量，执行：
   ```
   ssh ... <user@host> bash -s
   ```
6. 父进程写脚本到 `stdin_pipe` 写端，然后关闭写端
7. 父进程循环读 `stdout_pipe`，使用 `select()` 实现 idle timeout
8. 读完后调用 `parse_session_output()`：按 boundary 分割，提取每条命令的 output 和 exit_code
9. 更新 `g_session_pid`（mutex 保护），执行完清 `-1`
10. 返回 `ssh_batch_t`

**parse_session_output**：扫描 buffer，找到 `<BOUNDARY>:<exit_code>\n`，把 boundary 前的内容作为该命令的 output（去掉首尾多余换行），exit_code 解析为整数。

### ssh_session_exec_stream 实现要点

与 `ssh_session_exec` 相同的 fork/exec 流程，区别：
- 读取循环中每次找到 boundary → 立即提取并调用 `cb(idx, cmd, output, exit_code, ud)`
- 然后从 buffer 中移除已处理部分，继续读取
- 若所有读取完毕后 buffer 无任何 boundary → SSH 连接失败，写入 `error_buf`

### ssh_cancel_current

```c
void ssh_cancel_current(void) {
    pthread_mutex_lock(&g_session_mutex);
    if (g_session_pid != (pid_t)-1)
        kill(g_session_pid, SIGKILL);
    pthread_mutex_unlock(&g_session_mutex);
}
```

### ssh_batch_free

释放 `results` 数组中每个 `cmd`、`output`、`workdir`（均为堆分配），再释放 `results` 数组，最后释放 `ssh_batch_t` 本身。

---

## src/http_handler.h

```c
#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "threadpool.h"

#define WEB_ROOT "html"

void handle_client(const client_task_t *task);
void http_handler_init(void);   /* 初始化全局统计（启动时调用一次）*/

#endif /* HTTP_HANDLER_H */
```

---

## src/http_handler.c — 完整实现要求

### 常量

```c
#define MAX_BODY_SIZE    (64  * 1024)
#define MAX_CMD_COUNT    SSH_MAX_CMDS   /* 64 */
#define CMD_BUF_SIZE     2048
#define ONLINE_WINDOW_SEC 300
#define MAX_IP_SLOTS     512
#define MAX_CORES        64
#define TOP_PER_CORE     3
#define TOP_GLOBAL       10
#define MAX_PROCS        2048
```

### 动态字符串 strbuf_t

```c
typedef struct { char *data; size_t len; size_t cap; } strbuf_t;

static void sb_reserve(strbuf_t *b, size_t extra);
static void sb_append(strbuf_t *b, const char *s, size_t n);
static void sb_appendf(strbuf_t *b, const char *fmt, ...);
static void sb_json_str(strbuf_t *b, const char *s);   /* JSON 转义后追加 */
```

### MIME 表

```c
static const struct { const char *ext; const char *type; } MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".css",  "text/css" },
    { ".js",   "application/javascript" },
    { ".json", "application/json" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
    { NULL,    "application/octet-stream" }
};
```

### JSON 解析（最小实现）

```c
/* 从 JSON 字符串中提取字段值（无完整解析库，手工扫描） */
static int  json_get_str(const char *json, const char *key, char *out, size_t len);
static int  json_get_int(const char *json, const char *key, int def);
static int  json_get_str_array(const char *json, const char *key,
                                char **out, int max_count, size_t item_len);
```

### HTTP 响应函数

```c
static void send_response(int fd, int status, const char *status_text, const char *body);
static void send_json(int fd, int status, const char *status_text,
                      const char *json, size_t json_len);
static void send_file(int fd, const char *filepath);
```

`send_json` 必须加 CORS 头：
```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

`send_file` 流式分块读取（64 KB chunk），根据扩展名选 MIME，加 Cache-Control 头。

### 全局统计（监控子系统）

```c
static volatile long  g_total_reqs   = 0;
static volatile int   g_active_conns = 0;
static volatile int   g_peak_active  = 0;
static time_t         g_start_time   = 0;

typedef struct { char ip[48]; time_t last_seen; } ip_slot_t;
static ip_slot_t      g_ip_slots[MAX_IP_SLOTS];
static int            g_ip_count    = 0;
static int            g_peak_online = 0;
static pthread_mutex_t g_stat_mutex = PTHREAD_MUTEX_INITIALIZER;
```

**在线人数统计**：每次请求记录客户端 IP + 时间戳；查询时清理超过 ONLINE_WINDOW_SEC 的条目，返回剩余计数。

### CPU 监控（/proc/stat + /proc/self/stat）

```c
typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_snap_t;

static cpu_snap_t      g_prev_sys;
static cpu_snap_t      g_prev_core[MAX_CORES];
static cpu_snap_t      g_prev_proc;   /* /proc/self/stat */
static time_t          g_prev_ts  = 0;
static float           g_cpu_sys_pct;
static float           g_cpu_proc_pct;
static float           g_cpu_core_pct[MAX_CORES];
static int             g_num_cores = 0;
```

计算公式：
```c
delta_total = (cur.user+cur.nice+cur.sys+cur.idle+cur.iowait+cur.irq+cur.softirq+cur.steal)
            - (prev.*同字段之和)
delta_work  = delta_total - (cur.idle - prev.idle)
pct         = (delta_work * 100.0) / delta_total
```

### 进程监控（/proc/[pid]/stat + /proc/[pid]/status）

```c
typedef struct { char name[64]; char user[32]; float pct; int pid; int core; } top_proc_t;
static top_proc_t g_core_top[MAX_CORES][TOP_PER_CORE];
static top_proc_t g_global_top[TOP_GLOBAL];
```

扫描逻辑：
1. opendir("/proc")，遍历数字目录名
2. 读 `/proc/<pid>/stat` 提取进程名（括号内）、utime+stime 之和、processor（字段 39，从 0 计）
3. 读 `/proc/<pid>/status` 提取 Uid，再从 `/etc/passwd`（或 getpwuid）转换为用户名
4. 与上次快照对比 delta ticks，换算百分比（需除以 delta_total / num_cores）
5. 维护每个 core 的 top-3 和全局 top-10（插入排序）

### API 路由

**`handle_api_ssh_exec_one(int client_fd, const char *body)`**

解析 JSON：host, port, user, pass, command
调用 `ssh_batch_exec()` 执行单条命令
返回：
```json
{"error":"","exit_code":0,"output":"..."}
```

**`handle_api_ssh_exec(int client_fd, const char *body)`**

解析 JSON：host, port, user, pass, commands[], timeout
调用 `ssh_session_exec()` 执行多条命令
返回：
```json
{
  "error": "",
  "results": [
    {"cmd":"...","exit_code":0,"output":"...","workdir":""},
    ...
  ]
}
```

**`handle_api_ssh_exec_stream(int client_fd, const char *body)`**

解析同上，调用 `ssh_session_exec_stream()`
先发送 SSE 响应头：
```
HTTP/1.1 200 OK\r\n
Content-Type: text/event-stream\r\n
Cache-Control: no-cache\r\n
Access-Control-Allow-Origin: *\r\n
\r\n
```
每条命令完成后回调发送：
```
data: {"type":"result","i":<idx>,"cmd":"...","exit_code":<n>,"output":"..."}\n\n
```
全部完成后发送：
```
data: {"type":"done","total":<n>}\n\n
```
空闲超时中断（含 PTY/网络设备逐条模式）：
```
data: {"type":"timeout","completed":<已推送 result 条数>,"total":<n>,"timeout_sec":<秒>,"i":<被中断命令 0-based 下标>,"partial":"..."}\n\n
```
（`partial` 可选；`i` 由 `ssh_session_exec_stream` 的 `out_timeout_cmd_idx` 填入。）

若出错：
```
data: {"type":"error","msg":"..."}\n\n
```

**`handle_api_cancel(int client_fd)`**

调用 `ssh_cancel_current()`，返回 `{"ok":true}`

**`POST /api/save-report`**

- 请求头：`Content-Type: text/html; charset=utf-8`，`X-Report-Filename` 为 URL 编码的文件名（仅 basename，服务端再净化）
- 正文：完整 HTML 字符串；正文上限 `SAVE_REPORT_MAX_BODY`（5 MB），与普通 POST `MAX_BODY_SIZE`（64 KB）分离
- 行为：在 `WEB_ROOT/report/YYYYMM/` 下创建子目录（`YYYYMM` 为服务端本地时间），写入文件；返回 `{"ok":true,"path":"report/YYYYMM/文件名.html"}` 或 `{"ok":false,"error":"..."}`

**`GET /api/client-info`**

- 返回 JSON：`{"ip":"a.b.c.d"}`，`ip` 为当前 HTTP 连接的 TCP 对端 IPv4；`linux_cmd_test` 存档弹窗用其生成<strong>默认文件名</strong>中的客户端 IP 段（预览与成功后的访问链接使用 `location.host`，即 Web 服务器）

**`GET /api/reports`**

- 扫描 `WEB_ROOT/report/` 下名为 6 位数字 `YYYYMM` 的子目录，列出其中 `.html` 常规文件
- 返回 JSON：`{"months":[{"ym":"202603","files":[{"name":"…","mtime":unix,"size":bytes}]}]}`（月份目录降序，文件按 mtime 降序；单项数量有上限）

**`handle_api_monitor(int client_fd)`**

刷新 CPU/内存/进程快照，返回 JSON：
```json
{
  "sys_cpu_pct": 45.2,
  "sys_mem_total_kb": 16777216,
  "sys_mem_used_kb": 8388608,
  "sys_mem_pct": 50.0,
  "proc_cpu_pct": 2.5,
  "proc_mem_kb": 5120,
  "online_now": 3,
  "online_peak": 10,
  "total_requests": 1234,
  "active_conns": 2,
  "uptime_sec": 3600,
  "cores": [
    {
      "pct": 45.5,
      "top": [
        {"n":"bash","u":"root","p":25.3,"i":1234},
        ...
      ]
    }
  ],
  "top10": [
    {"n":"bash","u":"root","p":25.3,"i":1234},
    ...
  ]
}
```

**`handle_api_procs(int client_fd, const char *query)`**

接收 URL 参数 `q=<keyword>`（可选）
遍历 /proc 扫描进程，按名称/用户子串过滤（大小写不敏感）
返回：
```json
{"procs":[{"n":"bash","u":"root","p":15.2,"i":1234,"c":0},...]}
```

### handle_client 主流程

```c
void handle_client(const client_task_t *task) {
    int fd = task->client_fd;
    // 1. 读请求（最多 8 KB）
    // 2. 解析请求行：METHOD PATH HTTP/VERSION
    // 3. 更新统计（total_reqs, active_conns, ip_slots）
    // 4. OPTIONS 请求：直接返回 CORS 允许头
    // 5. POST 分发：
    //    Content-Length 提取 → 读 body → 路由
    //    /api/ssh-exec-stream → handle_api_ssh_exec_stream
    //    /api/ssh-exec-one   → handle_api_ssh_exec_one
    //    /api/ssh-exec       → handle_api_ssh_exec
    //    /api/cancel         → handle_api_cancel
    // 6. GET 分发：
    //    路径包含 ".." → 403
    //    /api/monitor        → handle_api_monitor
    //    /api/procs          → handle_api_procs（解析 query string）
    //    其他               → 拼接 WEB_ROOT + path → send_file
    // 7. 路径 "/" → 重定向或发送 index.html
    // 8. 关闭 socket，更新 active_conns--
}
```

---

## src/main.c — 完整实现要求

### 常量

```c
#define DEFAULT_PORT        8881
#define DEFAULT_QUEUE_SIZE  128
#define MIN_THREADS         2
#define MAX_THREADS         64
```

### 全局变量

```c
static volatile int   g_running   = 1;
static threadpool_t  *g_pool      = NULL;
static int            g_server_fd = -1;
```

### 信号处理

```c
static void sig_handler(int signo) {
    (void)signo;
    g_running = 0;
    if (g_server_fd >= 0) close(g_server_fd);
}
```

注册 SIGINT、SIGTERM → sig_handler；SIGPIPE → SIG_IGN。

### CPU 核心检测

```c
static int detect_cpu_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}
static int recommend_threads(void) {
    int c = detect_cpu_cores();
    int t = (int)(c * 1.5);
    if (t < MIN_THREADS) t = MIN_THREADS;
    if (t > MAX_THREADS) t = MAX_THREADS;
    return t;
}
```

### 启动流程

1. 解析命令行参数 `-p <port>` `-t <threads>` `-q <size>` `-l <logdir>` `-h`
2. `log_init(log_dir)`
3. 注册信号处理
4. `threadpool_create(threads, queue_size)`
5. `http_handler_init()` — 记录启动时间
6. 创建 TCP socket：`SO_REUSEADDR`，`bind()`，`listen(128)`
7. 打印监听信息到 stdout
8. `accept()` 循环：
   - accept 新连接
   - 构造 `client_task_t`（fd + addr）
   - `threadpool_submit(pool, task)`
9. 退出后：`threadpool_destroy(pool)`，`log_close()`，打印关闭信息

---

## HTML 前端文件

### html/index.html

导航首页，提供以下页面的链接入口：
- `linux_cmd_test.html` — SSH 批量命令测试
- `reports.html` — 已存档 HTML 报告列表（`GET /api/reports`）
- `monitor.html` — 系统实时监控
- `logviewer.html` — 日志查看
- `extract.html` — 结果提取
- `test_plan.html` — 测试计划编辑

### html/linux_cmd_test.html — 主测试界面

**功能：**
1. SSH 连接参数表单：host, port, user, password
2. 命令输入区（多行，支持批量命令）
3. 执行模式选择：批量模式（ssh-exec-one 逐条）/ 会话模式（ssh-exec）/ 流式模式（ssh-exec-stream）
4. 实时结果展示区：每条命令的输出和退出码
5. 取消按钮：调用 `/api/cancel`
6. 执行按钮下方 **暂停 / 恢复**：用 Promise 门闩阻塞 `fetch` SSE 的 `reader.read()` 与逐事件处理；仅暂停前端消费，不中断远端 SSH（复位时先 `resume` 再 `abort`，避免卡在 `await`）
7. 通过 SSE（`fetch` + `ReadableStream`）接收 `/api/ssh-exec-stream` 的实时结果
8. 支持多主机（行格式 `host:port:user:pass`），逐主机执行相同命令集
9. 结果对比面板（多主机结果并排显示）
10. 工具栏「📂 报告库」跳转 `reports.html`；与「💾 存档」配套
11. 「💾 存档」打开弹窗：请求 `GET /api/client-info` 取 TCP 对端 IP 写入<strong>默认文件名</strong>；预览与成功后的<strong>访问链接</strong>使用 `location.host`（Web 服务器，如 `IP:8881`）；非 SSH 目标

**实现细节：**
- 使用原生 JavaScript（无框架）
- 流式模式使用 `fetch()` + `ReadableStream` 或 `EventSource` 消费 SSE
- 命令和结果支持导出（复制到剪贴板 / 下载 JSON）
- 加载状态指示（按钮禁用、进度文字）

### html/monitor.html — 实时监控

**功能：**
1. 轮询 `/api/monitor`（每 2 秒）
2. 显示：
   - 系统 CPU 总体利用率（进度条）
   - 每核 CPU 利用率（小进度条）
   - 内存使用（总量、已用、百分比）
   - 本进程 CPU/内存
   - 在线用户数（当前/峰值）
   - 总请求数、活跃连接数、运行时长
3. Top10 进程表（名称、用户、CPU%、PID）
4. 每核 Top3 进程
5. 进程搜索框，调用 `/api/procs?q=<keyword>` 过滤

### html/logviewer.html — 日志查看

**功能：**
1. 列出日志文件（通过 `/api/logs` 或直接 GET `logs/server_N.log`）
2. 显示日志内容，支持关键词高亮
3. 自动滚动到最新行
4. 刷新按钮

### html/extract.html — 结果提取

**功能：**
1. 粘贴或输入多条命令执行结果（JSON 或文本）
2. 使用正则提取关键数据
3. 格式化后展示（表格/JSON/CSV）
4. 支持导出

### html/test_plan.html — 测试计划编辑

**功能：**
1. 编辑测试计划（目标主机列表 + 命令集合）
2. 以 JSON 格式保存（localStorage 或下载）
3. 加载已保存计划并在 linux_cmd_test.html 中执行

---

## 安全要点（实现时必须保留）

| 措施 | 位置 | 说明 |
|------|------|------|
| 路径遍历防护 | http_handler.c | 拒绝含 `..` 的请求路径 |
| SSH 输入白名单 | ssh_exec.c | host/user 只允许 `[a-zA-Z0-9.\-_@\[\]:]` |
| 密码临时文件 | ssh_exec.c | mode 0600，用后立即 unlink |
| askpass 脚本 | ssh_exec.c | mode 0700，用后立即 unlink |
| execvpe 执行 | ssh_exec.c | argv 传参，不经 shell 解释（防命令注入） |
| 输出限制 | ssh_exec.c | 每条命令 512 KB 上限 |
| idle timeout | ssh_exec.c | select() 超时后 SIGKILL（默认 300 s）|
| 请求体限制 | http_handler.c | POST body 最大 64 KB |
| SIGPIPE 忽略 | main.c | 防止写入断开的连接崩溃进程 |

---

## 变更记录

| 日期 | 版本 | 变更内容 |
|------|------|----------|
| 2026-03-30 | v1.0 | 初始文档生成，完整描述项目架构、接口、实现要求 |
| 2026-03-30 | v1.1 | 导出 HTML 报告增加每条命令的执行时间和备注显示 |
| 2026-03-30 | v1.2 | monitor.html 进程查询结果新增 Kill 按钮（两步确认）；后端新增 POST /api/kill 端点 |
| 2026-03-30 | v1.3 | 超时中断保留已有结果：ssh_exec 新增 out_timed_out 参数，服务端发 type:timeout 事件，前端新增 markBlockTimeout 和超时状态展示 |
| 2026-03-30 | v1.4 | 降低 CPU 占用：UID→用户名缓存（64 条）、scan_proc_top 并发 trylock+同秒不重扫、/api/monitor 响应缓存 2 秒 |
| 2026-03-30 | v1.5 | linux_cmd_test.html：执行区增加暂停/恢复（SSE 消费门闩）；帮助文案同步 |
| 2026-03-30 | v1.6 | linux_cmd_test「存档」：默认文件名用 /api/client-info 客户端 IP；预览与成功链接用 location.host（服务器） |
| 2026-03-30 | v1.7 | reports.html + GET /api/reports 浏览存档；index 与 linux_cmd_test 入口 |
| 2026-03-30 | v1.8 | PTY 流式空闲超时：`ssh_session_exec_stream` 增加 `out_timeout_cmd_idx`，SSE `timeout` 事件带 `i`；`linux_cmd_test.html` 收到 `timeout` 时用标签跳出 SSE 读循环，立即取消「执行中」 |

---

## 重建验证清单

- [ ] `make` 无警告编译通过
- [ ] `./bin/simpleserver -p 8881` 启动，日志出现 "Listening on port 8881"
- [ ] 浏览器访问 `http://localhost:8881/` 展示导航页面
- [ ] SSH 单命令执行返回正确输出和退出码
- [ ] SSH 会话模式 `cd /tmp && pwd` 返回 `/tmp`（证明 cd 持久）
- [ ] 流式模式实时推送（页面逐条显示结果）
- [ ] `/api/monitor` 返回含 cores 数组的 JSON
- [ ] `/api/cancel` 能终止长时间运行的命令
- [ ] 日志文件写入 `logs/server_0.log`
- [ ] 发送 SIGINT 后服务器优雅退出，不泄漏线程
