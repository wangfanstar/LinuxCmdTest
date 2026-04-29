#ifndef WEBDATA_H
#define WEBDATA_H

#include "platform.h"

/* 在 logs 目录下创建/打开 WebData.db；应在 log_init 成功后调用 */
int  webdata_init(const char *log_dir);
void webdata_close(void);

/* 与文本日志对应的一行；由 log_write 在落盘后调用 */
void webdata_append_log(const char *ts, const char *level, int pid, const char *message);

/* 登录事件；与 auth login_logs 同步一行 */
void webdata_login_event(const char *ts, const char *ip, const char *username, int ok,
                         const char *note);

/* GET /api/webdata-login-stats?ip_sort=last|count&user_sort=last|count */
void handle_api_webdata_login_stats(http_sock_t client_fd, const char *path_with_query);

/* GET /api/webdata-app-logs?limit=N */
void handle_api_webdata_app_logs(http_sock_t client_fd, const char *path_with_query);

#endif /* WEBDATA_H */
