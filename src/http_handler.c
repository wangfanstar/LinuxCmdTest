#include "http_handler.h"
#include "ssh_exec.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <pwd.h>

/* ------------------------------------------------------------------ */
/*  MIME 类型映射                                                       */
/* ------------------------------------------------------------------ */

typedef struct { const char *ext; const char *mime; } mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8"       },
    { ".htm",  "text/html; charset=utf-8"       },
    { ".css",  "text/css"                        },
    { ".js",   "application/javascript"         },
    { ".json", "application/json"               },
    { ".png",  "image/png"                       },
    { ".jpg",  "image/jpeg"                      },
    { ".jpeg", "image/jpeg"                      },
    { ".gif",  "image/gif"                       },
    { ".svg",  "image/svg+xml"                  },
    { ".ico",  "image/x-icon"                   },
    { ".txt",  "text/plain; charset=utf-8"      },
    { ".log",  "text/plain; charset=utf-8"      },
    { ".pdf",  "application/pdf"                },
    { ".zip",  "application/zip"                },
    { NULL,    "application/octet-stream"       }
};

static const char *get_mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot) {
        for (int i = 0; MIME_TABLE[i].ext != NULL; i++) {
            if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
                return MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/*  动态字符串缓冲区（用于构造 JSON 响应）                              */
/* ------------------------------------------------------------------ */

typedef struct { char *data; size_t len; size_t cap; } strbuf_t;

static int sb_reserve(strbuf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t nc = (b->cap + extra + 1) * 2;
    if (nc < 32768) nc = 32768;
    char *nd = realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd; b->cap = nc;
    return 0;
}

static int sb_append(strbuf_t *b, const char *s, size_t n)
{
    if (sb_reserve(b, n)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

#define SB_LIT(b, s)  sb_append(b, s, sizeof(s) - 1)

static int sb_appendf(strbuf_t *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return sb_append(b, tmp, n > 0 ? (size_t)n : 0);
}

/* 追加 JSON 转义字符串（含双引号） */
static void sb_json_str(strbuf_t *b, const char *s)
{
    SB_LIT(b, "\"");
    for (; *s; s++) {
        switch (*s) {
            case '"':  SB_LIT(b, "\\\""); break;
            case '\\': SB_LIT(b, "\\\\"); break;
            case '\n': SB_LIT(b, "\\n");  break;
            case '\r': SB_LIT(b, "\\r");  break;
            case '\t': SB_LIT(b, "\\t");  break;
            default:
                if ((unsigned char)*s < 0x20)
                    sb_appendf(b, "\\u%04x", (unsigned char)*s);
                else
                    sb_append(b, s, 1);
        }
    }
    SB_LIT(b, "\"");
}

/* ------------------------------------------------------------------ */
/*  JSON 解析辅助（最小实现，仅处理固定字段）                          */
/* ------------------------------------------------------------------ */

static int json_get_str(const char *json, const char *key,
                         char *out, size_t len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' ||
           *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < len - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int def)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' ||
           *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

static int json_get_str_array(const char *json, const char *key,
                               char **out, int max_count, size_t item_len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p && *p != '[') p++;
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p && (*p == ' ' || *p == '\t' ||
               *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i < item_len - 1) {
                if (*p == '\\') {
                    p++;
                    if (!*p) break;
                    switch (*p) {
                        case 'n': out[count][i++] = '\n'; break;
                        case 'r': out[count][i++] = '\r'; break;
                        case 't': out[count][i++] = '\t'; break;
                        default:  out[count][i++] = *p;   break;
                    }
                } else { out[count][i++] = *p; }
                p++;
            }
            out[count][i] = '\0';
            if (*p == '"') p++;
            count++;
        } else if (*p == ']') break;
        else p++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  HTTP 响应发送                                                       */
/* ------------------------------------------------------------------ */

static void send_response(int fd, int status, const char *status_text,
                           const char *body)
{
    char header[512];
    int  body_len = (int)strlen(body);
    int  hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);
    write(fd, header, hlen);
    write(fd, body, body_len);
}

static void send_json(int fd, int status, const char *status_text,
                      const char *json, size_t json_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, json_len);
    write(fd, header, hlen);
    write(fd, json, json_len);
}

static int send_file(int fd, const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode)) return -1;

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return -1;

    const char *mime  = get_mime(filepath);
    const char *cache = (strstr(mime, "text/plain") != NULL)
                        ? "no-store" : "public, max-age=3600";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long)st.st_size, cache);
    write(fd, header, hlen);

    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0)
        write(fd, buf, (size_t)n);

    close(file_fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec-one  (单条命令，实时响应)                       */
/* ------------------------------------------------------------------ */

#define MAX_BODY_SIZE  (64 * 1024)
#define MAX_CMD_COUNT  SSH_MAX_CMDS
#define CMD_BUF_SIZE   2048

static void handle_api_ssh_exec_one(int client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    char command[CMD_BUF_SIZE] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_get_str(body, "pass", pass, sizeof(pass));
    port = json_get_int(body, "port", 22);
    if (!user[0]) strcpy(user, "root");

    if (json_get_str(body, "command", command, sizeof(command)) < 0 || !command[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing command\"}", 26); return;
    }

    LOG_INFO("api_ssh_exec_one: %s@%s:%d  cmd=%s", user, host, port, command);

    char *cmds[1] = { command };
    ssh_batch_t *result = ssh_batch_exec(host, port, user, pass, cmds, 1);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    if (result && !result->error[0] && result->count > 0) {
        sb_appendf(&sb, ",\"exit_code\":%d", result->results[0].exit_code);
        SB_LIT(&sb, ",\"output\":");
        sb_json_str(&sb, result->results[0].output ? result->results[0].output : "");
    } else {
        SB_LIT(&sb, ",\"exit_code\":-1,\"output\":\"\"");
    }
    SB_LIT(&sb, "}");

    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);
    if (result) ssh_batch_free(result);
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec                                                  */
/* ------------------------------------------------------------------ */

static void handle_api_ssh_exec(int client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_get_str(body, "pass", pass, sizeof(pass));
    port = json_get_int(body, "port", 22);
    if (!user[0]) strcpy(user, "root");

    /* 提取命令数组 */
    char  cmd_bufs[MAX_CMD_COUNT][CMD_BUF_SIZE];
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];

    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec: %s@%s:%d  commands=%d", user, host, port, cmd_count);

    ssh_batch_t *result = ssh_session_exec(host, port, user, pass,
                                           cmd_ptrs, cmd_count);

    /* 构造 JSON 响应 */
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    SB_LIT(&sb, ",\"results\":[");

    if (result && !result->error[0]) {
        for (int i = 0; i < result->count; i++) {
            if (i > 0) SB_LIT(&sb, ",");
            SB_LIT(&sb, "{\"cmd\":");
            sb_json_str(&sb, result->results[i].cmd     ? result->results[i].cmd     : "");
            sb_appendf(&sb, ",\"exit_code\":%d", result->results[i].exit_code);
            SB_LIT(&sb, ",\"output\":");
            sb_json_str(&sb, result->results[i].output  ? result->results[i].output  : "");
            SB_LIT(&sb, ",\"workdir\":");
            sb_json_str(&sb, result->results[i].workdir ? result->results[i].workdir : "");
            SB_LIT(&sb, "}");
        }
    }
    SB_LIT(&sb, "]}");

    send_json(client_fd, 200, "OK", sb.data, sb.len);

    free(sb.data);
    if (result) ssh_batch_free(result);
}

