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
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <pwd.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/uio.h>
#include <netinet/tcp.h>

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
    /* 先探测实际长度，避免栈缓冲截断 */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;

    if ((size_t)n < 4096) {
        char tmp[4096];
        va_start(ap, fmt);
        vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        return sb_append(b, tmp, (size_t)n);
    }
    /* 超出栈缓冲，堆分配精确大小 */
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) return -1;
    va_start(ap, fmt);
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = sb_append(b, tmp, (size_t)n);
    free(tmp);
    return r;
}

/* 追加 JSON 转义字符串（含双引号）；返回 0 成功，-1 内存分配失败 */
static int sb_json_str(strbuf_t *b, const char *s)
{
    if (SB_LIT(b, "\"") < 0) return -1;
    for (; *s; s++) {
        int r;
        switch (*s) {
            case '"':  r = SB_LIT(b, "\\\""); break;
            case '\\': r = SB_LIT(b, "\\\\"); break;
            case '\n': r = SB_LIT(b, "\\n");  break;
            case '\r': r = SB_LIT(b, "\\r");  break;
            case '\t': r = SB_LIT(b, "\\t");  break;
            default:
                if ((unsigned char)*s < 0x20)
                    r = sb_appendf(b, "\\u%04x", (unsigned char)*s);
                else
                    r = sb_append(b, s, 1);
        }
        if (r < 0) return -1;
    }
    return SB_LIT(b, "\"");
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

/* 读取密码字段：支持 JSON 字符串或 null（空密码）；其它非字符串视为空 */
static void json_read_pass_value_at(const char *vp, char *out, size_t len)
{
    out[0] = '\0';
    if (!vp || !*vp) return;
    if (strncmp(vp, "null", 4) == 0 &&
        (vp[4] == '\0' || vp[4] == ',' || vp[4] == '}' || vp[4] == ']' ||
         isspace((unsigned char)vp[4])))
        return;
    if (*vp != '"') return;
    vp++;
    size_t i = 0;
    while (*vp && *vp != '"' && i < len - 1) {
        if (*vp == '\\') {
            vp++;
            if (!*vp) break;
            switch (*vp) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *vp; break;
            }
        } else {
            out[i++] = *vp;
        }
        vp++;
    }
    out[i] = '\0';
}

static const char *json_parse_skip_string(const char *p)
{
    if (!p || *p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\') {
            if (!p[1]) break;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

/* 跳过 {…} 或 […]，字符串内忽略括号 */
static const char *json_skip_composite(const char *p)
{
    int b = 0, s = 0;
    if (*p == '{') {
        b++;
        p++;
    } else if (*p == '[') {
        s++;
        p++;
    } else
        return p;
    while (*p && (b > 0 || s > 0)) {
        if (*p == '"') {
            p = json_parse_skip_string(p);
            continue;
        }
        if (*p == '{') {
            b++;
            p++;
            continue;
        }
        if (*p == '}') {
            b--;
            p++;
            continue;
        }
        if (*p == '[') {
            s++;
            p++;
            continue;
        }
        if (*p == ']') {
            s--;
            p++;
            continue;
        }
        p++;
    }
    return p;
}

static const char *json_skip_value_full(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) p++;
    if (!p || !*p) return p;
    if (*p == '"') return json_parse_skip_string(p);
    if (*p == '{' || *p == '[') return json_skip_composite(p);
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           !isspace((unsigned char)*p))
        p++;
    return p;
}

/* 仅从根对象读取 pass / password，避免 strstr("\"pass\"") 命中 expected 等嵌套字段 */
static void json_api_get_pass(const char *body, char *out, size_t len)
{
    out[0] = '\0';
    const char *p = body;
    while (*p && *p != '{') p++;
    if (*p != '{') return;
    p++;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '}') return;
        if (*p != '"') return;
        p++;
        const char *k0 = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p += 2;
                continue;
            }
            p++;
        }
        if (*p != '"') return;
        size_t klen = (size_t)(p - k0);
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ':') return;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        int hit = (klen == 4 && !strncmp(k0, "pass", 4)) ||
                  (klen == 8 && !strncmp(k0, "password", 8));
        if (hit) {
            json_read_pass_value_at(p, out, len);
            return;
        }
        p = json_skip_value_full(p);
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}')
            return;
    }
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
/*  报告存档：HTML / JSON 配置 → WEB_ROOT/report/<user>/YYYYMM/         */
/* ------------------------------------------------------------------ */

static int http_header_value(const char *headers, const char *name,
                             char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = headers;

    for (;;) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end == p)
            return -1;
        if ((size_t)(line_end - p) >= nlen + 1 &&
            strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vl = (size_t)(line_end - v);
            if (vl >= cap)
                vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            while (vl > 0 && (out[vl - 1] == ' ' || out[vl - 1] == '\t'))
                out[--vl] = '\0';
            return 0;
        }
        p = line_end + 2;
    }
}

static void url_decode_report_fn(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            char hx[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hx, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void sanitize_report_basename(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);

    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20)
            *p = '_';
    }

    while (name[0] == '.')
        memmove(name, name + 1, strlen(name));

    if (name[0] == '\0') {
        strncpy(name, "report.html", 256);
        name[255] = '\0';
        return;
    }

    size_t len = strlen(name);
    if (len > 200) {
        name[200] = '\0';
        len = 200;
    }
    if (len < 5 || strcasecmp(name + len - 5, ".html") != 0) {
        if (len + 5 < 256)
            memcpy(name + len, ".html", 6);
    }
}

/* 存档文件名净化（删除接口用）：不强制改后缀，由 report_*_basename_ok 校验 */
static void sanitize_report_archive_name(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);

    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20)
            *p = '_';
    }
    while (name[0] == '.')
        memmove(name, name + 1, strlen(name));
    if (name[0] == '\0')
        return;
    if (strlen(name) > 200)
        name[200] = '\0';
}

/* SSH 登录名净化为单级目录名：report/<user>/… */
static void sanitize_report_user_dir(char *name, size_t cap)
{
    char *slash;

    if (!name || cap < 4) return;
    if (!name[0]) {
        snprintf(name, cap, "root");
        return;
    }
    slash = strrchr(name, '/');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);

    for (char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-')
            *p = '_';
    }
    while (name[0] == '.')
        memmove(name, name + 1, strlen(name) + 1);

    if (name[0] == '\0') {
        snprintf(name, cap, "root");
        return;
    }
    if (strlen(name) > 63)
        name[63] = '\0';
}

static void sanitize_config_basename(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);

    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20)
            *p = '_';
    }
    while (name[0] == '.')
        memmove(name, name + 1, strlen(name));

    if (name[0] == '\0') {
        strncpy(name, "ssh-config.json", 256);
        name[255] = '\0';
        return;
    }
    size_t len = strlen(name);
    if (len > 200)
        name[200] = '\0';
    len = strlen(name);
    if (len < 6 || strcasecmp(name + len - 5, ".json") != 0) {
        if (len + 6 < 256)
            memcpy(name + len, ".json", 6);
    }
}

/* 创建 report/<user>/<YYYYMM>/ ，返回最终目录路径于 dirpath */
static int mkdir_report_user_ym(char *dirpath, size_t dcap,
                                  const char *user_sanitized, const char *yyyymm)
{
    char tmp[512];

    snprintf(tmp, sizeof(tmp), "%s/report", WEB_ROOT);
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;

    snprintf(tmp, sizeof(tmp), "%s/report/%s", WEB_ROOT, user_sanitized);
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;

    snprintf(dirpath, dcap, "%s/report/%s/%s", WEB_ROOT, user_sanitized, yyyymm);
    if (mkdir(dirpath, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*  POST /api/save-report — HTML 写入 WEB_ROOT/report/<user>/YYYYMM/   */
static void handle_api_save_report(int client_fd, const char *req_headers,
                                   const char *body, size_t body_len)
{
    char filename[256];
    char userdir[128];

    if (http_header_value(req_headers, "X-Report-Filename", filename,
                          sizeof(filename)) != 0 || filename[0] == '\0') {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Report-Filename\"}", 45);
        return;
    }

    url_decode_report_fn(filename);
    sanitize_report_basename(filename);

    if (http_header_value(req_headers, "X-Report-User", userdir,
                          sizeof(userdir)) != 0 || userdir[0] == '\0')
        strncpy(userdir, "root", sizeof(userdir));
    else
        url_decode_report_fn(userdir);
    userdir[sizeof(userdir) - 1] = '\0';
    sanitize_report_user_dir(userdir, sizeof(userdir));

    time_t     now = time(NULL);
    struct tm  tm_local;
    if (localtime_r(&now, &tm_local) == NULL) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"time\"}", 28);
        return;
    }

    char yyyymm[16];
    if (strftime(yyyymm, sizeof(yyyymm), "%Y%m", &tm_local) == 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"strftime\"}", 32);
        return;
    }

    char dirpath[512];
    if (mkdir_report_user_ym(dirpath, sizeof(dirpath), userdir, yyyymm) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir\"}", 30);
        return;
    }

    char filepath[640];
    int  fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38);
        return;
    }

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/report/%s/%s/", WEB_ROOT, userdir,
             yyyymm);
    if (strncmp(filepath, prefix, strlen(prefix)) != 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"bad path\"}", 33);
        return;
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"open file\"}", 35);
        return;
    }

    if (body_len > 0 &&
        fwrite(body, 1, body_len, fp) != body_len) {
        fclose(fp);
        unlink(filepath);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write\"}", 30);
        return;
    }

    fclose(fp);

    char resp[768];
    int  rl = snprintf(resp, sizeof(resp),
                       "{\"ok\":true,\"path\":\"report/%s/%s/%s\"}",
                       userdir, yyyymm, filename);
    if (rl < 0 || (size_t)rl >= sizeof(resp)) {
        send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    } else {
        send_json(client_fd, 200, "OK", resp, (size_t)rl);
    }

    LOG_INFO("save_report  %s  (%zu bytes)", filepath, body_len);
}

