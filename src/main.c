#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <pthread.h>

#include "platform.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "log.h"
#include "threadpool.h"
#include "http_handler.h"
#include "monitor.h"
#include "auth_db.h"

/* ------------------------------------------------------------------ */
/*  默认配置                                                            */
/* ------------------------------------------------------------------ */

#define DEFAULT_PORT        8881
#define DEFAULT_QUEUE_SIZE  128
#define MIN_THREADS         2
#define MAX_THREADS         64

/* ------------------------------------------------------------------ */
/*  全局变量                                                            */
/* ------------------------------------------------------------------ */

static volatile int  g_running     = 1;
static threadpool_t *g_pool        = NULL;
static http_sock_t   g_server_fd  = HTTP_SOCK_INVALID;

/* ------------------------------------------------------------------ */
/*  信号处理（POSIX；Windows 使用 SetConsoleCtrlHandler）              */
/* ------------------------------------------------------------------ */

#ifndef _WIN32
static void sig_handler(int signo)
{
    (void)signo;
    g_running = 0;
    if (http_sock_is_valid(g_server_fd)) {
        (void)http_sock_shutdown_rw(g_server_fd);
    }
}
#endif

#ifdef _WIN32
static BOOL WINAPI win_console_handler(DWORD ctrl)
{
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_CLOSE_EVENT || ctrl == CTRL_BREAK_EVENT) {
        g_running = 0;
        if (http_sock_is_valid(g_server_fd)) {
            (void)http_sock_shutdown_rw(g_server_fd);
        }
        return TRUE;
    }
    return FALSE;
}
#endif

/* ------------------------------------------------------------------ */
/*  CPU 核心数探测                                                      */
/* ------------------------------------------------------------------ */

/* 根据 CPU 核数计算推荐线程数（cores × 1.5，限 [MIN, MAX]） */
static int recommend_threads(void)
{
    int cores = platform_cpu_cores();
    int t     = cores + cores / 2; /* ≈ 1.5× */
    if (t < MIN_THREADS) t = MIN_THREADS;
    if (t > MAX_THREADS) t = MAX_THREADS;
    return t;
}

/* ------------------------------------------------------------------ */
/*  命令行（不依赖 getopt，便于 Windows/MSVC）                            */
/* ------------------------------------------------------------------ */

/* 0 成功；1 需打印帮助；<0 参数错误 */
static int parse_cli(int argc, char **argv, int *p_port, int *p_threads, int *p_queue,
                     char *log_dir, size_t log_len)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            fprintf(stderr, "unexpected argument: %s\n", a);
            return -1;
        }
        if (a[1] == 'h' && a[2] == '\0') {
            return 1;
        }
        char  opt  = a[1];
        const char *val;
        if (a[2] != '\0') {
            val = a + 2;
        } else {
            if (i + 1 >= argc) {
                fprintf(stderr, "option -%c requires a value\n", opt);
                return -1;
            }
            val = argv[++i];
        }
        switch (opt) {
        case 'p': {
            int po = atoi(val);
            if (po <= 0 || po > 65535) {
                fprintf(stderr, "invalid port: %s\n", val);
                return -1;
            }
            *p_port = po;
        } break;
        case 't': {
            int t = atoi(val);
            if (t < MIN_THREADS) t = MIN_THREADS;
            if (t > MAX_THREADS) t = MAX_THREADS;
            *p_threads = t;
        } break;
        case 'q': {
            int q = atoi(val);
            if (q < 8) q = 8;
            *p_queue = q;
        } break;
        case 'l': {
            strncpy(log_dir, val, log_len - 1);
            log_dir[log_len - 1] = '\0';
        } break;
        default:
            fprintf(stderr, "unknown option: -%c\n", opt);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  用法说明                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  -p <port>     Listen port           (default: %d)\n"
        "  -t <threads>  Worker thread count   (default: auto, %d-%d)\n"
        "  -q <size>     Task queue size        (default: %d)\n"
        "  -l <dir>      Log directory          (default: logs)\n"
        "  -h            Show this help\n",
        prog, DEFAULT_PORT, MIN_THREADS, MAX_THREADS, DEFAULT_QUEUE_SIZE);
}