/* ================================================================
   监控统计模块
   ================================================================ */
#include <time.h>

/* ── 请求 / 连接计数 ── */
static volatile long  g_total_reqs   = 0;
static volatile int   g_active_conns = 0;
static volatile int   g_peak_active  = 0;
static time_t         g_start_time   = 0;

/* ── 在线用户（5分钟窗口内唯一 IP） ── */
#define ONLINE_WINDOW_SEC 300
#define MAX_IP_SLOTS      512

typedef struct { char ip[16]; time_t ts; } ip_slot_t;
static ip_slot_t       g_ip_slots[MAX_IP_SLOTS];
static int             g_ip_count    = 0;
static int             g_peak_online = 0;
static pthread_mutex_t g_ip_mutex    = PTHREAD_MUTEX_INITIALIZER;

/* ── CPU 快照（增量法） ── */
#define MAX_CORES 64
typedef struct { unsigned long long work; unsigned long long total; } cpu_snap_t;
static cpu_snap_t      g_prev_sys              = {0, 0};
static cpu_snap_t      g_prev_proc             = {0, 0};
static cpu_snap_t      g_prev_core[MAX_CORES];
static time_t          g_prev_ts               = 0;
static float           g_cpu_sys_pct           = 0.0f;
static float           g_cpu_proc_pct          = 0.0f;
static float           g_cpu_core_pct[MAX_CORES];
static int             g_cpu_core_count        = 0;
static pthread_mutex_t g_cpu_mutex             = PTHREAD_MUTEX_INITIALIZER;