/*  POST /api/save-config — JSON 配置写入 report/<user>/YYYYMM/         */
static void handle_api_save_config(int client_fd, const char *req_headers,
                                   const char *body, size_t body_len)
{
    char filename[256];
    char userdir[128];

    if (http_header_value(req_headers, "X-Config-Filename", filename,
                          sizeof(filename)) != 0 || filename[0] == '\0') {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Config-Filename\"}", 46);
        return;
    }

    url_decode_report_fn(filename);
    sanitize_config_basename(filename);

    if (http_header_value(req_headers, "X-Report-User", userdir,
                          sizeof(userdir)) != 0 || userdir[0] == '\0')
        strncpy(userdir, "root", sizeof(userdir));
    else
        url_decode_report_fn(userdir);
    userdir[sizeof(userdir) - 1] = '\0';
    sanitize_report_user_dir(userdir, sizeof(userdir));

    time_t     now = time(NULL);
    struct tm  tm_local;
    if (localtime_r(&now, &tm_local) == NULL) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"time\"}", 28);
        return;
    }

    char yyyymm[16];
    if (strftime(yyyymm, sizeof(yyyymm), "%Y%m", &tm_local) == 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"strftime\"}", 32);
        return;
    }

    char dirpath[512];
    if (mkdir_report_user_ym(dirpath, sizeof(dirpath), userdir, yyyymm) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir\"}", 30);
        return;
    }

    char filepath[640];
    int  fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38);
        return;
    }

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/report/%s/%s/", WEB_ROOT, userdir,
             yyyymm);
    if (strncmp(filepath, prefix, strlen(prefix)) != 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"bad path\"}", 33);
        return;
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"open file\"}", 35);
        return;
    }

    if (body_len > 0 &&
        fwrite(body, 1, body_len, fp) != body_len) {
        fclose(fp);
        unlink(filepath);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write\"}", 30);
        return;
    }

    fclose(fp);

    char resp[768];
    int  rl = snprintf(resp, sizeof(resp),
                       "{\"ok\":true,\"path\":\"report/%s/%s/%s\"}",
                       userdir, yyyymm, filename);
    if (rl < 0 || (size_t)rl >= sizeof(resp)) {
        send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    } else {
        send_json(client_fd, 200, "OK", resp, (size_t)rl);
    }

    LOG_INFO("save_config  %s  (%zu bytes)", filepath, body_len);
}

/* ------------------------------------------------------------------ */
/*  GET /api/reports — 列出已存档 .html / .json（旧路径 + 按用户分子目录） */
/* ------------------------------------------------------------------ */

typedef struct {
    char      name[256];
    char      kind[8]; /* "html" or "json" */
    long long mtime;
    long long size;
} report_one_t;

static int dir_name_is_yyyymm(const char *name)
{
    size_t i, n = strlen(name);
    if (n != 6)
        return 0;
    for (i = 0; i < n; i++)
        if (!isdigit((unsigned char)name[i]))
            return 0;
    return 1;
}

static int report_html_basename_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name)
        return 0;
    n = strlen(name);
    if (n < 6 || strcasecmp(name + n - 5, ".html") != 0)
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

static int cmp_report_one_desc(const void *a, const void *b)
{
    const report_one_t *x = (const report_one_t *)a;
    const report_one_t *y = (const report_one_t *)b;
    if (y->mtime > x->mtime)
        return 1;
    if (y->mtime < x->mtime)
        return -1;
    return strcmp(x->name, y->name);
}

static int report_json_basename_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name)
        return 0;
    n = strlen(name);
    if (n < 6 || strcasecmp(name + n - 5, ".json") != 0)
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

/* report 下第一层：用户名目录（非六位年月、安全字符） */
static int report_user_first_segment_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name || name[0] == '.')
        return 0;
    if (dir_name_is_yyyymm(name))
        return 0;
    n = strlen(name);
    if (n > 80)
        return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

#define REPORTS_MAX_GROUPS     400
#define REPORTS_FILES_PER_GRP  200

typedef struct {
    char         user[96];
    int          legacy; /* 1: 旧路径 report/YM/… */
    char         ym[8];
    report_one_t files[REPORTS_FILES_PER_GRP];
    int          nf;
} report_grp_t;

static int cmp_report_grp_desc(const void *a, const void *b)
{
    const report_grp_t *x = (const report_grp_t *)a;
    const report_grp_t *y = (const report_grp_t *)b;
    int                 c = strcmp(y->ym, x->ym);
    if (c != 0)
        return c;
    if (x->legacy != y->legacy)
        return x->legacy - y->legacy;
    return strcmp(x->user, y->user);
}

static void scan_report_dir_archive_files(const char *sub, report_grp_t *g)
{
    DIR           *sd = opendir(sub);
    struct dirent *de;
    if (!sd)
        return;
    while (g->nf < REPORTS_FILES_PER_GRP) {
        de = readdir(sd);
        if (!de)
            break;
        int is_html = report_html_basename_ok(de->d_name);
        int is_json = !is_html && report_json_basename_ok(de->d_name);
        if (!is_html && !is_json)
            continue;
        {
            char        fp[768];
            struct stat st;
            snprintf(fp, sizeof(fp), "%s/%s", sub, de->d_name);
            if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            strncpy(g->files[g->nf].name, de->d_name,
                    sizeof(g->files[g->nf].name) - 1);
            g->files[g->nf].name[sizeof(g->files[g->nf].name) - 1] = '\0';
            strncpy(g->files[g->nf].kind, is_json ? "json" : "html",
                    sizeof(g->files[g->nf].kind) - 1);
            g->files[g->nf].kind[sizeof(g->files[g->nf].kind) - 1] = '\0';
            g->files[g->nf].mtime = (long long)st.st_mtime;
            g->files[g->nf].size  = (long long)st.st_size;
            g->nf++;
        }
    }
    closedir(sd);
}

/* GET /api/reports：兼容旧路径 report 下六位年月目录，以及 report 下用户名子目录 */
static void handle_api_reports(int client_fd)
{
    char          base[384];
    DIR          *d;
    strbuf_t      sb = {0};
    report_grp_t  groups[REPORTS_MAX_GROUPS];
    int           ng = 0;
    struct dirent *de;

    snprintf(base, sizeof(base), "%s/report", WEB_ROOT);
    d = opendir(base);
    if (!d) {
        send_json(client_fd, 200, "OK", "{\"months\":[],\"groups\":[]}", 28);
        return;
    }

    /* 旧布局：report 下六位年月目录中的 .html */
    while (ng < REPORTS_MAX_GROUPS) {
        de = readdir(d);
        if (!de)
            break;
        if (de->d_name[0] == '.')
            continue;
        if (!dir_name_is_yyyymm(de->d_name))
            continue;
        {
            report_grp_t *g = &groups[ng];
            char          sub[512];

            memset(g, 0, sizeof(*g));
            g->legacy = 1;
            strncpy(g->ym, de->d_name, 7);
            g->ym[7] = '\0';
            snprintf(sub, sizeof(sub), "%s/%s", base, de->d_name);
            scan_report_dir_archive_files(sub, g);
            if (g->nf > 0)
                ng++;
        }
    }
    rewinddir(d);

    /* 新布局：report 下用户目录再下六位年月目录中的 .html / .json */
    while (ng < REPORTS_MAX_GROUPS) {
        de = readdir(d);
        if (!de)
            break;
        if (de->d_name[0] == '.')
            continue;
        if (!report_user_first_segment_ok(de->d_name))
            continue;
        {
            char userbase[512];
            DIR *ud;

            snprintf(userbase, sizeof(userbase), "%s/%s", base, de->d_name);
            ud = opendir(userbase);
            if (!ud)
                continue;
            while (ng < REPORTS_MAX_GROUPS) {
                struct dirent *ue = readdir(ud);
                if (!ue)
                    break;
                if (ue->d_name[0] == '.')
                    continue;
                if (!dir_name_is_yyyymm(ue->d_name))
                    continue;
                {
                    report_grp_t *g = &groups[ng];
                    char          sub[640];

                    memset(g, 0, sizeof(*g));
                    g->legacy = 0;
                    strncpy(g->user, de->d_name,
                            sizeof(g->user) - 1);
                    g->user[sizeof(g->user) - 1] = '\0';
                    strncpy(g->ym, ue->d_name, 7);
                    g->ym[7] = '\0';
                    snprintf(sub, sizeof(sub), "%s/%s", userbase, ue->d_name);
                    scan_report_dir_archive_files(sub, g);
                    if (g->nf > 0)
                        ng++;
                }
            }
            closedir(ud);
        }
    }
    closedir(d);

    qsort(groups, (size_t)ng, sizeof(groups[0]), cmp_report_grp_desc);

    SB_LIT(&sb, "{\"months\":[],\"groups\":[");
    {
        int gi, fi, first_g = 1;
        for (gi = 0; gi < ng; gi++) {
            report_grp_t *g = &groups[gi];
            if (!first_g)
                SB_LIT(&sb, ",");
            first_g = 0;
            SB_LIT(&sb, "{\"legacy\":");
            sb_appendf(&sb, "%s", g->legacy ? "true" : "false");
            SB_LIT(&sb, ",\"user\":");
            sb_json_str(&sb, g->user);
            SB_LIT(&sb, ",\"ym\":\"");
            sb_append(&sb, g->ym, strlen(g->ym));
            SB_LIT(&sb, "\",\"files\":[");

            qsort(g->files, (size_t)g->nf, sizeof(g->files[0]),
                  cmp_report_one_desc);
            for (fi = 0; fi < g->nf; fi++) {
                if (fi)
                    SB_LIT(&sb, ",");
                SB_LIT(&sb, "{\"name\":");
                sb_json_str(&sb, g->files[fi].name);
                SB_LIT(&sb, ",\"kind\":");
                sb_json_str(&sb, g->files[fi].kind[0] ? g->files[fi].kind : "html");
                sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}",
                           (long long)g->files[fi].mtime,
                           (long long)g->files[fi].size);
            }
            SB_LIT(&sb, "]}");
        }
    }
    SB_LIT(&sb, "]}");

    if (sb.data) {
        send_json(client_fd, 200, "OK", sb.data, sb.len);
        free(sb.data);
    } else {
        send_json(client_fd, 200, "OK", "{\"months\":[],\"groups\":[]}", 28);
    }
}

