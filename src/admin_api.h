#ifndef ADMIN_API_H
#define ADMIN_API_H

#include "http_utils.h"

/* GET /api/admin-log-files —— 列出日志目录内 server_N.log */
void handle_api_admin_log_files(http_sock_t client_fd);

/* GET /api/admin-log-read?file=&offset=&limit= —— 按字节读日志块 */
void handle_api_admin_log_read(http_sock_t client_fd, const char *path_qs);

/* GET /api/admin-ip-stats?file=&from=&to=&path=&ip=&top= —— IP 访问统计 */
void handle_api_admin_ip_stats(http_sock_t client_fd, const char *path_qs);

#endif /* ADMIN_API_H */