/* ── 每核 / 全局 Top 进程 ── */
#define TOP_PER_CORE  3
#define TOP_GLOBAL   10
#define MAX_PROCS  2048

typedef struct { char name[32]; char user[20]; float pct; pid_t pid; } top_proc_t;
typedef struct { pid_t pid; unsigned long long ticks; } proc_snap_t;

static top_proc_t      g_core_top[MAX_CORES][TOP_PER_CORE];
static int             g_core_top_cnt[MAX_CORES];
static top_proc_t      g_global_top[TOP_GLOBAL];
static int             g_global_top_cnt = 0;
static proc_snap_t     g_prev_procs[MAX_PROCS];
static int             g_prev_proc_count  = 0;
static pthread_mutex_t g_proc_mutex       = PTHREAD_MUTEX_INITIALIZER;

void stats_init(void) { g_start_time = time(NULL); }

/* 记录请求开始 */
static void stats_req_start(const char *ip)
{
    __sync_fetch_and_add(&g_total_reqs, 1);
    int active = __sync_add_and_fetch(&g_active_conns, 1);
    /* 更新峰值并发 */
    int peak = g_peak_active;
    while (active > peak) {
        if (__sync_bool_compare_and_swap(&g_peak_active, peak, active)) break;
        peak = g_peak_active;
    }
    /* 记录 IP */
    time_t now = time(NULL);
    pthread_mutex_lock(&g_ip_mutex);
    int found = 0;
    int oldest_i = 0;
    for (int i = 0; i < g_ip_count; i++) {
        if (g_ip_slots[i].ts < g_ip_slots[oldest_i].ts) oldest_i = i;
        if (strncmp(g_ip_slots[i].ip, ip, 15) == 0) {
            g_ip_slots[i].ts = now; found = 1; break;
        }
    }
    if (!found) {
        if (g_ip_count < MAX_IP_SLOTS) {
            strncpy(g_ip_slots[g_ip_count].ip, ip, 15);
            g_ip_slots[g_ip_count++].ts = now;
        } else {
            strncpy(g_ip_slots[oldest_i].ip, ip, 15);
            g_ip_slots[oldest_i].ts = now;
        }
    }
    int online = 0;
    for (int i = 0; i < g_ip_count; i++)
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) online++;
    if (online > g_peak_online) g_peak_online = online;
    pthread_mutex_unlock(&g_ip_mutex);
}

static void stats_req_end(void) { __sync_fetch_and_sub(&g_active_conns, 1); }

/* 读取系统 CPU 快照 (/proc/stat) */
static int snap_sys_cpu(cpu_snap_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long u, n, sy, id, iow, irq, si, st;
    int r = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &sy, &id, &iow, &irq, &si, &st);
    fclose(f);
    if (r < 4) return -1;
    s->work  = u + n + sy + irq + si + st;
    s->total = s->work + id + iow;
    return 0;
}

/* 读取每个逻辑核心的 CPU 快照（/proc/stat 中 cpu0/cpu1/... 行） */
static int snap_core_cpus(cpu_snap_t *cores, int max_cores, int *count)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    *count = 0;
    while (*count < max_cores && fgets(line, sizeof(line), f)) {
        /* 只处理 "cpuN " 行（N 为数字），跳过汇总 "cpu " 行及其他行 */
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] < '0' || line[3] > '9') continue;
        unsigned long long u=0, n=0, sy=0, id=0, iow=0, irq=0, si=0, st=0;
        /* 跳过 "cpuN" 标签，找到第一个空格后开始解析数字 */
        char *p = line + 3;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &sy, &id, &iow, &irq, &si, &st) < 4) continue;
        cores[*count].work  = u + n + sy + irq + si + st;
        cores[*count].total = cores[*count].work + id + iow;
        (*count)++;
    }
    fclose(f);
    return 0;
}