/* POST /api/delete-report — 删除 report 下已存档 .html / .json（JSON 体） */
static void handle_api_delete_report(int client_fd, const char *body)
{
    char name[256] = {0};
    char user[128] = {0};
    char ym[16]    = {0};
    char filepath[800];
    int  legacy    = 0;

    if (!body || !body[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"empty body\"}", 35);
        return;
    }
    {
        const char *p = strstr(body, "\"legacy\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (strncmp(p, "true", 4) == 0) legacy = 1;
            }
        }
    }
    if (json_get_str(body, "name", name, sizeof(name)) < 0 || !name[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing name\"}", 37);
        return;
    }
    sanitize_report_archive_name(name);
    if (!report_html_basename_ok(name) && !report_json_basename_ok(name)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid filename\"}", 40);
        return;
    }
    if (json_get_str(body, "ym", ym, sizeof(ym)) < 0 || !dir_name_is_yyyymm(ym)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid ym\"}", 33);
        return;
    }

    if (legacy) {
        snprintf(filepath, sizeof(filepath), "%s/report/%s/%s", WEB_ROOT, ym,
                 name);
    } else {
        if (json_get_str(body, "user", user, sizeof(user)) < 0 || !user[0]) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"missing user\"}", 37);
            return;
        }
        sanitize_report_user_dir(user, sizeof(user));
        snprintf(filepath, sizeof(filepath), "%s/report/%s/%s/%s", WEB_ROOT,
                 user, ym, name);
    }

    {
        char prefix[400];
        snprintf(prefix, sizeof(prefix), "%s/report/", WEB_ROOT);
        if (strncmp(filepath, prefix, strlen(prefix)) != 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"path\"}", 28);
            return;
        }
    }

    if (unlink(filepath) != 0) {
        int code = 500;
        if (errno == ENOENT) code = 404;
        send_json(client_fd, code, code == 404 ? "Not Found" : "Internal Server Error",
                  "{\"ok\":false,\"error\":\"unlink failed\"}", 33);
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
}

static int query_param_get(const char *path, const char *key, char *out,
                           size_t cap)
{
    const char *q = strchr(path, '?');
    size_t      klen;

    if (!q || cap < 2)
        return -1;
    q++;
    klen = strlen(key);
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            size_t i = 0;
            while (*q && *q != '&' && i + 1 < cap) {
                if (*q == '%' && isxdigit((unsigned char)q[1]) &&
                    isxdigit((unsigned char)q[2])) {
                    char hx[3] = { q[1], q[2], '\0' };
                    out[i++] = (char)strtol(hx, NULL, 16);
                    q += 3;
                } else if (*q == '+') {
                    out[i++] = ' ';
                    q++;
                } else {
                    out[i++] = *q++;
                }
            }
            out[i] = '\0';
            return 0;
        }
        {
            const char *amp = strchr(q, '&');
            if (!amp)
                break;
            q = amp + 1;
        }
    }
    return -1;
}

/* GET /api/list-ssh-configs?user= ：列出各 YYYYMM 子目录下的 .json 配置 */
static void handle_api_list_ssh_configs(int client_fd, const char *path)
{
    char     userdir[128];
    char     ubase[512];
    DIR     *d;
    strbuf_t sb = {0};

    if (query_param_get(path, "user", userdir, sizeof(userdir)) != 0)
        strncpy(userdir, "root", sizeof(userdir));
    userdir[sizeof(userdir) - 1] = '\0';
    sanitize_report_user_dir(userdir, sizeof(userdir));

    snprintf(ubase, sizeof(ubase), "%s/report/%s", WEB_ROOT, userdir);

    SB_LIT(&sb, "{\"ok\":true,\"user\":");
    sb_json_str(&sb, userdir);
    SB_LIT(&sb, ",\"files\":[");

    d = opendir(ubase);
    if (d) {
        int          first = 1;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char sub[640];
            DIR *sd;

            if (de->d_name[0] == '.')
                continue;
            if (!dir_name_is_yyyymm(de->d_name))
                continue;
            snprintf(sub, sizeof(sub), "%s/%s", ubase, de->d_name);
            sd = opendir(sub);
            if (!sd)
                continue;
            while (1) {
                struct dirent *fe = readdir(sd);
                char           fp[768];
                struct stat    st;

                if (!fe)
                    break;
                if (!report_json_basename_ok(fe->d_name))
                    continue;
                snprintf(fp, sizeof(fp), "%s/%s", sub, fe->d_name);
                if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
                    continue;
                if (!first)
                    SB_LIT(&sb, ",");
                first = 0;
                SB_LIT(&sb, "{\"ym\":\"");
                sb_append(&sb, de->d_name, strlen(de->d_name));
                SB_LIT(&sb, "\",\"name\":");
                sb_json_str(&sb, fe->d_name);
                sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}",
                           (long long)st.st_mtime, (long long)st.st_size);
            }
            closedir(sd);
        }
        closedir(d);
    }
    SB_LIT(&sb, "]}");

    if (sb.data) {
        send_json(client_fd, 200, "OK", sb.data, sb.len);
        free(sb.data);
    } else {
        send_json(client_fd, 200, "OK",
                  "{\"ok\":true,\"user\":\"root\",\"files\":[]}", 40);
    }
}

/* GET /api/list-all-configs : 列出 report/ 下所有用户目录中各月份的 .json 配置 */
static void handle_api_list_all_configs(int client_fd)
{
    char     rbase[512];
    DIR     *d;
    strbuf_t sb = {0};

    snprintf(rbase, sizeof(rbase), "%s/report", WEB_ROOT);
    SB_LIT(&sb, "{\"ok\":true,\"files\":[");

    d = opendir(rbase);
    if (d) {
        int first = 1;
        struct dirent *ue;
        while ((ue = readdir(d)) != NULL) {
            char udir[640];
            DIR *ud;
            struct dirent *de;

            if (!report_user_first_segment_ok(ue->d_name))
                continue;
            snprintf(udir, sizeof(udir), "%s/%s", rbase, ue->d_name);
            ud = opendir(udir);
            if (!ud)
                continue;
            while ((de = readdir(ud)) != NULL) {
                char sub[768];
                DIR *sd;
                struct dirent *fe;

                if (!dir_name_is_yyyymm(de->d_name))
                    continue;
                snprintf(sub, sizeof(sub), "%s/%s", udir, de->d_name);
                sd = opendir(sub);
                if (!sd)
                    continue;
                while ((fe = readdir(sd)) != NULL) {
                    char fp[896];
                    struct stat st;

                    if (!report_json_basename_ok(fe->d_name))
                        continue;
                    snprintf(fp, sizeof(fp), "%s/%s", sub, fe->d_name);
                    if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
                        continue;
                    if (!first)
                        SB_LIT(&sb, ",");
                    first = 0;
                    SB_LIT(&sb, "{\"user\":");
                    sb_json_str(&sb, ue->d_name);
                    SB_LIT(&sb, ",\"ym\":\"");
                    sb_append(&sb, de->d_name, strlen(de->d_name));
                    SB_LIT(&sb, "\",\"name\":");
                    sb_json_str(&sb, fe->d_name);
                    sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}",
                               (long long)st.st_mtime, (long long)st.st_size);
                }
                closedir(sd);
            }
            closedir(ud);
        }
        closedir(d);
    }
    SB_LIT(&sb, "]}");

    if (sb.data) {
        send_json(client_fd, 200, "OK", sb.data, sb.len);
        free(sb.data);
    } else {
        send_json(client_fd, 200, "OK", "{\"ok\":true,\"files\":[]}", 22);
    }
}

/* GET /api/client-info — TCP 对端 IPv4（浏览器所在机器），供存档默认名与链接预览 */
static void handle_api_client_info(int client_fd, const char *client_ip)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ip\":");
    sb_json_str(&sb, client_ip ? client_ip : "");
    SB_LIT(&sb, "}");
    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error", "{\"ip\":\"\"}", 9);
    free(sb.data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec-one  (单条命令，实时响应)                       */
/* ------------------------------------------------------------------ */

#define MAX_BODY_SIZE        (64 * 1024)
#define SAVE_REPORT_MAX_BODY (5 * 1024 * 1024) /* POST /api/save-report 正文上限 */
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
    json_api_get_pass(body, pass, sizeof(pass));
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

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\",\"exit_code\":-1,\"output\":\"\"}", 51);
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
    json_api_get_pass(body, pass, sizeof(pass));
    port    = json_get_int(body, "port",    22);
    int timeout = json_get_int(body, "timeout", 0);
    if (!user[0]) strcpy(user, "root");

    /* 提取命令数组（堆分配，避免 1000×2048=2MB 栈压力） */
    char (*cmd_bufs)[CMD_BUF_SIZE] = calloc(MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (!cmd_bufs) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\"}", 24); return;
    }
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];

    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        free(cmd_bufs);
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec: %s@%s:%d  commands=%d timeout=%d",
             user, host, port, cmd_count, timeout);

    ssh_batch_t *result = ssh_session_exec(host, port, user, pass,
                                           cmd_ptrs, cmd_count, timeout);

    /* 构造 JSON 响应 */
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    SB_LIT(&sb, ",\"results\":[");

    if (result && !result->error[0]) {
        for (int i = 0; i < result->count; i++) {
            if (i > 0) SB_LIT(&sb, ",");
            SB_LIT(&sb, "{\"cmd\":");
            sb_json_str(&sb, result->results[i].cmd     ? result->results[i].cmd     : "");
            sb_appendf(&sb, ",\"exit_code\":%d", result->results[i].exit_code);
            SB_LIT(&sb, ",\"output\":");
            sb_json_str(&sb, result->results[i].output  ? result->results[i].output  : "");
            SB_LIT(&sb, ",\"workdir\":");
            sb_json_str(&sb, result->results[i].workdir ? result->results[i].workdir : "");
            SB_LIT(&sb, "}");
        }
    }
    SB_LIT(&sb, "]}");

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\",\"results\":[]}", 38);

    free(sb.data);
    free(cmd_bufs);
    if (result) ssh_batch_free(result);
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec-stream  (SSE 流式，每条命令完成即推送)          */
/* ------------------------------------------------------------------ */

/* 返回 0 成功，-1 连接已断（EPIPE / ECONNRESET） */
static int sse_write_json(int fd, const char *json)
{
    /* 用 writev 一次性写出，避免 Nagle 算法将三次小写分批延迟发送 */
    struct iovec iov[3];
    iov[0].iov_base = (void *)"data: ";
    iov[0].iov_len  = 6;
    iov[1].iov_base = (void *)json;
    iov[1].iov_len  = strlen(json);
    iov[2].iov_base = (void *)"\n\n";
    iov[2].iov_len  = 2;
    ssize_t w = writev(fd, iov, 3);
    return (w < 0) ? -1 : 0;
}

