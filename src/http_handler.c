#include "http_handler.h"
#include "ssh_exec.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
    { NULL,    "application/octet-stream"       }
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
/*  动态字符串缓冲区（用于构造 JSON 响应）                              */
/* ------------------------------------------------------------------ */

typedef struct { char *data; size_t len; size_t cap; } strbuf_t;

static int sb_reserve(strbuf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t nc = (b->cap + extra + 1) * 2;
    if (nc < 32768) nc = 32768;
    char *nd = realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd; b->cap = nc;
    return 0;
}

static int sb_append(strbuf_t *b, const char *s, size_t n)
{
    if (sb_reserve(b, n)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

#define SB_LIT(b, s)  sb_append(b, s, sizeof(s) - 1)

static int sb_appendf(strbuf_t *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return sb_append(b, tmp, n > 0 ? (size_t)n : 0);
}

/* 追加 JSON 转义字符串（含双引号） */
static void sb_json_str(strbuf_t *b, const char *s)
{
    SB_LIT(b, "\"");
    for (; *s; s++) {
        switch (*s) {
            case '"':  SB_LIT(b, "\\\""); break;
            case '\\': SB_LIT(b, "\\\\"); break;
            case '\n': SB_LIT(b, "\\n");  break;
            case '\r': SB_LIT(b, "\\r");  break;
            case '\t': SB_LIT(b, "\\t");  break;
            default:
                if ((unsigned char)*s < 0x20)
                    sb_appendf(b, "\\u%04x", (unsigned char)*s);
                else
                    sb_append(b, s, 1);
        }
    }
    SB_LIT(b, "\"");
}

/* ------------------------------------------------------------------ */
/*  JSON 解析辅助（最小实现，仅处理固定字段）                          */
/* ------------------------------------------------------------------ */

static int json_get_str(const char *json, const char *key,
                         char *out, size_t len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' ||
           *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < len - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int def)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' ||
           *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

static int json_get_str_array(const char *json, const char *key,
                               char **out, int max_count, size_t item_len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p && *p != '[') p++;
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p && (*p == ' ' || *p == '\t' ||
               *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i < item_len - 1) {
                if (*p == '\\') {
                    p++;
                    if (!*p) break;
                    switch (*p) {
                        case 'n': out[count][i++] = '\n'; break;
                        case 'r': out[count][i++] = '\r'; break;
                        case 't': out[count][i++] = '\t'; break;
                        default:  out[count][i++] = *p;   break;
                    }
                } else { out[count][i++] = *p; }
                p++;
            }
            out[count][i] = '\0';
            if (*p == '"') p++;
            count++;
        } else if (*p == ']') break;
        else p++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  HTTP 响应发送                                                       */
/* ------------------------------------------------------------------ */

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
    write(fd, body, body_len);
}

static void send_json(int fd, int status, const char *status_text,
                      const char *json, size_t json_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, json_len);
    write(fd, header, hlen);
    write(fd, json, json_len);
}