/* 读取进程 CPU 快照 (/proc/self/stat，utime+stime，clock ticks) */
static int snap_proc_cpu(cpu_snap_t *s)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    char buf[512] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    unsigned long long ut = 0, st2 = 0;
    sscanf(p + 1,
           " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
           &ut, &st2);
    s->work  = ut + st2;
    s->total = s->work;
    return 0;
}

/* 读取 /proc/[pid]/stat：进程名、ticks（utime+stime）、last_cpu */
static int read_proc_stat_entry(pid_t pid, char *name, int nl,
                                unsigned long long *ticks, int *cpu)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[512] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *s = strchr(buf, '('), *e = strrchr(buf, ')');
    if (!s || !e || e <= s) return -1;
    int n = (int)(e - s - 1);
    if (n >= nl) n = nl - 1;
    memcpy(name, s + 1, (size_t)n); name[n] = '\0';
    unsigned long long ut = 0, st = 0; int proc = 0;
    /* 跳过 state ppid pgrp session tty tpgid flags minflt×4 → utime stime
       再跳 cutime cstime priority nice nthreads itrealvalue starttime vsize rss rsslim
       startcode endcode startstack kstkesp kstkeip signal blocked sigignore sigcatch
       wchan nswap cnswap exit_signal → processor */
    int r = sscanf(e + 1,
        " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u"
        " %llu %llu"
        " %*d %*d %*d %*d %*d %*d %*u %*u %*d"
        " %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d"
        " %d", &ut, &st, &proc);
    if (r < 3) return -1;
    *ticks = ut + st;
    *cpu   = proc;
    return 0;
}

/* 读取 /proc/[pid]/status 的 UID，并解析为用户名 */
static void read_proc_user(pid_t pid, char *user, int ul)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { strncpy(user, "?", ul); return; }
    char line[128]; uid_t uid = (uid_t)-1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "Uid:", 4) == 0) { sscanf(line + 4, " %u", &uid); break; }
    fclose(f);
    struct passwd pw0, *pw = NULL; char pbuf[256];
    if (uid != (uid_t)-1 &&
        getpwuid_r(uid, &pw0, pbuf, sizeof(pbuf), &pw) == 0 && pw)
        strncpy(user, pw->pw_name, (size_t)ul - 1);
    else if (uid != (uid_t)-1)
        snprintf(user, (size_t)ul, "%u", uid);
    else
        strncpy(user, "?", (size_t)ul);
    user[ul - 1] = '\0';
}

/* 插入排序维护 top-N（按 pct 降序） */
static void top_insert(top_proc_t top[], int *cnt, int max,
                       const char *name, const char *user, float pct, pid_t pid)
{
    int tc = *cnt, pos = tc;
    for (int i = 0; i < tc; i++) { if (pct > top[i].pct) { pos = i; break; } }
    if (pos >= max) return;
    int last = (tc < max) ? tc - 1 : max - 2;
    for (int k = last; k >= pos; k--) top[k + 1] = top[k];
    strncpy(top[pos].name, name, 31); top[pos].name[31] = '\0';
    strncpy(top[pos].user, user, 19); top[pos].user[19] = '\0';
    top[pos].pct = pct;
    top[pos].pid = pid;
    if (tc < max) (*cnt)++;
}