typedef struct {
    int   fd;
    char (*cmds)[CMD_BUF_SIZE];
    int   completed;
    int   conn_broken;   /* 1 = 客户端已断开，停止写入并取消 SSH 会话 */
} stream_ctx_t;

/* 写入失败时：标记断开、立即杀掉 SSH 进程组，避免进程泄漏 */
static void sse_on_write_fail(stream_ctx_t *ctx)
{
    if (!ctx->conn_broken) {
        ctx->conn_broken = 1;
        LOG_INFO("sse_stream: client disconnected, cancelling SSH session");
        ssh_cancel_current();
    }
}

static void on_stream_result(int idx, const char *cmd, const char *output,
                              int exit_code, const char *prompt_after, void *ud)
{
    stream_ctx_t *ctx = (stream_ctx_t *)ud;
    int           fd  = ctx->fd;
    strbuf_t      sb  = {0};

    /* 连接已断：跳过所有写入（ssh_cancel_current 已在首次失败时调用） */
    if (ctx->conn_broken) return;

    if (idx == -3) {
        /* PTY 部分输出事件（实时进度）：prompt_after 存放命令 0-based 下标字符串 */
        int cmd_i = (prompt_after && *prompt_after) ? atoi(prompt_after) : 0;
        sb_appendf(&sb, "{\"type\":\"partial\",\"i\":%d,\"output\":", cmd_i);
        sb_json_str(&sb, output ? output : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }
    if (idx == -2) {
        /* PTY 诊断事件：cmd=info串，output=tail片段 */
        sb_appendf(&sb, "{\"type\":\"diag\",\"info\":");
        sb_json_str(&sb, cmd ? cmd : "");
        sb_appendf(&sb, ",\"tail\":");
        sb_json_str(&sb, output ? output : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }
    if (idx == -1) {
        SB_LIT(&sb, "{\"type\":\"session_prompt\",\"prompt\":");
        sb_json_str(&sb, prompt_after ? prompt_after : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }

    ctx->completed++;
    sb_appendf(&sb, "{\"type\":\"result\",\"i\":%d,\"cmd\":", idx);
    sb_json_str(&sb, cmd ? cmd : "");
    sb_appendf(&sb, ",\"exit_code\":%d,\"output\":", exit_code);
    sb_json_str(&sb, output ? output : "");
    SB_LIT(&sb, ",\"prompt_after\":");
    sb_json_str(&sb, prompt_after ? prompt_after : "");
    SB_LIT(&sb, "}");
    if (sb.data) {
        if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
        free(sb.data);
    }
}

static void handle_api_ssh_exec_stream(int client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_api_get_pass(body, pass, sizeof(pass));
    port             = json_get_int(body, "port",       22);
    int timeout      = json_get_int(body, "timeout",    0);
    int net_device   = json_get_int(body, "net_device", 0);
    int pty_debug    = json_get_int(body, "pty_debug",  0);
    if (!user[0]) strcpy(user, "root");

    char (*cmd_bufs)[CMD_BUF_SIZE] = calloc(MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (!cmd_bufs) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\"}", 24); return;
    }
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];
    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        free(cmd_bufs);
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec_stream: %s@%s:%d  commands=%d timeout_req=%d pty_debug=%d",
             user, host, port, cmd_count, timeout, pty_debug);

    /* 禁用 Nagle 算法：确保每次 writev 后立即发送，SSE 事件不被攒批 */
    {
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&flag, sizeof(flag));
    }

    /* SSE 响应头（无 Content-Length，流式；X-Accel-Buffering:no 告知 nginx 不缓冲） */
    const char *sse_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Accel-Buffering: no\r\n"
        "Connection: close\r\n"
        "\r\n";
    write(client_fd, sse_hdr, strlen(sse_hdr));

    if (pty_debug) {
        int eff_to = (timeout > 0) ? timeout : 300;
        strbuf_t ds = {0};
        sb_appendf(&ds,
                   "{\"type\":\"debug\",\"pty_debug\":1,\"timeout_req\":%d,"
                   "\"effective_timeout_sec\":%d,"
                   "\"log\":\"server file logs: ssh_pty_diag / ssh_session_exec_stream\"}",
                   timeout, eff_to);
        if (ds.data) {
            sse_write_json(client_fd, ds.data);  /* 初始化阶段写失败：直接返回即可 */
            free(ds.data);
        }
    }

    /* 流式执行 */
    stream_ctx_t ctx = { client_fd, cmd_bufs, 0, 0 };
    char  error_buf[512] = {0};
    int   timed_out = 0;
    int   timeout_cmd_idx = -1;
#define PARTIAL_BUF_MAX (64 * 1024)
    char *partial_buf = calloc(1, PARTIAL_BUF_MAX);
    ssh_session_exec_stream(host, port, user, pass,
                            cmd_ptrs, cmd_count,
                            on_stream_result, &ctx,
                            error_buf, sizeof(error_buf), timeout,
                            &timed_out,
                            &timeout_cmd_idx,
                            partial_buf, partial_buf ? PARTIAL_BUF_MAX : 0,
                            net_device,
                            pty_debug);

    /* 结束事件（连接已断时跳过，SSH 进程已在写入失败时 cancel） */
    if (!ctx.conn_broken) {
        int timeout_sec = (timeout > 0) ? timeout : 300;
        strbuf_t sb = {0};
        if (timed_out) {
            sb_appendf(&sb, "{\"type\":\"timeout\",\"completed\":%d,\"total\":%d,\"timeout_sec\":%d",
                       ctx.completed, cmd_count, timeout_sec);
            if (timeout_cmd_idx >= 0)
                sb_appendf(&sb, ",\"i\":%d", timeout_cmd_idx);
            if (partial_buf && partial_buf[0]) {
                SB_LIT(&sb, ",\"partial\":");
                sb_json_str(&sb, partial_buf);
            }
            SB_LIT(&sb, "}");
        } else if (error_buf[0]) {
            SB_LIT(&sb, "{\"type\":\"error\",\"message\":");
            sb_json_str(&sb, error_buf);
            SB_LIT(&sb, "}");
        } else {
            sb_appendf(&sb, "{\"type\":\"done\",\"total\":%d}", cmd_count);
        }
        if (sb.data) { sse_write_json(client_fd, sb.data); free(sb.data); }
    }
    free(partial_buf);
    free(cmd_bufs);
}

/* ================================================================
   监控统计模块
   ================================================================ */
#include <time.h>

/* ── 请求 / 连接计数 ── */
static volatile long  g_total_reqs   = 0;
static volatile int   g_active_conns = 0;
static volatile int   g_peak_active  = 0;
static time_t         g_start_time   = 0;

/* ── 在线用户（5分钟窗口内唯一 IP） ── */
#define ONLINE_WINDOW_SEC 300
#define MAX_IP_SLOTS      512

typedef struct { char ip[16]; time_t ts; } ip_slot_t;
static ip_slot_t       g_ip_slots[MAX_IP_SLOTS];
static int             g_ip_count    = 0;
static int             g_peak_online = 0;
static pthread_mutex_t g_ip_mutex    = PTHREAD_MUTEX_INITIALIZER;

/* ── CPU 快照（增量法） ── */
#define MAX_CORES 64
typedef struct { unsigned long long work; unsigned long long total; } cpu_snap_t;
static cpu_snap_t      g_prev_sys              = {0, 0};
static cpu_snap_t      g_prev_proc             = {0, 0};
static cpu_snap_t      g_prev_core[MAX_CORES];
static time_t          g_prev_ts               = 0;
static float           g_cpu_sys_pct           = 0.0f;
static float           g_cpu_proc_pct          = 0.0f;
static float           g_cpu_core_pct[MAX_CORES];
static int             g_cpu_core_count        = 0;
static pthread_mutex_t g_cpu_mutex             = PTHREAD_MUTEX_INITIALIZER;

/* ── 每核 / 全局 Top 进程 ── */
#define TOP_PER_CORE  3
#define TOP_GLOBAL   10
#define MAX_PROCS  2048

typedef struct { char name[32]; char user[20]; float pct; pid_t pid; } top_proc_t;
typedef struct { pid_t pid; unsigned long long ticks; } proc_snap_t;

static top_proc_t      g_core_top[MAX_CORES][TOP_PER_CORE];
static int             g_core_top_cnt[MAX_CORES];
static top_proc_t      g_global_top[TOP_GLOBAL];
static int             g_global_top_cnt = 0;
static proc_snap_t     g_prev_procs[MAX_PROCS];
static int             g_prev_proc_count  = 0;
static pthread_mutex_t g_proc_mutex       = PTHREAD_MUTEX_INITIALIZER;
static double          g_last_scan_dt     = 3.0; /* 上次 scan 的采样间隔，供 /api/procs 估算 CPU% */

/* ── UID→用户名缓存（避免对每个进程重复调 getpwuid_r） ── */
#define UID_CACHE_SIZE 64
typedef struct { uid_t uid; char name[32]; } uid_ce_t;
static uid_ce_t        g_uid_cache[UID_CACHE_SIZE];
static int             g_uid_cache_cnt = 0;
static pthread_mutex_t g_uid_mutex     = PTHREAD_MUTEX_INITIALIZER;

