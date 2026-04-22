#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "platform.h"
#ifndef _WIN32
#include <netinet/in.h>
#endif

/* Web 根目录（相对于服务器工作目录） */
#define WEB_ROOT  "html"

/* 处理一个客户端连接：解析 HTTP 请求并发送响应 */
void handle_client(http_sock_t client_fd, struct sockaddr_in *addr);

#endif /* HTTP_HANDLER_H */
