#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include <netinet/in.h>

/* Web 根目录（相对于服务器工作目录） */
#define WEB_ROOT  "html"

/* 处理一个客户端连接：解析 HTTP 请求并发送响应 */
void handle_client(int client_fd, struct sockaddr_in *addr);

/* 初始化监控统计（在 main 启动时调用一次） */
void stats_init(void);

#endif /* HTTP_HANDLER_H */