static void uid_lookup(uid_t uid, char *out, int outlen)
{
    pthread_mutex_lock(&g_uid_mutex);
    for (int i = 0; i < g_uid_cache_cnt; i++) {
        if (g_uid_cache[i].uid == uid) {
            strncpy(out, g_uid_cache[i].name, (size_t)(outlen - 1));
            out[outlen - 1] = '\0';
            pthread_mutex_unlock(&g_uid_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_uid_mutex);

    /* cache miss: 调系统接口 */
    struct passwd pw0, *pw = NULL; char pbuf[256];
    char name[32] = "?";
    if (getpwuid_r(uid, &pw0, pbuf, sizeof(pbuf), &pw) == 0 && pw)
        strncpy(name, pw->pw_name, 31);
    else
        snprintf(name, sizeof(name), "%u", (unsigned)uid);
    name[31] = '\0';

    pthread_mutex_lock(&g_uid_mutex);
    int slot = (g_uid_cache_cnt < UID_CACHE_SIZE)
               ? g_uid_cache_cnt++ : (int)((unsigned)uid % UID_CACHE_SIZE);
    g_uid_cache[slot].uid = uid;
    memcpy(g_uid_cache[slot].name, name, 32);
    pthread_mutex_unlock(&g_uid_mutex);

    strncpy(out, name, (size_t)(outlen - 1));
    out[outlen - 1] = '\0';
}

/* ── /api/monitor 响应缓存 ── */
#define MONITOR_CACHE_SEC 8
static char           *g_monitor_json     = NULL;
static size_t          g_monitor_json_len = 0;
static time_t          g_monitor_json_ts  = 0;
static pthread_mutex_t g_monitor_json_mtx    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_monitor_update_mtx  = PTHREAD_MUTEX_INITIALIZER;

/* ── /api/procs 响应缓存（全量扫 /proc 开销大，相同 q+ports 短时复用） ── */
#define PROCS_CACHE_SEC 4
static char           *g_procs_json     = NULL;
static size_t          g_procs_json_len = 0;
static time_t          g_procs_json_ts  = 0;
static char            g_procs_cache_key[160] = "";
static pthread_mutex_t g_procs_json_mtx = PTHREAD_MUTEX_INITIALIZER;

void stats_init(void) { g_start_time = time(NULL); }

/* 记录请求开始 */
static void stats_req_start(const char *ip)
{
    __sync_fetch_and_add(&g_total_reqs, 1);
    int active = __sync_add_and_fetch(&g_active_conns, 1);
    /* 更新峰值并发 */
    int peak = g_peak_active;
    while (active > peak) {
        if (__sync_bool_compare_and_swap(&g_peak_active, peak, active)) break;
        peak = g_peak_active;
    }
    /* 记录 IP */
    time_t now = time(NULL);
    pthread_mutex_lock(&g_ip_mutex);
    int found = 0;
    int oldest_i = 0;
    for (int i = 0; i < g_ip_count; i++) {
        if (g_ip_slots[i].ts < g_ip_slots[oldest_i].ts) oldest_i = i;
        if (strncmp(g_ip_slots[i].ip, ip, 15) == 0) {
            g_ip_slots[i].ts = now; found = 1; break;
        }
    }
    if (!found) {
        if (g_ip_count < MAX_IP_SLOTS) {
            strncpy(g_ip_slots[g_ip_count].ip, ip, 15);
            g_ip_slots[g_ip_count++].ts = now;
        } else {
            strncpy(g_ip_slots[oldest_i].ip, ip, 15);
            g_ip_slots[oldest_i].ts = now;
        }
    }
    int online = 0;
    for (int i = 0; i < g_ip_count; i++)
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) online++;
    if (online > g_peak_online) g_peak_online = online;
    pthread_mutex_unlock(&g_ip_mutex);
}

static void stats_req_end(void) { __sync_fetch_and_sub(&g_active_conns, 1); }

/* 一次读取 /proc/stat，同时获取汇总行（cpu）和各核行（cpu0/cpu1/...）
 * 避免两次 open() 系统调用 */
static int snap_sys_and_cores(cpu_snap_t *sys, cpu_snap_t *cores,
                               int max_cores, int *count)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    *count = 0;
    int sys_done = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;   /* cpu 行结束 */
        unsigned long long u=0, n=0, sy=0, id=0, iow=0, irq=0, si=0, st=0;
        char *p = line + 3;
        if (*p == ' ' || *p == '\t') {
            /* 汇总行 "cpu " */
            if (!sys_done &&
                sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &sy, &id, &iow, &irq, &si, &st) >= 4) {
                sys->work  = u + n + sy + irq + si + st;
                sys->total = sys->work + id + iow;
                sys_done = 1;
            }
        } else if (*p >= '0' && *p <= '9' && *count < max_cores) {
            /* 各核行 "cpuN " */
            while (*p && *p != ' ' && *p != '\t') p++;
            if (sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &sy, &id, &iow, &irq, &si, &st) >= 4) {
                cores[*count].work  = u + n + sy + irq + si + st;
                cores[*count].total = cores[*count].work + id + iow;
                (*count)++;
            }
        }
    }
    fclose(f);
    return sys_done ? 0 : -1;
}

/* 读取进程 CPU 快照 (/proc/self/stat，utime+stime，clock ticks) */
static int snap_proc_cpu(cpu_snap_t *s)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    char buf[512] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    unsigned long long ut = 0, st2 = 0;
    sscanf(p + 1,
           " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
           &ut, &st2);
    s->work  = ut + st2;
    s->total = s->work;
    return 0;
}

/* 读取 /proc/[pid]/stat：进程名、ticks（utime+stime）、last_cpu */
static int read_proc_stat_entry(pid_t pid, char *name, int nl,
                                unsigned long long *ticks, int *cpu)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[512] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *s = strchr(buf, '('), *e = strrchr(buf, ')');
    if (!s || !e || e <= s) return -1;
    int n = (int)(e - s - 1);
    if (n >= nl) n = nl - 1;
    memcpy(name, s + 1, (size_t)n); name[n] = '\0';
    unsigned long long ut = 0, st = 0; int proc = 0;
    /* 跳过 state ppid pgrp session tty tpgid flags minflt×4 → utime stime
       再跳 cutime cstime priority nice nthreads itrealvalue starttime vsize rss rsslim
       startcode endcode startstack kstkesp kstkeip signal blocked sigignore sigcatch
       wchan nswap cnswap exit_signal → processor */
    int r = sscanf(e + 1,
        " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u"
        " %llu %llu"
        " %*d %*d %*d %*d %*d %*d %*u %*u %*d"
        " %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d"
        " %d", &ut, &st, &proc);
    if (r < 3) return -1;
    *ticks = ut + st;
    *cpu   = proc;
    return 0;
}

/* 读取 /proc/[pid]/status 的 UID，通过缓存转换为用户名 */
static void read_proc_user(pid_t pid, char *user, int ul)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { strncpy(user, "?", (size_t)ul); return; }
    char line[128]; uid_t uid = (uid_t)-1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "Uid:", 4) == 0) { sscanf(line + 4, " %u", &uid); break; }
    fclose(f);
    if (uid == (uid_t)-1) { strncpy(user, "?", (size_t)ul); return; }
    uid_lookup(uid, user, ul);
}

/* 插入排序维护 top-N（按 pct 降序） */
static void top_insert(top_proc_t top[], int *cnt, int max,
                       const char *name, const char *user, float pct, pid_t pid)
{
    int tc = *cnt, pos = tc;
    for (int i = 0; i < tc; i++) { if (pct > top[i].pct) { pos = i; break; } }
    if (pos >= max) return;
    int last = (tc < max) ? tc - 1 : max - 2;
    for (int k = last; k >= pos; k--) top[k + 1] = top[k];
    strncpy(top[pos].name, name, 31); top[pos].name[31] = '\0';
    strncpy(top[pos].user, user, 19); top[pos].user[19] = '\0';
    top[pos].pct = pct;
    top[pos].pid = pid;
    if (tc < max) (*cnt)++;
}

/* 扫描 /proc，更新每核 Top-3 和全局 Top-10 */
static void scan_proc_top(double dt, int core_count)
{
    if (dt < 0.3 || core_count <= 0) return;

    /* trylock：另一线程正在扫描时直接跳过，避免并发重扫 /proc */
    static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
    static time_t          last_scan = 0;
    if (pthread_mutex_trylock(&scan_lock) != 0) return;
    /* 至少间隔 2 秒再全量扫 /proc（monitor/procs 并发时避免 CPU 尖峰） */
    time_t scan_now = time(NULL);
    if (last_scan != 0 && difftime(scan_now, last_scan) < 2.0) {
        pthread_mutex_unlock(&scan_lock);
        return;
    }
    last_scan = scan_now;

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double scale = 100.0 / ((double)clk * dt);

    /* 读取上次快照副本（在 mutex 外完成文件扫描） */
    static proc_snap_t prev_copy[MAX_PROCS];
    int prev_count;
    pthread_mutex_lock(&g_proc_mutex);
    prev_count = g_prev_proc_count;
    memcpy(prev_copy, g_prev_procs, sizeof(proc_snap_t) * (size_t)prev_count);
    pthread_mutex_unlock(&g_proc_mutex);

    /* 临时工作区 */
    static proc_snap_t cur[MAX_PROCS];
    int cur_count = 0;

    top_proc_t tmp_core[MAX_CORES][TOP_PER_CORE];
    int        tmp_core_cnt[MAX_CORES];
    memset(tmp_core_cnt, 0, sizeof(int) * (size_t)core_count);

    top_proc_t tmp_global[TOP_GLOBAL];
    int        tmp_global_cnt = 0;

    /* 扫描 /proc */
    DIR *dir = opendir("/proc");
    if (!dir) { pthread_mutex_unlock(&scan_lock); return; }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && cur_count < MAX_PROCS) {
        const char *p = de->d_name;
        if (*p < '1' || *p > '9') continue;
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        if (*q != '\0') continue;

        pid_t pid = (pid_t)atoi(de->d_name);
        char name[32]; unsigned long long ticks; int last_cpu;
        if (read_proc_stat_entry(pid, name, sizeof(name), &ticks, &last_cpu) < 0)
            continue;
        if (last_cpu < 0 || last_cpu >= core_count) last_cpu = 0;

        cur[cur_count].pid   = pid;
        cur[cur_count].ticks = ticks;
        cur_count++;

        unsigned long long prev_ticks = 0;
        for (int i = 0; i < prev_count; i++)
            if (prev_copy[i].pid == pid) { prev_ticks = prev_copy[i].ticks; break; }

        float pct = (ticks > prev_ticks)
                    ? (float)((double)(ticks - prev_ticks) * scale) : 0.0f;
        if (pct < 0.05f) continue;

        char user[20] = "?";
        read_proc_user(pid, user, sizeof(user));

        top_insert(tmp_core[last_cpu], &tmp_core_cnt[last_cpu], TOP_PER_CORE,
                   name, user, pct, pid);
        top_insert(tmp_global, &tmp_global_cnt, TOP_GLOBAL, name, user, pct, pid);
    }
    closedir(dir);

    /* 写回全局结果 */
    pthread_mutex_lock(&g_proc_mutex);
    for (int i = 0; i < core_count; i++) {
        g_core_top_cnt[i] = tmp_core_cnt[i];
        for (int j = 0; j < tmp_core_cnt[i]; j++)
            g_core_top[i][j] = tmp_core[i][j];
    }
    g_global_top_cnt = tmp_global_cnt;
    for (int j = 0; j < tmp_global_cnt; j++) g_global_top[j] = tmp_global[j];
    memcpy(g_prev_procs, cur, sizeof(proc_snap_t) * (size_t)cur_count);
    g_prev_proc_count = cur_count;
    g_last_scan_dt    = dt;
    pthread_mutex_unlock(&g_proc_mutex);

    pthread_mutex_unlock(&scan_lock);
}

