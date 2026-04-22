#ifndef REPORT_API_H
#define REPORT_API_H

#include <stddef.h>

#include "platform.h"

void handle_api_save_report(http_sock_t client_fd, const char *req_headers,
                             const char *body, size_t body_len);
void handle_api_save_config(http_sock_t client_fd, const char *req_headers,
                             const char *body, size_t body_len);
void handle_api_reports(http_sock_t client_fd);
void handle_api_delete_report(http_sock_t client_fd, const char *body);
void handle_api_list_ssh_configs(http_sock_t client_fd, const char *path);
void handle_api_list_all_configs(http_sock_t client_fd);

#endif /* REPORT_API_H */
