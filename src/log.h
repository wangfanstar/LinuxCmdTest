#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* 日志级别 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

/* 日志配置 */
#define LOG_DIR          "logs"
#define LOG_FILE_PREFIX  "server"
#define LOG_MAX_FILES    10          /* 最多保留 10 个日志文件 */
#define LOG_MAX_SIZE     (100 * 1024 * 1024)  /* 单文件最大 100MB */

/* 初始化日志系统，返回 0 成功，-1 失败 */
int  log_init(const char *log_dir);

/* 关闭日志系统 */
void log_close(void);

/* 写日志，带格式化 */
void log_write(log_level_t level, const char *fmt, ...);

/* 快捷宏 */
#define LOG_DEBUG(fmt, ...) log_write(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_write(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_write(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