/* 扫描 /proc，更新每核 Top-3 和全局 Top-10 */
static void scan_proc_top(double dt, int core_count)
{
    if (dt < 0.3 || core_count <= 0) return;
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double scale = 100.0 / ((double)clk * dt);

    /* 读取上次快照副本（在 mutex 外完成文件扫描） */
    static proc_snap_t prev_copy[MAX_PROCS];
    int prev_count;
    pthread_mutex_lock(&g_proc_mutex);
    prev_count = g_prev_proc_count;
    memcpy(prev_copy, g_prev_procs, sizeof(proc_snap_t) * (size_t)prev_count);
    pthread_mutex_unlock(&g_proc_mutex);

    /* 临时工作区 */
    static proc_snap_t cur[MAX_PROCS];
    int cur_count = 0;

    top_proc_t tmp_core[MAX_CORES][TOP_PER_CORE];
    int        tmp_core_cnt[MAX_CORES];
    memset(tmp_core_cnt, 0, sizeof(int) * (size_t)core_count);

    top_proc_t tmp_global[TOP_GLOBAL];
    int        tmp_global_cnt = 0;

    /* 扫描 /proc */
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && cur_count < MAX_PROCS) {
        const char *p = de->d_name;
        if (*p < '1' || *p > '9') continue;
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        if (*q != '\0') continue;

        pid_t pid = (pid_t)atoi(de->d_name);
        char name[32]; unsigned long long ticks; int last_cpu;
        if (read_proc_stat_entry(pid, name, sizeof(name), &ticks, &last_cpu) < 0)
            continue;
        if (last_cpu < 0 || last_cpu >= core_count) last_cpu = 0;

        cur[cur_count].pid   = pid;
        cur[cur_count].ticks = ticks;
        cur_count++;

        unsigned long long prev_ticks = 0;
        for (int i = 0; i < prev_count; i++)
            if (prev_copy[i].pid == pid) { prev_ticks = prev_copy[i].ticks; break; }

        float pct = (ticks > prev_ticks)
                    ? (float)((double)(ticks - prev_ticks) * scale) : 0.0f;
        if (pct < 0.05f) continue;

        char user[20] = "?";
        read_proc_user(pid, user, sizeof(user));

        top_insert(tmp_core[last_cpu], &tmp_core_cnt[last_cpu], TOP_PER_CORE,
                   name, user, pct, pid);
        top_insert(tmp_global, &tmp_global_cnt, TOP_GLOBAL, name, user, pct, pid);
    }
    closedir(dir);

    /* 写回全局结果 */
    pthread_mutex_lock(&g_proc_mutex);
    for (int i = 0; i < core_count; i++) {
        g_core_top_cnt[i] = tmp_core_cnt[i];
        for (int j = 0; j < tmp_core_cnt[i]; j++)
            g_core_top[i][j] = tmp_core[i][j];
    }
    g_global_top_cnt = tmp_global_cnt;
    for (int j = 0; j < tmp_global_cnt; j++) g_global_top[j] = tmp_global[j];
    memcpy(g_prev_procs, cur, sizeof(proc_snap_t) * (size_t)cur_count);
    g_prev_proc_count = cur_count;
    pthread_mutex_unlock(&g_proc_mutex);
}

/* 更新 CPU 百分比（与上次快照比较） */
static void update_cpu(void)
{
    cpu_snap_t sys_now, proc_now;
    cpu_snap_t core_now[MAX_CORES];
    int core_count = 0;
    time_t now = time(NULL);
    if (snap_sys_cpu(&sys_now) < 0 || snap_proc_cpu(&proc_now) < 0) return;
    snap_core_cpus(core_now, MAX_CORES, &core_count);

    double dt = 0.0;
    pthread_mutex_lock(&g_cpu_mutex);
    if (g_prev_ts > 0) {
        dt = difftime(now, g_prev_ts);
        unsigned long long ds = sys_now.total - g_prev_sys.total;
        unsigned long long dw = sys_now.work  - g_prev_sys.work;
        if (ds > 0) g_cpu_sys_pct = (float)dw / (float)ds * 100.0f;

        if (dt > 0.05) {
            long clk = sysconf(_SC_CLK_TCK);
            if (clk <= 0) clk = 100;
            unsigned long long dp = proc_now.work - g_prev_proc.work;
            g_cpu_proc_pct = (float)dp / ((float)clk * (float)dt) * 100.0f;
        }

        /* 每核百分比 */
        for (int i = 0; i < core_count && i < g_cpu_core_count; i++) {
            unsigned long long cd = core_now[i].total - g_prev_core[i].total;
            unsigned long long cw = core_now[i].work  - g_prev_core[i].work;
            g_cpu_core_pct[i] = (cd > 0) ? (float)cw / (float)cd * 100.0f : 0.0f;
        }
    }
    g_prev_sys  = sys_now;
    g_prev_proc = proc_now;
    g_prev_ts   = now;
    /* 保存每核快照 */
    for (int i = 0; i < core_count; i++) g_prev_core[i] = core_now[i];
    if (core_count > 0) {
        if (g_cpu_core_count == 0)
            for (int i = 0; i < core_count; i++) g_cpu_core_pct[i] = 0.0f;
        g_cpu_core_count = core_count;
    }
    pthread_mutex_unlock(&g_cpu_mutex);

    /* 扫描进程占用（在 g_cpu_mutex 外执行以减少锁持有时间） */
    scan_proc_top(dt, core_count);
}

