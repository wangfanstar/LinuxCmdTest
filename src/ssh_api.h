#ifndef SSH_API_H
#define SSH_API_H

#define MAX_CMD_COUNT  SSH_MAX_CMDS
#define CMD_BUF_SIZE   2048

void handle_api_ssh_exec_one(int client_fd, const char *body);
void handle_api_ssh_exec(int client_fd, const char *body);
void handle_api_ssh_exec_stream(int client_fd, const char *body);

#endif /* SSH_API_H */
