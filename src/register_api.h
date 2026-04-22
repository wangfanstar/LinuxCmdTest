#ifndef REGISTER_API_H
#define REGISTER_API_H

#include <stddef.h>

#include "platform.h"

void handle_api_list_register_files(http_sock_t client_fd);
void handle_api_list_register_dirs(http_sock_t client_fd);
void handle_api_save_register_file(http_sock_t client_fd, const char *req_headers,
                                    const char *body, size_t body_len);
void handle_api_rename_register_dir(http_sock_t client_fd, const char *body);
void handle_api_delete_register_dir(http_sock_t client_fd, const char *body);
void handle_api_rename_register_file(http_sock_t client_fd, const char *body);
void handle_api_delete_register_file(http_sock_t client_fd, const char *body);
void handle_api_client_info(http_sock_t client_fd, const char *client_ip);

#endif /* REGISTER_API_H */
