#ifndef MONITOR_H
#define MONITOR_H

void stats_init(void);
void stats_req_start(const char *ip);
void stats_req_end(void);

void handle_api_monitor(int client_fd);
void handle_api_procs(int client_fd, const char *query, int include_ports);
void handle_api_port(int client_fd, int query_port);

#endif /* MONITOR_H */