/* 读取系统内存 (/proc/meminfo) */
static void read_sys_mem(long *total_kb, long *used_kb)
{
    *total_kb = *used_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64];
    long val, total = 0, avail = 0;
    while (fscanf(f, "%63s %ld %*s\n", key, &val) == 2) {
        if (strcmp(key, "MemTotal:")     == 0) total = val;
        if (strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    fclose(f);
    *total_kb = total;
    *used_kb  = total - avail;
}

/* 读取进程 RSS (/proc/self/status) */
static long read_proc_rss(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char key[64]; long val = 0;
    while (fscanf(f, "%63s %ld %*s\n", key, &val) == 2)
        if (strcmp(key, "VmRSS:") == 0) { fclose(f); return val; }
    fclose(f);
    return val;
}

/* 在线人数（窗口内唯一 IP） */
static int count_online(void)
{
    time_t now = time(NULL);
    int cnt = 0;
    pthread_mutex_lock(&g_ip_mutex);
    for (int i = 0; i < g_ip_count; i++)
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) cnt++;
    pthread_mutex_unlock(&g_ip_mutex);
    return cnt;
}

/* GET /api/monitor */
static void handle_api_monitor(int client_fd)
{
    update_cpu();

    long mem_total_kb = 0, mem_used_kb = 0;
    read_sys_mem(&mem_total_kb, &mem_used_kb);
    long proc_rss_kb = read_proc_rss();
    int  online_now  = count_online();
    long uptime_sec  = (long)difftime(time(NULL), g_start_time);
    float mem_pct    = mem_total_kb > 0
                       ? (float)mem_used_kb / (float)mem_total_kb * 100.0f : 0.0f;

    /* 快照 CPU 全局数据 */
    float  cpu_sys, cpu_proc;
    float  core_pct[MAX_CORES];
    int    core_count;
    pthread_mutex_lock(&g_cpu_mutex);
    cpu_sys    = g_cpu_sys_pct;
    cpu_proc   = g_cpu_proc_pct;
    core_count = g_cpu_core_count;
    for (int i = 0; i < core_count; i++) core_pct[i] = g_cpu_core_pct[i];
    pthread_mutex_unlock(&g_cpu_mutex);

    /* 快照每核 Top + 全局 Top */
    top_proc_t snap_core[MAX_CORES][TOP_PER_CORE];
    int        snap_core_cnt[MAX_CORES];
    top_proc_t snap_global[TOP_GLOBAL];
    int        snap_global_cnt;
    pthread_mutex_lock(&g_proc_mutex);
    for (int i = 0; i < core_count; i++) {
        snap_core_cnt[i] = g_core_top_cnt[i];
        for (int j = 0; j < g_core_top_cnt[i]; j++)
            snap_core[i][j] = g_core_top[i][j];
    }
    snap_global_cnt = g_global_top_cnt;
    for (int j = 0; j < g_global_top_cnt; j++) snap_global[j] = g_global_top[j];
    pthread_mutex_unlock(&g_proc_mutex);

    /* 构造 JSON */
    strbuf_t sb = {0};
    sb_appendf(&sb,
        "{"
        "\"sys_cpu_pct\":%.1f,"
        "\"sys_mem_total_kb\":%ld,"
        "\"sys_mem_used_kb\":%ld,"
        "\"sys_mem_pct\":%.1f,"
        "\"proc_cpu_pct\":%.1f,"
        "\"proc_mem_kb\":%ld,"
        "\"online_now\":%d,"
        "\"online_peak\":%d,"
        "\"total_requests\":%ld,"
        "\"active_conns\":%d,"
        "\"uptime_sec\":%ld,",
        cpu_sys,
        mem_total_kb, mem_used_kb, mem_pct,
        cpu_proc, proc_rss_kb,
        online_now, g_peak_online,
        g_total_reqs, g_active_conns,
        uptime_sec);

    /* "cores": 每核 pct + top3 */
    SB_LIT(&sb, "\"cores\":[");
    for (int i = 0; i < core_count; i++) {
        char hdr[32];
        int hn = snprintf(hdr, sizeof(hdr), i ? ",{\"pct\":%.1f,\"top\":["
                                              :   "{\"pct\":%.1f,\"top\":[",
                          core_pct[i]);
        sb_append(&sb, hdr, (size_t)hn);
        for (int j = 0; j < snap_core_cnt[i]; j++) {
            char entry[144];
            int en = snprintf(entry, sizeof(entry),
                j ? ",{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}"
                  :  "{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}",
                snap_core[i][j].name, snap_core[i][j].user,
                snap_core[i][j].pct,  (int)snap_core[i][j].pid);
            sb_append(&sb, entry, (size_t)en);
        }
        SB_LIT(&sb, "]}");
    }
    SB_LIT(&sb, "],");

    /* "top10": 全局 Top-10 */
    SB_LIT(&sb, "\"top10\":[");
    for (int j = 0; j < snap_global_cnt; j++) {
        char entry[144];
        int en = snprintf(entry, sizeof(entry),
            j ? ",{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}"
              :  "{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}",
            snap_global[j].name, snap_global[j].user,
            snap_global[j].pct,  (int)snap_global[j].pid);
        sb_append(&sb, entry, (size_t)en);
    }
    SB_LIT(&sb, "]}");

    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);
}