/* ------------------------------------------------------------------ */
/*  主函数                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int  port         = DEFAULT_PORT;
    int  threads      = recommend_threads();
    int  queue_size   = DEFAULT_QUEUE_SIZE;
    char log_dir[256] = LOG_DIR;
    int  pr;

    pr = parse_cli(argc, argv, &port, &threads, &queue_size, log_dir, sizeof(log_dir));
    if (pr < 0) {
        return 1;
    }
    if (pr == 1) {
        usage(argv[0]);
        return 0;
    }

    /* 初始化日志 */
    if (log_init(log_dir) != 0) {
        fprintf(stderr, "failed to init log system\n");
        return 1;
    }

    platform_net_init();
    atexit(platform_net_end);

    stats_init();
    LOG_INFO("=== simplewebserver starting ===");
    LOG_INFO("port=%d  threads=%d  queue=%d  logdir=%s",
             port, threads, queue_size, log_dir);
    LOG_INFO("cpu cores detected: %d", platform_cpu_cores());
    if (auth_db_init() != 0) {
        LOG_WARN("auth sqlite init failed, runtime auth APIs may not work");
    }

#ifdef _WIN32
    (void)SetConsoleCtrlHandler(win_console_handler, TRUE);
#endif
#ifndef _WIN32
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#if defined(SIGPIPE)
    signal(SIGPIPE, SIG_IGN);
#endif
#endif

    g_pool = threadpool_create(threads, queue_size);
    if (!g_pool) {
        LOG_ERROR("failed to create thread pool");
        log_close();
        return 1;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!http_sock_is_valid(g_server_fd)) {
        char emsg[128];
        platform_format_sock_err(emsg, sizeof(emsg));
        LOG_ERROR("socket: %s", emsg);
        log_close();
        return 1;
    }

    {
        int       reuse     = 1;
        socklen_t optlen    = (socklen_t)sizeof(reuse);
        const void *opt_ptr = (const void *)&reuse;
        if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, opt_ptr, optlen) != 0) {
            char emsg[128];
            platform_format_sock_err(emsg, sizeof(emsg));
            LOG_WARN("setsockopt SO_REUSEADDR: %s", emsg);
        }
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port        = htons((uint16_t)port);

    if (bind(g_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        char emsg[128];
        platform_format_sock_err(emsg, sizeof(emsg));
        LOG_ERROR("bind port %d: %s", port, emsg);
        http_sock_close(g_server_fd);
        g_server_fd = HTTP_SOCK_INVALID;
        log_close();
        return 1;
    }

    if (listen(g_server_fd, queue_size) != 0) {
        char emsg[128];
        platform_format_sock_err(emsg, sizeof(emsg));
        LOG_ERROR("listen: %s", emsg);
        http_sock_close(g_server_fd);
        g_server_fd = HTTP_SOCK_INVALID;
        log_close();
        return 1;
    }

    LOG_INFO("listening on 0.0.0.0:%d", port);
    printf("simplewebserver running on http://0.0.0.0:%d\n", port);
    printf("Press Ctrl+C to stop.\n");

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t          addr_len = (socklen_t)sizeof(client_addr);

        http_sock_t client_fd = accept(g_server_fd,
                                    (struct sockaddr *)&client_addr, &addr_len);
        if (!http_sock_is_valid(client_fd)) {
            if (!g_running) {
                break;
            }
            char emsg[128];
            platform_format_sock_err(emsg, sizeof(emsg));
            LOG_WARN("accept: %s", emsg);
            continue;
        }

        client_task_t task = { .client_fd = client_fd, .client_addr = client_addr };
        if (threadpool_submit(g_pool, task) < 0) {
            LOG_WARN("thread pool full, dropping connection");
            http_sock_close(client_fd);
        }
    }

    LOG_INFO("shutting down...");
    if (http_sock_is_valid(g_server_fd)) {
        http_sock_close(g_server_fd);
    }
    g_server_fd = HTTP_SOCK_INVALID;
    threadpool_destroy(g_pool);
    LOG_INFO("=== simplewebserver stopped ===");
    auth_db_close();
    log_close();

    return 0;
}
