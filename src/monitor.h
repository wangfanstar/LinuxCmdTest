#ifndef MONITOR_H
#define MONITOR_H

#include "platform.h"

void stats_init(void);
void stats_req_start(const char *ip);
void stats_req_end(void);

void handle_api_monitor(http_sock_t client_fd);
void handle_api_procs(http_sock_t client_fd, const char *query, int include_ports);
void handle_api_port(http_sock_t client_fd, int query_port);

#endif /* MONITOR_H */