static int send_file(int fd, const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode)) return -1;

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return -1;

    const char *mime  = get_mime(filepath);
    const char *cache = (strstr(mime, "text/plain") != NULL)
                        ? "no-store" : "public, max-age=3600";
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

    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0)
        write(fd, buf, (size_t)n);

    close(file_fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec-one  (单条命令，实时响应)                       */
/* ------------------------------------------------------------------ */

#define MAX_BODY_SIZE  (64 * 1024)
#define MAX_CMD_COUNT  SSH_MAX_CMDS
#define CMD_BUF_SIZE   2048

static void handle_api_ssh_exec_one(int client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    char command[CMD_BUF_SIZE] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_get_str(body, "pass", pass, sizeof(pass));
    port = json_get_int(body, "port", 22);
    if (!user[0]) strcpy(user, "root");

    if (json_get_str(body, "command", command, sizeof(command)) < 0 || !command[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing command\"}", 26); return;
    }

    LOG_INFO("api_ssh_exec_one: %s@%s:%d  cmd=%s", user, host, port, command);

    char *cmds[1] = { command };
    ssh_batch_t *result = ssh_batch_exec(host, port, user, pass, cmds, 1);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    if (result && !result->error[0] && result->count > 0) {
        sb_appendf(&sb, ",\"exit_code\":%d", result->results[0].exit_code);
        SB_LIT(&sb, ",\"output\":");
        sb_json_str(&sb, result->results[0].output ? result->results[0].output : "");
    } else {
        SB_LIT(&sb, ",\"exit_code\":-1,\"output\":\"\"");
    }
    SB_LIT(&sb, "}");

    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);
    if (result) ssh_batch_free(result);
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec                                                  */
/* ------------------------------------------------------------------ */

static void handle_api_ssh_exec(int client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_get_str(body, "pass", pass, sizeof(pass));
    port = json_get_int(body, "port", 22);
    if (!user[0]) strcpy(user, "root");

    /* 提取命令数组 */
    char  cmd_bufs[MAX_CMD_COUNT][CMD_BUF_SIZE];
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];

    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec: %s@%s:%d  commands=%d", user, host, port, cmd_count);

    ssh_batch_t *result = ssh_session_exec(host, port, user, pass,
                                           cmd_ptrs, cmd_count);

    /* 构造 JSON 响应 */
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    SB_LIT(&sb, ",\"results\":[");

    if (result && !result->error[0]) {
        for (int i = 0; i < result->count; i++) {
            if (i > 0) SB_LIT(&sb, ",");
            SB_LIT(&sb, "{\"cmd\":");
            sb_json_str(&sb, result->results[i].cmd    ? result->results[i].cmd    : "");
            sb_appendf(&sb, ",\"exit_code\":%d", result->results[i].exit_code);
            SB_LIT(&sb, ",\"output\":");
            sb_json_str(&sb, result->results[i].output ? result->results[i].output : "");
            SB_LIT(&sb, "}");
        }
    }
    SB_LIT(&sb, "]}");

    send_json(client_fd, 200, "OK", sb.data, sb.len);

    free(sb.data);
    if (result) ssh_batch_free(result);
}

/* ------------------------------------------------------------------ */
/*  主处理入口                                                          */
/* ------------------------------------------------------------------ */

void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    clock_t t_start = clock();

    /* 读取请求头（最多 8KB） */
    char req_buf[8192];
    memset(req_buf, 0, sizeof(req_buf));

    ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(req_buf) - 1) {
        n = read(client_fd, req_buf + total, sizeof(req_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) { close(client_fd); return; }

    /* 解析请求行 */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);

    LOG_INFO("request  %s:%d \"%s %s %s\"",
             client_ip, client_port, method, path, version);

    /* ── POST ──────────────────────────────────────── */
    if (strcasecmp(method, "POST") == 0) {
        long content_length = 0;
        const char *cl = strcasestr(req_buf, "\r\nContent-Length:");
        if (cl) {
            cl += strlen("\r\nContent-Length:");
            while (*cl == ' ') cl++;
            content_length = atol(cl);
        }

        char *body = NULL;
        if (content_length > 0 && content_length <= MAX_BODY_SIZE) {
            body = calloc((size_t)content_length + 1, 1);
            if (body) {
                const char *hdr_end = strstr(req_buf, "\r\n\r\n");
                size_t already = 0;
                if (hdr_end) {
                    hdr_end += 4;
                    already = (size_t)(total - (hdr_end - req_buf));
                    if (already > (size_t)content_length)
                        already = (size_t)content_length;
                    memcpy(body, hdr_end, already);
                }
                size_t rcvd = already;
                while (rcvd < (size_t)content_length) {
                    n = read(client_fd, body + rcvd,
                             (size_t)content_length - rcvd);
                    if (n <= 0) break;
                    rcvd += (size_t)n;
                }
            }
        }

        if (strcmp(path, "/api/ssh-exec") == 0) {
            if (body) handle_api_ssh_exec(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/ssh-exec-one") == 0) {
            if (body) handle_api_ssh_exec_one(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/cancel") == 0) {
            ssh_cancel_current();
            send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
        } else {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1>");
        }

        free(body);
        goto done;
    }

    /* ── GET ───────────────────────────────────────── */
    if (strcasecmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed",
                      "<h1>405 Method Not Allowed</h1>");
        goto done;
    }

    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden", "<h1>403 Forbidden</h1>");
        goto done;
    }

    {
        char filepath[2048];
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);

        if (send_file(client_fd, filepath) < 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "<h1>404 Not Found</h1><p>%s</p>", path);
            send_response(client_fd, 404, "Not Found", body);
            LOG_WARN("not_found  %s:%d \"%s\"", client_ip, client_port, path);
        }
    }

done:;
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    LOG_INFO("response %s:%d \"%s\" done in %.2fms",
             client_ip, client_port, path, elapsed);

    close(client_fd);
}
