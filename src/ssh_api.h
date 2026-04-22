#ifndef SSH_API_H
#define SSH_API_H

#include "ssh_exec.h"
#include "platform.h"

#define MAX_CMD_COUNT  SSH_MAX_CMDS
#define CMD_BUF_SIZE   2048
void handle_api_ssh_exec_one(http_sock_t client_fd, const char *body);
void handle_api_ssh_exec(http_sock_t client_fd, const char *body);
void handle_api_ssh_exec_stream(http_sock_t client_fd, const char *body);

#endif /* SSH_API_H */