/* 更新 CPU 百分比（与上次快照比较） */
static void update_cpu(void)
{
    cpu_snap_t sys_now, proc_now;
    cpu_snap_t core_now[MAX_CORES];
    int core_count = 0;
    time_t now = time(NULL);
    if (snap_sys_and_cores(&sys_now, core_now, MAX_CORES, &core_count) < 0) return;
    if (snap_proc_cpu(&proc_now) < 0) return;

    double dt = 0.0;
    pthread_mutex_lock(&g_cpu_mutex);
    if (g_prev_ts > 0) {
        dt = difftime(now, g_prev_ts);
        unsigned long long ds = sys_now.total - g_prev_sys.total;
        unsigned long long dw = sys_now.work  - g_prev_sys.work;
        if (ds > 0) g_cpu_sys_pct = (float)dw / (float)ds * 100.0f;

        if (dt > 0.05) {
            long clk = sysconf(_SC_CLK_TCK);
            if (clk <= 0) clk = 100;
            unsigned long long dp = proc_now.work - g_prev_proc.work;
            g_cpu_proc_pct = (float)dp / ((float)clk * (float)dt) * 100.0f;
        }

        /* 每核百分比 */
        for (int i = 0; i < core_count && i < g_cpu_core_count; i++) {
            unsigned long long cd = core_now[i].total - g_prev_core[i].total;
            unsigned long long cw = core_now[i].work  - g_prev_core[i].work;
            g_cpu_core_pct[i] = (cd > 0) ? (float)cw / (float)cd * 100.0f : 0.0f;
        }
    }
    g_prev_sys  = sys_now;
    g_prev_proc = proc_now;
    g_prev_ts   = now;
    /* 保存每核快照 */
    for (int i = 0; i < core_count; i++) g_prev_core[i] = core_now[i];
    if (core_count > 0) {
        if (g_cpu_core_count == 0)
            for (int i = 0; i < core_count; i++) g_cpu_core_pct[i] = 0.0f;
        g_cpu_core_count = core_count;
    }
    pthread_mutex_unlock(&g_cpu_mutex);

    /* 扫描进程占用（在 g_cpu_mutex 外执行以减少锁持有时间） */
    scan_proc_top(dt, core_count);
}

/* 读取系统内存 (/proc/meminfo) */
static void read_sys_mem(long *total_kb, long *used_kb)
{
    *total_kb = *used_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64];
    long val, total = 0, avail = 0;
    while (fscanf(f, "%63s %ld %*s\n", key, &val) == 2) {
        if (strcmp(key, "MemTotal:")     == 0) total = val;
        if (strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    fclose(f);
    *total_kb = total;
    *used_kb  = total - avail;
}

/* 读取进程 RSS (/proc/self/status) */
static long read_proc_rss(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char key[64]; long val = 0;
    while (fscanf(f, "%63s %ld %*s\n", key, &val) == 2)
        if (strcmp(key, "VmRSS:") == 0) { fclose(f); return val; }
    fclose(f);
    return val;
}

/* 在线人数（窗口内唯一 IP） */
static int count_online(void)
{
    time_t now = time(NULL);
    int cnt = 0;
    pthread_mutex_lock(&g_ip_mutex);
    for (int i = 0; i < g_ip_count; i++)
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) cnt++;
    pthread_mutex_unlock(&g_ip_mutex);
    return cnt;
}

/* ── /proc/net/tcp[6] inode → 本地端口（供 /api/procs?ports=1） ── */
#define TCP_INODE_MAP_MAX 4096
typedef struct { unsigned long ino; int port; } tcp_ino_port_t;

static int cmp_tcp_ino(const void *a, const void *b)
{
    unsigned long x = ((const tcp_ino_port_t *)a)->ino;
    unsigned long y = ((const tcp_ino_port_t *)b)->ino;
    return (x > y) - (x < y);
}

/* 读取 tcp/tcp6 全部行，返回条数（可能达 max_n） */
static int load_tcp_ino_port_map(tcp_ino_port_t *out, int max_n)
{
    int n = 0;
    const char *paths[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int pi = 0; pi < 2; pi++) {
        FILE *f = fopen(paths[pi], "r");
        if (!f) continue;
        char line[512];
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            if (n >= max_n) { fclose(f); return n; }
            char local_addr[72];
            unsigned int st = 0;
            unsigned long ino = 0;
            if (sscanf(line, " %*d: %71s %*s %x %*s %*s %*s %*d %*d %lu",
                       local_addr, &st, &ino) < 3)
                continue;
            char *col = strchr(local_addr, ':');
            if (!col) continue;
            unsigned int ph = 0;
            if (sscanf(col + 1, "%x", &ph) != 1) continue;
            out[n].ino  = ino;
            out[n].port = (int)ph;
            n++;
        }
        fclose(f);
    }
    return n;
}

static const tcp_ino_port_t *lookup_ino(const tcp_ino_port_t *sorted, int n, unsigned long ino)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sorted[mid].ino < ino) lo = mid + 1;
        else if (sorted[mid].ino > ino) hi = mid - 1;
        else return &sorted[mid];
    }
    return NULL;
}

/* 将 pid 打开的 TCP socket 对应本地端口去重、排序后写入 out（逗号分隔） */
static void pid_format_tcp_ports(pid_t pid, tcp_ino_port_t *map, int map_n,
                                 char *out, size_t outsz)
{
    out[0] = '\0';
    if (map_n <= 0 || outsz < 8) return;

    uint16_t uports[96];
    int      nu = 0;

    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", (int)pid);
    DIR *fd_d = opendir(fd_dir);
    if (!fd_d) return;

    struct dirent *fde;
    while ((fde = readdir(fd_d)) != NULL && nu < 95) {
        if (fde->d_name[0] == '.') continue;
        char fp[128];
        snprintf(fp, sizeof(fp), "/proc/%d/fd/%s", (int)pid, fde->d_name);
        char lk[128];
        ssize_t lr = readlink(fp, lk, sizeof(lk) - 1);
        if (lr < 10) continue;
        lk[lr] = '\0';
        if (strncmp(lk, "socket:[", 8) != 0) continue;
        unsigned long ino = strtoul(lk + 8, NULL, 10);
        const tcp_ino_port_t *hit = lookup_ino(map, map_n, ino);
        if (!hit || hit->port <= 0 || hit->port > 65535) continue;
        uint16_t p = (uint16_t)hit->port;
        int dup = 0;
        for (int i = 0; i < nu; i++)
            if (uports[i] == p) { dup = 1; break; }
        if (!dup) uports[nu++] = p;
    }
    closedir(fd_d);

    if (nu == 0) return;
    for (int i = 1; i < nu; i++) {
        uint16_t k = uports[i];
        int j = i;
        while (j > 0 && uports[j - 1] > k) {
            uports[j] = uports[j - 1];
            j--;
        }
        uports[j] = k;
    }
    size_t pos = 0;
    for (int i = 0; i < nu && pos + 8 < outsz; i++) {
        int nw = snprintf(out + pos, outsz - pos, i ? ",%u" : "%u", (unsigned)uports[i]);
        if (nw <= 0 || (size_t)nw >= outsz - pos) break;
        pos += (size_t)nw;
    }
}

/* GET /api/port?port=<number>
 * 通过 /proc/net/tcp[6] 查找占用指定端口的进程。
 * 返回 JSON: {"port":8881,"procs":[{"pid":123,"name":"...","user":"...","state":"LISTEN",
 *   "p":1.2,"c":0},...]} ；p/c 与 /api/procs 相同方式估算 CPU%、末次调度 CPU。
 */
