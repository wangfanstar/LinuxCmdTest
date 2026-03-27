#include "http_handler.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/*  MIME 类型映射                                                       */
/* ------------------------------------------------------------------ */

typedef struct { const char *ext; const char *mime; } mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8"       },
    { ".htm",  "text/html; charset=utf-8"       },
    { ".css",  "text/css"                        },
    { ".js",   "application/javascript"         },
    { ".json", "application/json"               },
    { ".png",  "image/png"                       },
    { ".jpg",  "image/jpeg"                      },
    { ".jpeg", "image/jpeg"                      },
    { ".gif",  "image/gif"                       },
    { ".svg",  "image/svg+xml"                  },
    { ".ico",  "image/x-icon"                   },
    { ".txt",  "text/plain; charset=utf-8"      },
    { ".log",  "text/plain; charset=utf-8"      },
    { ".pdf",  "application/pdf"                },
    { ".zip",  "application/zip"                },
    { NULL,    "application/octet-stream"       }  /* 默认 */
};

static const char *get_mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot) {
        for (int i = 0; MIME_TABLE[i].ext != NULL; i++) {
            if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
                return MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/*  发送 HTTP 响应                                                      */
/* ------------------------------------------------------------------ */

/* 发送固定文本响应（用于错误页） */
static void send_response(int fd, int status, const char *status_text,
                          const char *body)
{
    char header[512];
    int  body_len = (int)strlen(body);
    int  hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);

    write(fd, header, hlen);
    write(fd, body,   body_len);
}

/* 发送文件内容 */
static int send_file(int fd, const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode))
        return -1;

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return -1;

    const char *mime = get_mime(filepath);
    /* 日志与纯文本文件禁止缓存，其余文件允许缓存 1 小时 */
    const char *cache = (strstr(mime, "text/plain") != NULL)
                        ? "no-store"
                        : "public, max-age=3600";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long)st.st_size, cache);

    write(fd, header, hlen);

    /* 分块传输文件 */
    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
        write(fd, buf, (size_t)n);
    }

    close(file_fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  请求解析与分发                                                      */
/* ------------------------------------------------------------------ */

void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    clock_t t_start = clock();

    /* 读取请求 */
    char req_buf[4096];
    memset(req_buf, 0, sizeof(req_buf));

    ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(req_buf) - 1) {
        n = read(client_fd, req_buf + total, sizeof(req_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        /* 检测到请求头结束 */
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) {
        close(client_fd);
        return;
    }

    /* 解析第一行：METHOD PATH HTTP/x.x */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);

    LOG_INFO("request  %s:%d \"%s %s %s\"", client_ip, client_port,
             method, path, version);

    /* 仅支持 GET */
    if (strcasecmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed",
                      "<h1>405 Method Not Allowed</h1>");
        goto done;
    }

    /* 路径安全检查：禁止目录遍历 */
    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden",
                      "<h1>403 Forbidden</h1>");
        goto done;
    }

    /* 构造文件系统路径 */
    char filepath[2048];
    if (strcmp(path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);
    }

    /* 发送文件或 404 */
    if (send_file(client_fd, filepath) < 0) {
        char body[256];
        snprintf(body, sizeof(body),
                 "<h1>404 Not Found</h1><p>%s</p>", path);
        send_response(client_fd, 404, "Not Found", body);
        LOG_WARN("not_found  %s:%d \"%s\"", client_ip, client_port, path);
    }

done:;
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    LOG_INFO("response %s:%d \"%s\" done in %.2fms",
             client_ip, client_port, path, elapsed);

    close(client_fd);
}
