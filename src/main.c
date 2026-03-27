#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "log.h"
#include "threadpool.h"
#include "http_handler.h"

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

static volatile int   g_running  = 1;
static threadpool_t  *g_pool     = NULL;
static int            g_server_fd = -1;

/* ------------------------------------------------------------------ */
/*  信号处理                                                            */
/* ------------------------------------------------------------------ */

static void sig_handler(int signo)
{
    (void)signo;
    g_running = 0;
    /* 唤醒 accept() 阻塞 */
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
    }
}

/* ------------------------------------------------------------------ */
/*  CPU 核心数探测                                                      */
/* ------------------------------------------------------------------ */

static int detect_cpu_cores(void)
{
#ifdef _SC_NPROCESSORS_ONLN
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return (int)cores;
#endif
    return 2;
}

/* 根据 CPU 核数计算推荐线程数（cores × 1.5，限 [MIN, MAX]） */
static int recommend_threads(void)
{
    int cores = detect_cpu_cores();
    int t = cores + cores / 2;   /* ≈ 1.5× */
    if (t < MIN_THREADS) t = MIN_THREADS;
    if (t > MAX_THREADS) t = MAX_THREADS;
    return t;
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
    int  port       = DEFAULT_PORT;
    int  threads    = recommend_threads();
    int  queue_size = DEFAULT_QUEUE_SIZE;
    char log_dir[256] = LOG_DIR;

    /* 解析命令行参数 */
    int opt;
    while ((opt = getopt(argc, argv, "p:t:q:l:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 't':
            threads = atoi(optarg);
            if (threads < MIN_THREADS) threads = MIN_THREADS;
            if (threads > MAX_THREADS) threads = MAX_THREADS;
            break;
        case 'q':
            queue_size = atoi(optarg);
            if (queue_size < 8) queue_size = 8;
            break;
        case 'l':
            strncpy(log_dir, optarg, sizeof(log_dir) - 1);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 0;
        }
    }

    /* 初始化日志 */
    if (log_init(log_dir) != 0) {
        fprintf(stderr, "failed to init log system\n");
        return 1;
    }

    LOG_INFO("=== simplewebserver starting ===");
    LOG_INFO("port=%d  threads=%d  queue=%d  logdir=%s",
             port, threads, queue_size, log_dir);
    LOG_INFO("cpu cores detected: %d", detect_cpu_cores());

    /* 注册信号 */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);   /* 忽略写到已关闭连接时的 SIGPIPE */

    /* 创建线程池 */
    g_pool = threadpool_create(threads, queue_size);
    if (!g_pool) {
        LOG_ERROR("failed to create thread pool");
        log_close();
        return 1;
    }

    /* 创建监听 socket */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOG_ERROR("socket: %s", strerror(errno));
        return 1;
    }

    /* SO_REUSEADDR：快速重启时避免 "Address already in use" */
    int reuse = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons((uint16_t)port);

    if (bind(g_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("bind port %d: %s", port, strerror(errno));
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, queue_size) < 0) {
        LOG_ERROR("listen: %s", strerror(errno));
        close(g_server_fd);
        return 1;
    }

    LOG_INFO("listening on 0.0.0.0:%d", port);
    printf("simplewebserver running on http://0.0.0.0:%d\n", port);
    printf("Press Ctrl+C to stop.\n");

    /* accept 主循环 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (!g_running) break;  /* 被信号中断，正常退出 */
            LOG_WARN("accept: %s", strerror(errno));
            continue;
        }

        client_task_t task = { .client_fd = client_fd,
                               .client_addr = client_addr };
        if (threadpool_submit(g_pool, task) < 0) {
            LOG_WARN("thread pool full, dropping connection");
            close(client_fd);
        }
    }

    /* 优雅关闭 */
    LOG_INFO("shutting down...");
    close(g_server_fd);
    g_server_fd = -1;
    threadpool_destroy(g_pool);
    LOG_INFO("=== simplewebserver stopped ===");
    log_close();

    return 0;
}