static void handle_api_port(int client_fd, int query_port)
{
    if (query_port <= 0 || query_port > 65535) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"invalid port\"}", 23);
        return;
    }

    static const char *tcp_states[] = {
        "","ESTABLISHED","SYN_SENT","SYN_RECV","FIN_WAIT1",
        "FIN_WAIT2","TIME_WAIT","CLOSE","CLOSE_WAIT","LAST_ACK",
        "LISTEN","CLOSING","NEW_SYN_RECV"
    };

    /* Step 1: 收集匹配端口的 socket inode */
    unsigned long inodes[256];
    int           istates[256];
    int           inode_cnt = 0;

    const char *tcp_files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int fi = 0; fi < 2; fi++) {
        FILE *f = fopen(tcp_files[fi], "r");
        if (!f) continue;
        char line[512];
        fgets(line, sizeof(line), f);   /* skip header */
        while (fgets(line, sizeof(line), f) && inode_cnt < 256) {
            char local_addr[72];
            unsigned int st = 0;
            unsigned long inode = 0;
            int n = sscanf(line, " %*d: %71s %*s %x %*s %*s %*s %*d %*d %lu",
                           local_addr, &st, &inode);
            if (n < 3) continue;
            char *colon = strchr(local_addr, ':');
            if (!colon) continue;
            unsigned int port_hex = 0;
            sscanf(colon + 1, "%x", &port_hex);
            if ((int)port_hex != query_port) continue;
            inodes[inode_cnt]  = inode;
            istates[inode_cnt] = (int)st;
            inode_cnt++;
        }
        fclose(f);
    }

    /* Step 2: 扫描 /proc/PID/fd/ 找到持有该 socket inode 的进程 */
    typedef struct { pid_t pid; int state; } port_proc_t;
    port_proc_t found[64];
    int         found_cnt = 0;

    if (inode_cnt > 0) {
        DIR *proc_dir = opendir("/proc");
        if (proc_dir) {
            struct dirent *pde;
            while ((pde = readdir(proc_dir)) != NULL && found_cnt < 64) {
                const char *nm = pde->d_name;
                if (*nm < '1' || *nm > '9') continue;
                const char *ep = nm;
                while (*ep >= '0' && *ep <= '9') ep++;
                if (*ep != '\0') continue;

                pid_t pid = (pid_t)atoi(nm);
                char fd_dir[64];
                snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", (int)pid);
                DIR *fd_d = opendir(fd_dir);
                if (!fd_d) continue;
                struct dirent *fde;
                while ((fde = readdir(fd_d)) != NULL) {
                    if (fde->d_name[0] == '.') continue;
                    char fd_path[128];
                    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/%s",
                             (int)pid, fde->d_name);
                    char link[128];
                    ssize_t lr = readlink(fd_path, link, sizeof(link) - 1);
                    if (lr < 10) continue;
                    link[lr] = '\0';
                    if (strncmp(link, "socket:[", 8) != 0) continue;
                    unsigned long sock_inode = strtoul(link + 8, NULL, 10);
                    for (int k = 0; k < inode_cnt; k++) {
                        if (inodes[k] != sock_inode) continue;
                        int dup = 0;
                        for (int j = 0; j < found_cnt; j++)
                            if (found[j].pid == pid) { dup = 1; break; }
                        if (!dup) {
                            found[found_cnt].pid   = pid;
                            found[found_cnt].state = istates[k];
                            found_cnt++;
                        }
                        break;
                    }
                }
                closedir(fd_d);
            }
            closedir(proc_dir);
        }
    }

    /* Step 3: 与 /api/procs 相同快照，估算各 PID 的 CPU%（供 monitor 进程表展示） */
    static proc_snap_t prev_snap[MAX_PROCS];
    int    prev_cnt;
    double last_dt;
    pthread_mutex_lock(&g_proc_mutex);
    prev_cnt = g_prev_proc_count;
    last_dt  = g_last_scan_dt;
    memcpy(prev_snap, g_prev_procs, sizeof(proc_snap_t) * (size_t)prev_cnt);
    pthread_mutex_unlock(&g_proc_mutex);
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double scale = (last_dt > 0.1) ? 100.0 / ((double)clk * last_dt) : 0.0;

    /* Step 4: 组装 JSON */
    strbuf_t sb = {0};
    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "{\"port\":%d,\"procs\":[", query_port);
    sb_append(&sb, hdr, (size_t)hlen);

    for (int i = 0; i < found_cnt; i++) {
        pid_t pid   = found[i].pid;
        int   state = found[i].state;

        char name[32] = "?";
        unsigned long long ticks; int cpu_idx;
        if (read_proc_stat_entry(pid, name, sizeof(name), &ticks, &cpu_idx) < 0)
            snprintf(name, sizeof(name), "?");

        char user[20] = "?";
        read_proc_user(pid, user, sizeof(user));

        const char *state_str = (state >= 1 && state <= 12) ? tcp_states[state] : "UNKNOWN";

        unsigned long long prev_ticks = 0;
        for (int j = 0; j < prev_cnt; j++)
            if (prev_snap[j].pid == pid) { prev_ticks = prev_snap[j].ticks; break; }
        float pct = (scale > 0 && ticks > prev_ticks)
                    ? (float)((double)(ticks - prev_ticks) * scale) : 0.0f;

        char entry[420];
        int en = snprintf(entry, sizeof(entry),
                          "%s{\"pid\":%d,\"name\":\"%s\",\"user\":\"%s\",\"state\":\"%s\","
                          "\"p\":%.1f,\"c\":%d,\"ports\":\"%d\"}",
                          i ? "," : "", (int)pid, name, user, state_str, pct, cpu_idx,
                          query_port);
        sb_append(&sb, entry, (size_t)en);
    }

    SB_LIT(&sb, "]}");
    send_json(client_fd, 200, "OK",
              sb.data ? sb.data : "{\"port\":0,\"procs\":[]}", sb.len);
    free(sb.data);
}

/* GET /api/procs?q=<query>&ports=1
 * 实时扫描 /proc，按进程名或用户名做大小写不敏感子串过滤，返回 JSON 数组。
 * CPU% 用上次快照 ticks 与当前 ticks 差值估算，dt 取上次 scan_proc_top 的采样间隔。
 * include_ports 非 0 时增加 "ports":"80,443"（本机 TCP 本地端口，去重排序；开销较大）。
 */
static void handle_api_procs(int client_fd, const char *query, int include_ports)
{
    char cache_key[160];
    snprintf(cache_key, sizeof(cache_key), "%d:%s", include_ports, query ? query : "");

    pthread_mutex_lock(&g_procs_json_mtx);
    if (g_procs_json &&
        difftime(time(NULL), g_procs_json_ts) < (double)PROCS_CACHE_SEC &&
        strcmp(cache_key, g_procs_cache_key) == 0) {
        send_json(client_fd, 200, "OK", g_procs_json, g_procs_json_len);
        pthread_mutex_unlock(&g_procs_json_mtx);
        return;
    }
    pthread_mutex_unlock(&g_procs_json_mtx);

    /* 复制上次快照 */
    static proc_snap_t prev_snap[MAX_PROCS];
    int    prev_cnt;
    double last_dt;
    pthread_mutex_lock(&g_proc_mutex);
    prev_cnt = g_prev_proc_count;
    last_dt  = g_last_scan_dt;
    memcpy(prev_snap, g_prev_procs, sizeof(proc_snap_t) * (size_t)prev_cnt);
    pthread_mutex_unlock(&g_proc_mutex);

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double scale = (last_dt > 0.1) ? 100.0 / ((double)clk * last_dt) : 0.0;

    tcp_ino_port_t *tcp_map = NULL;
    int               tcp_n   = 0;
    if (include_ports) {
        tcp_map = malloc(sizeof(tcp_ino_port_t) * (size_t)TCP_INODE_MAP_MAX);
        if (tcp_map) {
            tcp_n = load_tcp_ino_port_map(tcp_map, TCP_INODE_MAP_MAX);
            qsort(tcp_map, (size_t)tcp_n, sizeof(tcp_ino_port_t), cmp_tcp_ino);
        }
    }

    /* 小写化 query，用于大小写不敏感匹配 */
    char qlo[128] = "";
    if (query && *query) {
        size_t ql = strlen(query);
        if (ql >= sizeof(qlo)) ql = sizeof(qlo) - 1;
        for (size_t k = 0; k < ql; k++)
            qlo[k] = (char)tolower((unsigned char)query[k]);
        qlo[ql] = '\0';
    }

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"procs\":[");

    DIR *dir = opendir("/proc");
    if (dir) {
        int first = 1;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *nm = de->d_name;
            if (*nm < '1' || *nm > '9') continue;
            const char *ep = nm;
            while (*ep >= '0' && *ep <= '9') ep++;
            if (*ep != '\0') continue;

            pid_t pid = (pid_t)atoi(nm);
            char name[32]; unsigned long long ticks; int last_cpu;
            if (read_proc_stat_entry(pid, name, sizeof(name), &ticks, &last_cpu) < 0)
                continue;

            char user[20] = "?";
            read_proc_user(pid, user, sizeof(user));

            /* 过滤（大小写不敏感子串匹配） */
            if (qlo[0]) {
                char nlo[32], ulo[20];
                size_t nl = strlen(name), ul = strlen(user);
                for (size_t k = 0; k < nl; k++) nlo[k] = (char)tolower((unsigned char)name[k]);
                nlo[nl] = '\0';
                for (size_t k = 0; k < ul; k++) ulo[k] = (char)tolower((unsigned char)user[k]);
                ulo[ul] = '\0';
                if (!strstr(nlo, qlo) && !strstr(ulo, qlo)) continue;
            }

            /* 估算 CPU% */
            unsigned long long prev_ticks = 0;
            for (int i = 0; i < prev_cnt; i++)
                if (prev_snap[i].pid == pid) { prev_ticks = prev_snap[i].ticks; break; }
            float pct = (scale > 0 && ticks > prev_ticks)
                        ? (float)((double)(ticks - prev_ticks) * scale) : 0.0f;

            char ports_buf[192] = "";
            if (include_ports && tcp_map && tcp_n > 0)
                pid_format_tcp_ports(pid, tcp_map, tcp_n, ports_buf, sizeof(ports_buf));

            char entry[512];
            int en;
            if (include_ports)
                en = snprintf(entry, sizeof(entry),
                              "%s{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d,\"c\":%d,\"ports\":\"%s\"}",
                              first ? "" : ",", name, user, pct, (int)pid, last_cpu, ports_buf);
            else
                en = snprintf(entry, sizeof(entry),
                              "%s{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d,\"c\":%d}",
                              first ? "" : ",", name, user, pct, (int)pid, last_cpu);
            sb_append(&sb, entry, (size_t)en);
            first = 0;
        }
        closedir(dir);
    }
    SB_LIT(&sb, "]}");

    pthread_mutex_lock(&g_procs_json_mtx);
    free(g_procs_json);
    g_procs_json = (sb.data && sb.len > 0) ? strdup(sb.data) : NULL;
    g_procs_json_len = sb.len;
    g_procs_json_ts  = time(NULL);
    strncpy(g_procs_cache_key, cache_key, sizeof(g_procs_cache_key) - 1);
    g_procs_cache_key[sizeof(g_procs_cache_key) - 1] = '\0';
    pthread_mutex_unlock(&g_procs_json_mtx);

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error", "{\"procs\":[]}", 12);
    free(sb.data);
    free(tcp_map);
}

