#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  内部状态                                                            */
/* ------------------------------------------------------------------ */

static FILE          *g_fp       = NULL;   /* 当前日志文件句柄 */
static long           g_cur_size = 0;      /* 当前文件已写字节数 */
static int            g_file_idx = 0;      /* 当前文件序号 (0-based) */
static char           g_log_dir[256] = "logs";
static pthread_mutex_t g_mutex  = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };

/* ------------------------------------------------------------------ */
/*  内部函数                                                            */
/* ------------------------------------------------------------------ */

/* 构造日志文件路径，序号 idx */
static void build_path(char *buf, size_t len, int idx)
{
    snprintf(buf, len, "%s/%s_%d.log", g_log_dir, LOG_FILE_PREFIX, idx);
}

/* 删除最旧的日志文件（序号 0）并将其余文件向前滚动 */
static void rotate_files(void)
{
    char path[512];
    /* 删除最旧的一个 */
    build_path(path, sizeof(path), 0);
    remove(path);

    /* 向前重命名: 1->0, 2->1, ... (LOG_MAX_FILES-1) -> (LOG_MAX_FILES-2) */
    for (int i = 1; i < LOG_MAX_FILES; i++) {
        char old_path[512], new_path[512];
        build_path(old_path, sizeof(old_path), i);
        build_path(new_path, sizeof(new_path), i - 1);
        rename(old_path, new_path);
    }
    g_file_idx = LOG_MAX_FILES - 1;
}

/* 打开新的日志文件 */
static int open_log_file(void)
{
    char path[512];

    /* 如果文件数量已达上限，先滚动 */
    if (g_file_idx >= LOG_MAX_FILES) {
        rotate_files();
    }

    build_path(path, sizeof(path), g_file_idx);
    g_fp = fopen(path, "a");
    if (!g_fp) {
        fprintf(stderr, "log_init: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    /* 关闭用户空间缓冲，确保日志立即落盘 */
    setvbuf(g_fp, NULL, _IONBF, 0);

    /* 获取已有大小 */
    fseek(g_fp, 0, SEEK_END);
    g_cur_size = ftell(g_fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  公开接口                                                            */
/* ------------------------------------------------------------------ */

int log_init(const char *log_dir)
{
    if (log_dir && *log_dir) {
        strncpy(g_log_dir, log_dir, sizeof(g_log_dir) - 1);
    }

    /* 确保目录存在 */
#ifdef _WIN32
    _mkdir(g_log_dir);
#else
    mkdir(g_log_dir, 0755);
#endif

    /* 找到下一个可用序号（已有文件数） */
    char path[512];
    g_file_idx = 0;
    for (int i = 0; i < LOG_MAX_FILES; i++) {
        build_path(path, sizeof(path), i);
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); g_file_idx = i; }
        else { g_file_idx = i; break; }
    }

    return open_log_file();
}

void log_close(void)
{
    pthread_mutex_lock(&g_mutex);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    pthread_mutex_unlock(&g_mutex);
}

void log_write(log_level_t level, const char *fmt, ...)
{
    pthread_mutex_lock(&g_mutex);

    if (!g_fp) goto unlock;

    /* 当前文件超限，切换到下一个 */
    if (g_cur_size >= LOG_MAX_SIZE) {
        fclose(g_fp);
        g_fp = NULL;
        g_file_idx++;
        if (open_log_file() != 0) goto unlock;
    }

    /* 时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* 格式化用户消息 */
    va_list ap;
    va_start(ap, fmt);
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 写入 */
    int written = fprintf(g_fp, "[%s] [%s] [pid:%d] %s\n",
                          time_buf,
                          level_str[level],
                          (int)getpid(),
                          msg);
    if (written > 0) g_cur_size += written;

unlock:
    pthread_mutex_unlock(&g_mutex);
}