/* ------------------------------------------------------------------ */
/*  主处理入口                                                          */
/* ------------------------------------------------------------------ */

void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    stats_req_start(client_ip);
    clock_t t_start = clock();

    /* 读取请求头（最多 8KB） */
    char req_buf[8192];
    memset(req_buf, 0, sizeof(req_buf));

    ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(req_buf) - 1) {
        n = read(client_fd, req_buf + total, sizeof(req_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) { stats_req_end(); close(client_fd); return; }

    /* 解析请求行 */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);

    LOG_INFO("request  %s:%d \"%s %s %s\"",
             client_ip, client_port, method, path, version);

    /* ── POST ──────────────────────────────────────── */
    if (strcasecmp(method, "POST") == 0) {
        long content_length = 0;
        const char *cl = strcasestr(req_buf, "\r\nContent-Length:");
        if (cl) {
            cl += strlen("\r\nContent-Length:");
            while (*cl == ' ') cl++;
            content_length = atol(cl);
        }

        char *body = NULL;
        if (content_length > 0 && content_length <= MAX_BODY_SIZE) {
            body = calloc((size_t)content_length + 1, 1);
            if (body) {
                const char *hdr_end = strstr(req_buf, "\r\n\r\n");
                size_t already = 0;
                if (hdr_end) {
                    hdr_end += 4;
                    already = (size_t)(total - (hdr_end - req_buf));
                    if (already > (size_t)content_length)
                        already = (size_t)content_length;
                    memcpy(body, hdr_end, already);
                }
                size_t rcvd = already;
                while (rcvd < (size_t)content_length) {
                    n = read(client_fd, body + rcvd,
                             (size_t)content_length - rcvd);
                    if (n <= 0) break;
                    rcvd += (size_t)n;
                }
            }
        }

        if (strcmp(path, "/api/ssh-exec") == 0) {
            if (body) handle_api_ssh_exec(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/ssh-exec-one") == 0) {
            if (body) handle_api_ssh_exec_one(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/cancel") == 0) {
            ssh_cancel_current();
            send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
        } else {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1>");
        }

        free(body);
        goto done;
    }

    /* ── GET ───────────────────────────────────────── */
    if (strcasecmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed",
                      "<h1>405 Method Not Allowed</h1>");
        goto done;
    }

    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden", "<h1>403 Forbidden</h1>");
        goto done;
    }

    if (strcmp(path, "/api/monitor") == 0) {
        handle_api_monitor(client_fd);
        goto done;
    }

    {
        char filepath[2048];
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);

        if (send_file(client_fd, filepath) < 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "<h1>404 Not Found</h1><p>%s</p>", path);
            send_response(client_fd, 404, "Not Found", body);
            LOG_WARN("not_found  %s:%d \"%s\"", client_ip, client_port, path);
        }
    }

done:;
    stats_req_end();
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    LOG_INFO("response %s:%d \"%s\" done in %.2fms",
             client_ip, client_port, path, elapsed);

    close(client_fd);
}