/* GET /api/monitor */
static void handle_api_monitor(int client_fd)
{
    /* 缓存检查：MONITOR_CACHE_SEC 秒内多个并发请求共用同一快照 */
    pthread_mutex_lock(&g_monitor_json_mtx);
    if (g_monitor_json &&
        difftime(time(NULL), g_monitor_json_ts) < MONITOR_CACHE_SEC) {
        send_json(client_fd, 200, "OK", g_monitor_json, g_monitor_json_len);
        pthread_mutex_unlock(&g_monitor_json_mtx);
        return;
    }
    pthread_mutex_unlock(&g_monitor_json_mtx);

    /* 序列化 update_cpu()：同一时刻只有一个线程执行，其余等待后复用缓存 */
    pthread_mutex_lock(&g_monitor_update_mtx);
    pthread_mutex_lock(&g_monitor_json_mtx);
    int cache_valid = g_monitor_json &&
                      difftime(time(NULL), g_monitor_json_ts) < MONITOR_CACHE_SEC;
    pthread_mutex_unlock(&g_monitor_json_mtx);
    if (cache_valid) {
        pthread_mutex_unlock(&g_monitor_update_mtx);
        pthread_mutex_lock(&g_monitor_json_mtx);
        if (g_monitor_json)
            send_json(client_fd, 200, "OK", g_monitor_json, g_monitor_json_len);
        pthread_mutex_unlock(&g_monitor_json_mtx);
        return;
    }
    update_cpu();

    long mem_total_kb = 0, mem_used_kb = 0;
    read_sys_mem(&mem_total_kb, &mem_used_kb);
    long proc_rss_kb = read_proc_rss();
    int  online_now  = count_online();
    long uptime_sec  = (long)difftime(time(NULL), g_start_time);
    float mem_pct    = mem_total_kb > 0
                       ? (float)mem_used_kb / (float)mem_total_kb * 100.0f : 0.0f;

    /* 快照 CPU 全局数据 */
    float  cpu_sys, cpu_proc;
    float  core_pct[MAX_CORES];
    int    core_count;
    pthread_mutex_lock(&g_cpu_mutex);
    cpu_sys    = g_cpu_sys_pct;
    cpu_proc   = g_cpu_proc_pct;
    core_count = g_cpu_core_count;
    for (int i = 0; i < core_count; i++) core_pct[i] = g_cpu_core_pct[i];
    pthread_mutex_unlock(&g_cpu_mutex);

    /* 快照每核 Top + 全局 Top */
    top_proc_t snap_core[MAX_CORES][TOP_PER_CORE];
    int        snap_core_cnt[MAX_CORES];
    top_proc_t snap_global[TOP_GLOBAL];
    int        snap_global_cnt;
    pthread_mutex_lock(&g_proc_mutex);
    for (int i = 0; i < core_count; i++) {
        snap_core_cnt[i] = g_core_top_cnt[i];
        for (int j = 0; j < g_core_top_cnt[i]; j++)
            snap_core[i][j] = g_core_top[i][j];
    }
    snap_global_cnt = g_global_top_cnt;
    for (int j = 0; j < g_global_top_cnt; j++) snap_global[j] = g_global_top[j];
    pthread_mutex_unlock(&g_proc_mutex);

    /* 构造 JSON */
    strbuf_t sb = {0};
    sb_appendf(&sb,
        "{"
        "\"sys_cpu_pct\":%.1f,"
        "\"sys_mem_total_kb\":%ld,"
        "\"sys_mem_used_kb\":%ld,"
        "\"sys_mem_pct\":%.1f,"
        "\"proc_cpu_pct\":%.1f,"
        "\"proc_mem_kb\":%ld,"
        "\"online_now\":%d,"
        "\"online_peak\":%d,"
        "\"total_requests\":%ld,"
        "\"active_conns\":%d,"
        "\"uptime_sec\":%ld,",
        cpu_sys,
        mem_total_kb, mem_used_kb, mem_pct,
        cpu_proc, proc_rss_kb,
        online_now, g_peak_online,
        g_total_reqs, g_active_conns,
        uptime_sec);

    /* "cores": 每核 pct + top3 */
    SB_LIT(&sb, "\"cores\":[");
    for (int i = 0; i < core_count; i++) {
        char hdr[32];
        int hn = snprintf(hdr, sizeof(hdr), i ? ",{\"pct\":%.1f,\"top\":["
                                              :   "{\"pct\":%.1f,\"top\":[",
                          core_pct[i]);
        sb_append(&sb, hdr, (size_t)hn);
        for (int j = 0; j < snap_core_cnt[i]; j++) {
            char entry[144];
            int en = snprintf(entry, sizeof(entry),
                j ? ",{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}"
                  :  "{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}",
                snap_core[i][j].name, snap_core[i][j].user,
                snap_core[i][j].pct,  (int)snap_core[i][j].pid);
            sb_append(&sb, entry, (size_t)en);
        }
        SB_LIT(&sb, "]}");
    }
    SB_LIT(&sb, "],");

    /* "top10": 全局 Top-10 */
    SB_LIT(&sb, "\"top10\":[");
    for (int j = 0; j < snap_global_cnt; j++) {
        char entry[144];
        int en = snprintf(entry, sizeof(entry),
            j ? ",{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}"
              :  "{\"n\":\"%s\",\"u\":\"%s\",\"p\":%.1f,\"i\":%d}",
            snap_global[j].name, snap_global[j].user,
            snap_global[j].pct,  (int)snap_global[j].pid);
        sb_append(&sb, entry, (size_t)en);
    }
    SB_LIT(&sb, "]}");

    /* 写入缓存后再发送 */
    pthread_mutex_lock(&g_monitor_json_mtx);
    free(g_monitor_json);
    g_monitor_json     = sb.data ? strdup(sb.data) : NULL;
    g_monitor_json_len = sb.len;
    g_monitor_json_ts  = time(NULL);
    pthread_mutex_unlock(&g_monitor_json_mtx);
    pthread_mutex_unlock(&g_monitor_update_mtx);

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error", "{}", 2);
    free(sb.data);
}

/* ------------------------------------------------------------------ */
/*  主处理入口                                                          */
/* ------------------------------------------------------------------ */

void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    stats_req_start(client_ip);
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

    if (total <= 0) { stats_req_end(); close(client_fd); return; }

    /* 解析请求行 */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);

    /* 高频轮询接口不写日志，避免无谓 I/O */
    int is_poll_api = (strncmp(path, "/api/monitor", 12) == 0 ||
                       strncmp(path, "/api/procs",   10) == 0 ||
                       strcmp(path, "/api/client-info") == 0);
    if (!is_poll_api)
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

        long max_body_allowed = MAX_BODY_SIZE;
        if (strcmp(path, "/api/save-report") == 0 ||
            strcmp(path, "/api/save-config") == 0)
            max_body_allowed = SAVE_REPORT_MAX_BODY;

        char *body = NULL;
        if (content_length > 0 && content_length <= max_body_allowed) {
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
        } else if (strcmp(path, "/api/ssh-exec-stream") == 0) {
            if (body) handle_api_ssh_exec_stream(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/ssh-exec-one") == 0) {
            if (body) handle_api_ssh_exec_one(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/cancel") == 0) {
            ssh_cancel_current();
            send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
        } else if (strcmp(path, "/api/kill") == 0) {
            if (body) {
                int pid = json_get_int(body, "pid", -1);
                if (pid > 1) {
                    if (kill((pid_t)pid, SIGKILL) == 0) {
                        char resp[64];
                        int rlen = snprintf(resp, sizeof(resp),
                                            "{\"ok\":true,\"pid\":%d}", pid);
                        send_json(client_fd, 200, "OK", resp, (size_t)rlen);
                    } else {
                        char resp[128];
                        int rlen = snprintf(resp, sizeof(resp),
                                            "{\"ok\":false,\"error\":\"%s\",\"pid\":%d}",
                                            strerror(errno), pid);
                        send_json(client_fd, 200, "OK", resp, (size_t)rlen);
                    }
                } else {
                    send_json(client_fd, 400, "Bad Request",
                              "{\"ok\":false,\"error\":\"invalid pid\"}", 35);
                }
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"error\":\"empty body\"}", 21);
            }
        } else if (strcmp(path, "/api/save-report") == 0) {
            if (body)
                handle_api_save_report(client_fd, req_buf, body,
                                       (size_t)content_length);
            else if (content_length > SAVE_REPORT_MAX_BODY) {
                send_json(client_fd, 413, "Payload Too Large",
                          "{\"ok\":false,\"error\":\"body too large\"}", 38);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/save-config") == 0) {
            if (body)
                handle_api_save_config(client_fd, req_buf, body,
                                       (size_t)content_length);
            else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/delete-report") == 0) {
            if (body)
                handle_api_delete_report(client_fd, body);
            else
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
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

    if (strcmp(path, "/api/monitor") == 0) {
        handle_api_monitor(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/reports") == 0) {
        handle_api_reports(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/list-ssh-configs", 21) == 0) {
        const char *rest = path + 21;
        if (*rest == '\0' || *rest == '?') {
            handle_api_list_ssh_configs(client_fd, path);
            goto done;
        }
    }

    if (strcmp(path, "/api/list-all-configs") == 0) {
        handle_api_list_all_configs(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/client-info") == 0) {
        handle_api_client_info(client_fd, client_ip);
        goto done;
    }

    if (strncmp(path, "/api/procs", 10) == 0 &&
        (path[10] == '\0' || path[10] == '?')) {
        /* 解析 ?q= 参数 */
        const char *q = "";
        const char *qs = strchr(path, '?');
        char query_buf[128] = "";
        int include_ports = 0;
        if (qs && strstr(qs, "ports=1")) include_ports = 1;
        if (qs) {
            const char *qp = strstr(qs, "q=");
            if (qp) {
                qp += 2;
                size_t qi = 0;
                while (*qp && *qp != '&' && qi < sizeof(query_buf) - 1) {
                    /* 简单 URL 解码：%xx 和 + */
                    if (*qp == '+') { query_buf[qi++] = ' '; qp++; }
                    else if (*qp == '%' && isxdigit((unsigned char)qp[1]) && isxdigit((unsigned char)qp[2])) {
                        char hex[3] = { qp[1], qp[2], '\0' };
                        query_buf[qi++] = (char)strtol(hex, NULL, 16);
                        qp += 3;
                    } else {
                        query_buf[qi++] = *qp++;
                    }
                }
                query_buf[qi] = '\0';
                q = query_buf;
            }
        }
        handle_api_procs(client_fd, q, include_ports);
        goto done;
    }

    if (strncmp(path, "/api/port", 9) == 0 &&
        (path[9] == '\0' || path[9] == '?')) {
        int port_num = 0;
        const char *qs = strchr(path, '?');
        if (qs) {
            const char *pp = strstr(qs, "port=");
            if (pp) port_num = atoi(pp + 5);
        }
        handle_api_port(client_fd, port_num);
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
    stats_req_end();
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    if (!is_poll_api)
        LOG_INFO("response %s:%d \"%s\" done in %.2fms",
                 client_ip, client_port, path, elapsed);

    close(client_fd);
}
