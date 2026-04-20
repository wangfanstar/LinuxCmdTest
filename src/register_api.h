#ifndef REGISTER_API_H
#define REGISTER_API_H

#include <stddef.h>

void handle_api_list_register_files(int client_fd);
void handle_api_list_register_dirs(int client_fd);
void handle_api_save_register_file(int client_fd, const char *req_headers,
                                    const char *body, size_t body_len);
void handle_api_rename_register_dir(int client_fd, const char *body);
void handle_api_delete_register_dir(int client_fd, const char *body);
void handle_api_rename_register_file(int client_fd, const char *body);
void handle_api_delete_register_file(int client_fd, const char *body);
void handle_api_client_info(int client_fd, const char *client_ip);

#endif /* REGISTER_API_H */
