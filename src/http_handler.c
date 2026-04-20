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
#include <sys/wait.h>
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

/* 供 save_report 使用；完整定义见后文 */
static int dir_name_is_yyyymm(const char *name);
static int report_html_basename_ok(const char *name);
static int report_json_basename_ok(const char *name);

/* 创建 report/<YYYYMM>/（旧路径布局） */
static int mkdir_report_legacy_ym(char *dirpath, size_t dcap, const char *yyyymm)
{
    char tmp[512];

    if (!yyyymm || strlen(yyyymm) != 6)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s/report", WEB_ROOT);
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    snprintf(dirpath, dcap, "%s/report/%s", WEB_ROOT, yyyymm);
    if (mkdir(dirpath, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
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

/*  POST /api/save-report — HTML/JSON 写入 report 目录
 *  默认：服务器当月 WEB_ROOT/report/<user>/YYYYMM/
 *  可选头：X-Report-YYYYMM（六位年月）、X-Report-Legacy: 1|true（旧路径 report/YM/）、
 *          X-Report-Kind: json（显式按 JSON；否则若解码后文件名以 .json 结尾也按 JSON，避免
 *          自定义头丢失时 sanitize_report_basename 把 foo.json 改成 foo.json.html） */
static void handle_api_save_report(int client_fd, const char *req_headers,
                                   const char *body, size_t body_len)
{
    char filename[256];
    char userdir[128];
    char kind_hdr[32];
    char legacy_hdr[32];
    char ym_hdr[16];
    int  is_json   = 0;
    int  legacy_mode = 0;

    if (http_header_value(req_headers, "X-Report-Filename", filename,
                          sizeof(filename)) != 0 || filename[0] == '\0') {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Report-Filename\"}", 45);
        return;
    }

    url_decode_report_fn(filename);

    if (http_header_value(req_headers, "X-Report-Kind", kind_hdr,
                          sizeof(kind_hdr)) == 0 &&
        strcasecmp(kind_hdr, "json") == 0)
        is_json = 1;
    if (!is_json) {
        size_t fl = strlen(filename);
        if (fl >= 5 && strcasecmp(filename + fl - 5, ".json") == 0)
            is_json = 1;
    }

    if (is_json) {
        sanitize_report_archive_name(filename);
        if (filename[0] == '\0') {
            strncpy(filename, "upload.json", sizeof(filename));
            filename[sizeof(filename) - 1] = '\0';
        }
        {
            size_t len = strlen(filename);
            if (len > 200) {
                filename[200] = '\0';
                len = 200;
            }
            if (len < 6 || strcasecmp(filename + len - 5, ".json") != 0) {
                if (len + 6 < sizeof(filename))
                    memcpy(filename + len, ".json", 6);
            }
        }
        if (!report_json_basename_ok(filename)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid json filename\"}", 42);
            return;
        }
    } else {
        sanitize_report_basename(filename);
        if (!report_html_basename_ok(filename)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid html filename\"}", 42);
            return;
        }
    }

    if (http_header_value(req_headers, "X-Report-Legacy", legacy_hdr,
                          sizeof(legacy_hdr)) == 0 &&
        (legacy_hdr[0] == '1' ||
         strcasecmp(legacy_hdr, "true") == 0))
        legacy_mode = 1;

    userdir[0] = '\0';
    if (!legacy_mode) {
        if (http_header_value(req_headers, "X-Report-User", userdir,
                              sizeof(userdir)) != 0 || userdir[0] == '\0')
            strncpy(userdir, "root", sizeof(userdir));
        else
            url_decode_report_fn(userdir);
        userdir[sizeof(userdir) - 1] = '\0';
        sanitize_report_user_dir(userdir, sizeof(userdir));
    }

    char yyyymm[16];
    if (http_header_value(req_headers, "X-Report-YYYYMM", ym_hdr,
                          sizeof(ym_hdr)) == 0 && ym_hdr[0] != '\0') {
        url_decode_report_fn(ym_hdr);
        if (!dir_name_is_yyyymm(ym_hdr)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid X-Report-YYYYMM\"}", 44);
            return;
        }
        strncpy(yyyymm, ym_hdr, sizeof(yyyymm));
        yyyymm[sizeof(yyyymm) - 1] = '\0';
    } else {
        time_t     now = time(NULL);
        struct tm  tm_local;
        if (localtime_r(&now, &tm_local) == NULL) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"time\"}", 28);
            return;
        }
        if (strftime(yyyymm, sizeof(yyyymm), "%Y%m", &tm_local) == 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"strftime\"}", 32);
            return;
        }
    }

    char dirpath[512];
    char prefix[512];

    if (legacy_mode) {
        if (mkdir_report_legacy_ym(dirpath, sizeof(dirpath), yyyymm) != 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"mkdir\"}", 30);
            return;
        }
        snprintf(prefix, sizeof(prefix), "%s/report/%s/", WEB_ROOT, yyyymm);
    } else {
        if (mkdir_report_user_ym(dirpath, sizeof(dirpath), userdir, yyyymm) !=
            0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"mkdir\"}", 30);
            return;
        }
        snprintf(prefix, sizeof(prefix), "%s/report/%s/%s/", WEB_ROOT, userdir,
                 yyyymm);
    }

    char filepath[640];
    int  fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38);
        return;
    }

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
    int  rl;
    if (legacy_mode)
        rl = snprintf(resp, sizeof(resp),
                      "{\"ok\":true,\"path\":\"report/%s/%s\"}", yyyymm,
                      filename);
    else
        rl = snprintf(resp, sizeof(resp),
                      "{\"ok\":true,\"path\":\"report/%s/%s/%s\"}", userdir,
                      yyyymm, filename);
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

/* GET /api/list-register-files — 递归列出 html/register/ 下所有 .xml/.json 文件，
 * 返回相对于 register/ 的路径数组，供前端直接选择加载。                          */
static void scan_register_dir(const char *dir_path, const char *rel_prefix,
                               strbuf_t *sb, int *first)
{
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        char rel[512];
        if (rel_prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, de->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", de->d_name);
        if (S_ISDIR(st.st_mode)) {
            scan_register_dir(full, rel, sb, first);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(de->d_name);
            int ok = (nl > 4 && strcasecmp(de->d_name + nl - 4, ".xml")  == 0) ||
                     (nl > 5 && strcasecmp(de->d_name + nl - 5, ".json") == 0);
            if (!ok) continue;
            if (!*first) SB_LIT(sb, ",");
            *first = 0;
            sb_json_str(sb, rel);
        }
    }
    closedir(d);
}

static void handle_api_list_register_files(int client_fd)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"files\":[");
    int first = 1;
    scan_register_dir(WEB_ROOT "/register", "", &sb, &first);
    SB_LIT(&sb, "]}");
    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"files\":[]}", 22);
}

/* GET /api/list-register-dirs — 递归列出 html/register/ 下所有子目录路径 */
static void scan_register_subdirs(const char *dir_path, const char *rel_prefix,
                                  strbuf_t *sb, int *first)
{
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char rel[512];
        if (rel_prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, de->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", de->d_name);
        if (!*first) SB_LIT(sb, ",");
        *first = 0;
        sb_json_str(sb, rel);
        scan_register_subdirs(full, rel, sb, first);
    }
    closedir(d);
}

static void handle_api_list_register_dirs(int client_fd)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"dirs\":[");
    int first = 1;
    scan_register_subdirs(WEB_ROOT "/register", "", &sb, &first);
    SB_LIT(&sb, "]}");
    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"dirs\":[]}", 21);
}

/* 工具：递归创建目录 */
static int mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

/* 工具：验证子目录路径安全（无 .. / 绝对路径） */
static int register_subdir_safe(const char *s)
{
    if (!s || !*s) return 1;           /* 空串 = 根目录，允许 */
    if (s[0] == '/' || s[0] == '.') return 0;
    const char *p = s;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 0) return 0;
        if (clen == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (clen == 1 && p[0] == '.') return 0;
        p = slash ? slash + 1 : p + clen;
    }
    return 1;
}

/* 工具：验证文件名安全（无路径分隔符，必须以 .json/.xml 结尾） */
static int register_filename_safe(const char *fn)
{
    if (!fn || !*fn || fn[0] == '.') return 0;
    if (strchr(fn, '/') || strchr(fn, '\\') || strchr(fn, ':')) return 0;
    size_t l = strlen(fn);
    return (l > 5 && strcasecmp(fn + l - 5, ".json") == 0) ||
           (l > 4 && strcasecmp(fn + l - 4, ".xml")  == 0);
}

/* POST /api/save-register-file
 * Headers: X-Register-Subdir (可为空，表示根目录), X-Register-Filename
 * Body:    文件内容（JSON/XML）
 * 写入 html/register/<subdir>/<filename>                                 */
static void handle_api_save_register_file(int client_fd, const char *req_headers,
                                          const char *body, size_t body_len)
{
    char subdir[256]   = "";
    char filename[256] = "";

    /* 子目录（可选） */
    http_header_value(req_headers, "X-Register-Subdir", subdir, sizeof(subdir));
    url_decode_report_fn(subdir);

    /* 文件名（必填） */
    if (http_header_value(req_headers, "X-Register-Filename",
                          filename, sizeof(filename)) != 0 || !filename[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Register-Filename\"}", 51);
        return;
    }
    url_decode_report_fn(filename);

    if (!register_subdir_safe(subdir)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid subdir\"}", 38);
        return;
    }
    if (!register_filename_safe(filename)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid filename\"}", 40);
        return;
    }

    /* 构建目录路径并创建 */
    char dirpath[1024];
    if (subdir[0])
        snprintf(dirpath, sizeof(dirpath), WEB_ROOT "/register/%s", subdir);
    else
        snprintf(dirpath, sizeof(dirpath), WEB_ROOT "/register");

    if (mkdir_p(dirpath) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir\"}", 30);
        return;
    }

    /* 构建文件路径并校验前缀防路径穿越 */
    char filepath[1280];
    int fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38);
        return;
    }
    char prefix[512];
    snprintf(prefix, sizeof(prefix), WEB_ROOT "/register/");
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
    if (body_len > 0 && fwrite(body, 1, body_len, fp) != body_len) {
        fclose(fp); unlink(filepath);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write\"}", 30);
        return;
    }
    fclose(fp);

    /* 返回保存的相对路径 */
    char resp[640];
    const char *relpath = filepath + strlen(WEB_ROOT) + 1; /* skip "html/" */
    int rl = snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\"}", relpath);
    send_json(client_fd, 200, "OK", resp, (size_t)(rl > 0 ? rl : 11));
    LOG_INFO("save_register  %s  (%zu bytes)", filepath, body_len);
}

/* 工具：验证 register 下的相对路径安全（subdir/filename 均合法） */
static int register_relpath_safe(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '.') return 0;
    const char *slash = strrchr(p, '/');
    const char *fname = slash ? slash + 1 : p;
    if (!register_filename_safe(fname)) return 0;
    if (slash) {
        char dir[512];
        size_t dlen = (size_t)(slash - p);
        if (dlen >= sizeof(dir)) return 0;
        memcpy(dir, p, dlen);
        dir[dlen] = '\0';
        return register_subdir_safe(dir);
    }
    return 1;
}

/* 工具：递归删除目录 */
static int rmdir_r(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path) == 0 ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir_r(sub);
        else unlink(sub);
    }
    closedir(d);
    return rmdir(path);
}

/* POST /api/rename-register-dir
 * Body: {"from":"olddir","to":"newdir"}，路径相对于 html/register/          */
static void handle_api_rename_register_dir(int client_fd, const char *body)
{
    char from_rel[512] = "", to_rel[512] = "";
    if (json_get_str(body, "from", from_rel, sizeof(from_rel)) < 0 || !from_rel[0] ||
        json_get_str(body, "to",   to_rel,   sizeof(to_rel))   < 0 || !to_rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing from/to\"}", 40);
        return;
    }
    if (!register_subdir_safe(from_rel) || !register_subdir_safe(to_rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37);
        return;
    }
    char from_full[1024], to_full[1024];
    snprintf(from_full, sizeof(from_full), WEB_ROOT "/register/%s", from_rel);
    snprintf(to_full,   sizeof(to_full),   WEB_ROOT "/register/%s", to_rel);
    if (rename(from_full, to_full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("rename_register_dir  %s -> %s", from_full, to_full);
}

/* POST /api/delete-register-dir
 * Body: {"path":"dirname"}，路径相对于 html/register/，递归删除              */
static void handle_api_delete_register_dir(int client_fd, const char *body)
{
    char rel[512] = "";
    if (json_get_str(body, "path", rel, sizeof(rel)) < 0 || !rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing path\"}", 37);
        return;
    }
    if (!register_subdir_safe(rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37);
        return;
    }
    char full[1024];
    snprintf(full, sizeof(full), WEB_ROOT "/register/%s", rel);
    if (rmdir_r(full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("delete_register_dir  %s", full);
}

/* POST /api/rename-register-file
 * Body: {"from":"old/path.xml","to":"new/path.xml"}
 * 两个路径均相对于 html/register/                                           */
static void handle_api_rename_register_file(int client_fd,
                                            const char *body)
{
    char from_rel[512] = "", to_rel[512] = "";
    if (json_get_str(body, "from", from_rel, sizeof(from_rel)) < 0 || !from_rel[0] ||
        json_get_str(body, "to",   to_rel,   sizeof(to_rel))   < 0 || !to_rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing from/to\"}", 40);
        return;
    }
    if (!register_relpath_safe(from_rel) || !register_relpath_safe(to_rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37);
        return;
    }
    char from_full[1024], to_full[1024];
    snprintf(from_full, sizeof(from_full), WEB_ROOT "/register/%s", from_rel);
    snprintf(to_full,   sizeof(to_full),   WEB_ROOT "/register/%s", to_rel);

    /* 如目标目录不存在则创建 */
    char to_dir[1024];
    snprintf(to_dir, sizeof(to_dir), "%s", to_full);
    char *slash = strrchr(to_dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(to_dir); }

    if (rename(from_full, to_full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("rename_register  %s -> %s", from_full, to_full);
}

/* POST /api/delete-register-file
 * Body: {"path":"subfolder/file.xml"}
 * 路径相对于 html/register/                                                  */
static void handle_api_delete_register_file(int client_fd, const char *body)
{
    char rel[512] = "";
    if (json_get_str(body, "path", rel, sizeof(rel)) < 0 || !rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing path\"}", 37);
        return;
    }
    if (!register_relpath_safe(rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37);
        return;
    }
    char full[1024];
    snprintf(full, sizeof(full), WEB_ROOT "/register/%s", rel);
    if (unlink(full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("delete_register  %s", full);
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

/* ================================================================== */
/*  Wiki API                                                           */
/* ================================================================== */

#define WIKI_ROOT    WEB_ROOT "/wiki"
#define WIKI_MD_DB   WIKI_ROOT "/md_db"
#define WIKI_UPLOADS WIKI_ROOT "/uploads"

/* 动态提取 JSON 字符串字段（调用方 free） */
static char *json_get_str_alloc(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    size_t len = 0;
    const char *q = p;
    while (*q && *q != '"') { if (*q == '\\') { q++; if (!*q) break; } len++; q++; }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++; if (!*p) break;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *p;   break;
            }
        } else { out[i++] = *p; }
        p++;
    }
    out[i] = '\0';
    return out;
}

static void wiki_gen_id(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    unsigned long tid = (unsigned long)pthread_self() & 0xFFFF;
    snprintf(buf, sz, "note_%04d%02d%02d_%02d%02d%02d_%04lx",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, tid);
}

static void wiki_now_iso(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ── 前向声明（helper 中调用定义在后面的函数） ─────────────────────── */
static void wiki_rewrite_html(const char *id, const char *title,
                               const char *cat, const char *updated);

/* ── md_db 路径 helper ────────────────────────────────────────────── */

/* 在目录树中递归查找文件名为 target 的文件 */
static int wiki_md_find_r(char *buf, size_t sz, const char *dir, const char *target)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d)) != NULL && found < 0) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            found = wiki_md_find_r(buf, sz, child, target);
        else if (strcmp(de->d_name, target) == 0)
            { snprintf(buf, sz, "%s", child); found = 0; }
    }
    closedir(d);
    return found;
}

/* 在 WIKI_MD_DB 中找到 <id>.md，填充 buf，找不到返回 -1 */
static int wiki_md_find(char *buf, size_t sz, const char *id)
{
    char target[256]; snprintf(target, sizeof(target), "%s.md", id);
    return wiki_md_find_r(buf, sz, WIKI_MD_DB, target);
}

/* 按分类生成写入路径（自动建分类目录） */
static void wiki_md_write_path(char *buf, size_t sz, const char *id, const char *cat)
{
    if (cat && cat[0]) {
        char catdir[768]; snprintf(catdir, sizeof(catdir), "%s/%s", WIKI_MD_DB, cat);
        mkdir_p(catdir);
        snprintf(buf, sz, "%s/%s/%s.md", WIKI_MD_DB, cat, id);
    } else {
        snprintf(buf, sz, "%s/%s.md", WIKI_MD_DB, id);
    }
}

/* 递归扫描目录，将找到的 .md 文件追加到 JSON 数组 sb */
static void wiki_scan_md_dir(strbuf_t *sb, int *pfirst, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_scan_md_dir(sb, pfirst, child); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char line[4096] = {0}; int ok = (fgets(line, sizeof(line), fp) != NULL); fclose(fp);
        if (!ok || strncmp(line, "<!--META ", 9) != 0) continue;
        char *end = strstr(line, "-->"); if (!end) continue; *end = '\0';
        const char *mj = line + 9;
        char id[128]={0},title[512]={0},cat[512]={0},cre[64]={0},upd[64]={0};
        json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
        json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"created",cre,sizeof(cre));
        json_get_str(mj,"updated",upd,sizeof(upd));
        if (!id[0]) continue;
        if (!*pfirst) SB_LIT(sb, ","); *pfirst = 0;
        SB_LIT(sb,"{\"id\":"); sb_json_str(sb,id);
        SB_LIT(sb,",\"title\":"); sb_json_str(sb,title);
        SB_LIT(sb,",\"category\":"); sb_json_str(sb,cat);
        SB_LIT(sb,",\"created\":"); sb_json_str(sb,cre);
        SB_LIT(sb,",\"updated\":"); sb_json_str(sb,upd);
        SB_LIT(sb,"}");
    }
    closedir(d);
}

/* 递归扫描目录，重建所有 .md 对应的 html 文件 */
static void wiki_rebuild_md_dir(int *pcount, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_rebuild_md_dir(pcount, child); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char ml[4096] = {0}; fgets(ml, sizeof(ml), fp); fclose(fp);
        if (strncmp(ml,"<!--META ",9)!=0) continue;
        char *mend = strstr(ml,"-->"); if (!mend) continue; *mend='\0';
        const char *mj = ml + 9;
        char id[128]={0},title[512]={0},cat[512]={0},upd[64]={0};
        json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
        json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"updated",upd,sizeof(upd));
        if (!id[0]) continue;
        wiki_rewrite_html(id, title, cat[0] ? cat : NULL, upd);
        (*pcount)++;
    }
    closedir(d);
}

/* 递归更新目录树中 .md 文件的 category（old_path → new_path）并重写 html */
static void wiki_update_cat_in_dir(const char *dir,
                                    const char *old_path, const char *new_path,
                                    size_t old_len)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            wiki_update_cat_in_dir(child, old_path, new_path, old_len); continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char ml[4096]={0}; fgets(ml, sizeof(ml), fp);
        strbuf_t cbuf={0}; char tbuf[8192]; size_t nr;
        while ((nr=fread(tbuf,1,sizeof(tbuf),fp))>0) sb_append(&cbuf,tbuf,nr);
        fclose(fp);
        if (strncmp(ml,"<!--META ",9)!=0) { free(cbuf.data); continue; }
        char *end=strstr(ml,"-->"); if (!end) { free(cbuf.data); continue; }
        *end='\0'; const char *mj=ml+9;
        char aid[128]={0},atitle[512]={0},acat[512]={0},acre[64]={0},aupd[64]={0};
        json_get_str(mj,"id",aid,sizeof(aid)); json_get_str(mj,"title",atitle,sizeof(atitle));
        json_get_str(mj,"category",acat,sizeof(acat)); json_get_str(mj,"created",acre,sizeof(acre));
        json_get_str(mj,"updated",aupd,sizeof(aupd));
        char new_cat[512]={0};
        if (strcmp(acat,old_path)==0)
            snprintf(new_cat,sizeof(new_cat),"%s",new_path);
        else if (strncmp(acat,old_path,old_len)==0 && acat[old_len]=='/')
            snprintf(new_cat,sizeof(new_cat),"%s%s",new_path,acat+old_len);
        else { free(cbuf.data); continue; }
        fp = fopen(child,"wb"); if (!fp) { free(cbuf.data); continue; }
        strbuf_t mb={0};
        SB_LIT(&mb,"<!--META "); SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,aid);
        SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,atitle);
        SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,new_cat);
        SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,acre);
        SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,aupd);
        SB_LIT(&mb,"}-->\n");
        if (mb.data) fwrite(mb.data,1,mb.len,fp);
        if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
        fclose(fp); free(mb.data); free(cbuf.data);
        wiki_rewrite_html(aid, atitle, new_cat, aupd);
    }
    closedir(d);
}

/* 判断 md_db 某子目录下是否存在 .md 文件（递归） */
static int wiki_md_dir_has_md(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = wiki_md_dir_has_md(child);
        else { size_t nl=strlen(de->d_name);
               if (nl>=4 && strcmp(de->d_name+nl-3,".md")==0) found=1; }
    }
    closedir(d);
    return found;
}

static int wiki_upload_safe(const char *fn)
{
    if (!fn || !fn[0] || fn[0] == '.') return 0;
    if (strchr(fn, '/') || strchr(fn, '\\') || strchr(fn, ':')) return 0;
    const char *dot = strrchr(fn, '.');
    if (!dot) return 0;
    static const char *exts[] = {
        ".jpg",".jpeg",".png",".gif",".svg",".webp",
        ".pdf",".txt",".md",".zip",".tar",".gz",NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static void wiki_ensure_dirs(void)
{
    mkdir_p(WIKI_MD_DB);
    mkdir_p(WIKI_UPLOADS);
}

/* 写 HTML 实体编码 */
static void wiki_fhtml(FILE *fp, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
            case '&': fputs("&amp;", fp); break;
            case '<': fputs("&lt;",  fp); break;
            case '>': fputs("&gt;",  fp); break;
            case '"': fputs("&quot;",fp); break;
            default:  fputc(*s, fp);      break;
        }
    }
}

/* JS 字符串转义：将 s 作为双引号包围的 JS 字面量写入 fp */
static void wiki_fjs(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; s && *s; s++) {
        if      (*s == '\\') fputs("\\\\", fp);
        else if (*s == '"')  fputs("\\\"", fp);
        else if (*s == '\n') fputs("\\n",  fp);
        else if (*s == '\r') {}
        else                 fputc(*s, fp);
    }
    fputc('"', fp);
}

/* 写渲染后的独立 HTML 文件（含左侧目录导航侧栏） */
static int wiki_write_html_file(const char *filepath,
    const char *id, const char *title, const char *category,
    const char *updated, const char *html_body)
{
    FILE *fp = fopen(filepath, "wb");
    if (!fp) return -1;

    /* ── <head> ── */
    fputs("<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
          "<meta charset=\"UTF-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
          "<title>", fp);
    wiki_fhtml(fp, title);
    fputs(" - NoteWiki</title>\n<style>\n"
          "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}\n"
          "html,body{height:100%;overflow:hidden}\n"
          "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
               "background:#0d1117;color:#c9d1d9;line-height:1.7;"
               "display:flex;flex-direction:column}\n"
          "nav.topbar{background:#161b22;border-bottom:1px solid #30363d;"
                    "padding:8px 20px;font-size:.85rem;flex-shrink:0}\n"
          "nav.topbar a{color:#4a90e2;text-decoration:none}\n"
          ".layout{display:flex;flex:1;overflow:hidden}\n"
          ".sidebar{width:220px;background:#161b22;border-right:1px solid #30363d;"
                  "overflow:hidden;flex-shrink:0;transition:width .2s ease;"
                  "display:flex;flex-direction:column}\n"
          ".sidebar.collapsed{width:28px}\n"
          ".sidebar-body{overflow-y:auto;flex:1}\n"
          ".sidebar.collapsed .sidebar-body{display:none}\n"
          ".content{flex:1;overflow-y:auto;padding:32px 40px}\n"
          "article{width:100%}\n"
          ".st-top{font-size:.78rem;color:#8b949e;padding:6px 8px 6px 12px;"
                 "border-bottom:1px solid #21262d;font-weight:600;"
                 "letter-spacing:.04em;flex-shrink:0;"
                 "display:flex;align-items:center;justify-content:space-between}\n"
          ".sidebar.collapsed .st-top{justify-content:center;padding:6px 0}\n"
          ".sidebar.collapsed .st-top .panel-label{display:none}\n"
          ".st-cat{font-size:.8rem;font-weight:600;color:#8b949e;"
                 "padding:5px 12px 2px;letter-spacing:.04em;"
                 "user-select:none;white-space:nowrap;overflow:hidden;"
                 "text-overflow:ellipsis;cursor:pointer;display:flex;align-items:center;gap:4px}\n"
          ".st-cat:hover{color:#f0f6fc}\n"
          ".cat-arrow{font-size:.65rem;flex-shrink:0;width:10px;text-align:center}\n"
          ".cat-body{overflow:hidden}\n"
          ".st-art{font-size:.83rem;color:#c9d1d9;padding:3px 12px;"
                 "text-decoration:none;display:block;white-space:nowrap;"
                 "overflow:hidden;text-overflow:ellipsis}\n"
          ".st-art:hover{background:#21262d;color:#f0f6fc}\n"
          ".st-art.active{background:#1a3a5c;color:#7ab8ff;font-weight:500}\n"
          ".toc{width:200px;background:#161b22;border-left:1px solid #30363d;"
              "overflow:hidden;flex-shrink:0;transition:width .2s ease;"
              "display:flex;flex-direction:column}\n"
          ".toc.collapsed{width:28px}\n"
          ".toc-body{overflow-y:auto;flex:1}\n"
          ".toc.collapsed .toc-body{display:none}\n"
          ".toc-top{font-size:.78rem;color:#8b949e;padding:6px 4px 6px 12px;"
                  "border-bottom:1px solid #21262d;font-weight:600;"
                  "letter-spacing:.04em;flex-shrink:0;"
                  "display:flex;align-items:center;justify-content:space-between}\n"
          ".toc.collapsed .toc-top{justify-content:center;padding:6px 0}\n"
          ".toc.collapsed .toc-top .panel-label{display:none}\n"
          ".toc-node{}\n"
          ".toc-row{display:flex;align-items:center;gap:2px;cursor:pointer}\n"
          ".toc-tog{font-size:.6rem;color:#555;cursor:pointer;width:14px;text-align:center;"
                  "flex-shrink:0;user-select:none;padding:2px 0}\n"
          ".toc-tog:hover{color:#f0f6fc}\n"
          ".toc-tog-sp{width:14px;flex-shrink:0}\n"
          ".toc-children{overflow:hidden}\n"
          ".toc-item{font-size:.8rem;color:#8b949e;padding:3px 4px 3px 0;"
                   "text-decoration:none;display:block;white-space:nowrap;"
                   "overflow:hidden;text-overflow:ellipsis;flex:1;min-width:0}\n"
          ".toc-item:hover{color:#f0f6fc}\n"
          ".toc-row:hover{background:#21262d;border-radius:3px}\n"
          ".toc-item.active{color:#7ab8ff;font-weight:500}\n"
          ".panel-toggle{background:none;border:none;cursor:pointer;"
                        "color:#8b949e;font-size:.8rem;padding:2px 5px;"
                        "border-radius:3px;flex-shrink:0;line-height:1}\n"
          ".panel-toggle:hover{background:#21262d;color:#f0f6fc}\n"
          ".edit-btn{font-size:.78rem;color:#4a90e2;text-decoration:none;"
                   "padding:3px 10px;border:1px solid #2d3a54;border-radius:5px;"
                   "margin-left:auto;white-space:nowrap}\n"
          ".edit-btn:hover{background:#1a3a5c;border-color:#4a90e2}\n"
          ".copy-btn{font-size:.78rem;color:#8b949e;background:none;"
                   "padding:3px 10px;border:1px solid #30363d;border-radius:5px;"
                   "cursor:pointer;white-space:nowrap;margin-left:6px}\n"
          ".copy-btn:hover{background:#21262d;color:#f0f6fc;border-color:#8b949e}\n"
          "h1.at{font-size:1.75rem;color:#f0f6fc;margin-bottom:6px}\n"
          ".am{font-size:.75rem;color:#8b949e;padding-bottom:14px;"
              "border-bottom:1px solid #30363d;margin-bottom:24px}\n"
          ".ab h1,.ab h2,.ab h3,.ab h4{color:#f0f6fc;margin:1.3em 0 .5em}\n"
          ".ab p{margin:.7em 0}\n"
          ".ab pre{background:#161b22;border:1px solid #30363d;border-radius:6px;"
                  "padding:12px;overflow-x:auto;margin:1em 0}\n"
          ".ab code{font-family:monospace;font-size:.88em}\n"
          ".ab pre code{color:#c9d1d9}\n"
          ".ab :not(pre)>code{background:#1e2740;padding:1px 5px;"
                             "border-radius:3px;color:#7ab8ff}\n"
          ".ab blockquote{border-left:3px solid #4a90e2;padding:.3em 1em;"
                         "color:#8b949e;margin:1em 0}\n"
          ".ab table{border-collapse:collapse;width:100%;margin:1em 0}\n"
          ".ab th,.ab td{border:1px solid #30363d;padding:6px 10px;text-align:left}\n"
          ".ab th{background:#161b22;font-weight:600;color:#f0f6fc}\n"
          ".ab img{max-width:100%;border-radius:4px}\n"
          ".ab ul,.ab ol{padding-left:1.5em;margin:.5em 0}\n"
          ".ab a{color:#4a90e2}\n"
          ".ab hr{border:none;border-top:1px solid #30363d;margin:1.2em 0}\n"
          "</style>\n</head>\n<body>\n", fp);

    /* ── 顶部导航栏 ── */
    fputs("<nav class=\"topbar\">"
          "<a href=\"/wiki/notewiki.html\">← NoteWiki</a>", fp);
    if (category && category[0]) {
        fputs(" / ", fp); wiki_fhtml(fp, category);
    }
    fputs(" <a class=\"edit-btn\" href=\"/wiki/notewiki.html?edit=", fp);
    fputs(id, fp);
    fputs("\">✏ 编辑</a>"
          "<button class=\"copy-btn\" id=\"copy-html-btn\" onclick=\"copyHtml()\">复制 HTML</button>"
          "<button class=\"copy-btn\" id=\"copy-md-btn\" onclick=\"copyMd()\">复制 MD</button>"
          "</nav>\n", fp);

    /* ── 两栏布局 ── */
    fputs("<div class=\"layout\">\n"
          "<nav class=\"sidebar\" id=\"sidebar\">"
          "<div class=\"st-top\">"
          "<span class=\"panel-label\">文章目录</span>"
          "<button class=\"panel-toggle\" id=\"sb-toggle\">&#9664;</button>"
          "</div>"
          "<div class=\"sidebar-body\" id=\"sidebar-body\"></div>"
          "</nav>\n"
          "<div class=\"content\">\n"
          "<article>\n<h1 class=\"at\">", fp);
    wiki_fhtml(fp, title);
    fputs("</h1>\n<div class=\"am\">更新：", fp);
    fputs(updated, fp);
    fputs("</div>\n<div class=\"ab\" id=\"article-body\">\n", fp);
    if (html_body) fputs(html_body, fp);
    fputs("\n</div>\n</article>\n</div>\n"
          "<nav class=\"toc\" id=\"toc\">"
          "<div class=\"toc-top\">"
          "<button class=\"panel-toggle\" id=\"toc-toggle\">&#9654;</button>"
          "<span class=\"panel-label\">本文目录</span>"
          "</div>"
          "<div class=\"toc-body\" id=\"toc-body\"></div>"
          "</nav>\n"
          "</div>\n", fp);

    /* ── WIKI_CUR_ID 必须在复制脚本之前赋值 ── */
    fputs("<script>window.WIKI_CUR_ID=", fp);
    wiki_fjs(fp, id ? id : "");
    fputs(";</script>\n", fp);

    /* ── 复制功能脚本 ── */
    fputs("<script>\n"
          "/* toast：动态创建，不依赖 HTML 中预置的 div */\n"
          "function showToast(msg){\n"
          "  var t=document.getElementById('_wk_toast');\n"
          "  if(!t){\n"
          "    t=document.createElement('div');\n"
          "    t.id='_wk_toast';\n"
          "    t.style.cssText='position:fixed;bottom:28px;right:28px;z-index:99999;'\n"
          "      +'background:#238636;color:#fff;padding:10px 22px;'\n"
          "      +'border-radius:8px;font-size:14px;font-weight:500;'\n"
          "      +'box-shadow:0 4px 16px rgba(0,0,0,.6);display:none;pointer-events:none';\n"
          "    document.body.appendChild(t);\n"
          "  }\n"
          "  t.textContent=msg;\n"
          "  t.style.display='block';\n"
          "  clearTimeout(t._tid);\n"
          "  t._tid=setTimeout(function(){t.style.display='none';},2500);\n"
          "}\n"
          "/* execCommand 兜底（支持 HTTP） */\n"
          "function execCopy(text){\n"
          "  var ta=document.createElement('textarea');\n"
          "  ta.value=text;\n"
          "  ta.style.cssText='position:fixed;top:0;left:0;width:1px;height:1px;opacity:0';\n"
          "  document.body.appendChild(ta);\n"
          "  ta.focus();ta.select();ta.setSelectionRange(0,ta.value.length);\n"
          "  var ok=false;try{ok=document.execCommand('copy');}catch(e){}\n"
          "  document.body.removeChild(ta);\n"
          "  return ok;\n"
          "}\n"
          "function copyText(text,msg){\n"
          "  if(navigator.clipboard&&window.isSecureContext){\n"
          "    navigator.clipboard.writeText(text)\n"
          "      .then(function(){showToast(msg);})\n"
          "      .catch(function(){if(execCopy(text))showToast(msg);});\n"
          "  } else {\n"
          "    if(execCopy(text))showToast(msg);\n"
          "  }\n"
          "}\n"
          "function copyHtml(){\n"
          "  copyText(document.getElementById('article-body').innerHTML,'✓ HTML 已复制到剪贴板');\n"
          "}\n"
          "/* 页面加载时预取 MD 内容，按钮点击时直接在手势上下文内复制 */\n"
          "var _mdCache=null;\n"
          "fetch('/api/wiki-read?id='+window.WIKI_CUR_ID)\n"
          "  .then(function(r){return r.json();})\n"
          "  .then(function(d){if(d.ok)_mdCache=d.content;})\n"
          "  .catch(function(){});\n"
          "function copyMd(){\n"
          "  if(_mdCache!==null){copyText(_mdCache,'✓ Markdown 已复制到剪贴板');return;}\n"
          "  showToast('内容加载中，请稍后再试…');\n"
          "}\n"
          "</script>\n", fp);

    /* ── 侧栏渲染脚本（外链，避免 C 字符串转义问题） ── */
    fputs("<script src=\"/wiki/sidebar.js\"></script>\n"
          "</body>\n</html>\n", fp);

    fclose(fp);
    return 0;
}

/* 递归收集 WIKI_ROOT 内用户分类目录路径，写入 JSON 字符串数组
   top=1 时跳过系统目录（md_db / uploads）*/
static void wiki_collect_dirs(strbuf_t *sb, const char *base,
                               const char *rel, int *pfirst, int top)
{
    char full[1024];
    if (rel[0])
        snprintf(full, sizeof(full), "%s/%s", base, rel);
    else
        snprintf(full, sizeof(full), "%s", base);

    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        /* 顶层跳过系统目录 */
        if (top && (strcmp(de->d_name,"md_db")==0 ||
                    strcmp(de->d_name,"uploads")==0)) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char relpath[512];
        if (rel[0])
            snprintf(relpath, sizeof(relpath), "%s/%s", rel, de->d_name);
        else
            snprintf(relpath, sizeof(relpath), "%s", de->d_name);
        if (!*pfirst) SB_LIT(sb, ",");
        *pfirst = 0;
        sb_json_str(sb, relpath);
        wiki_collect_dirs(sb, base, relpath, pfirst, 0);
    }
    closedir(d);
}

/* GET /api/wiki-list */
static void handle_api_wiki_list(int client_fd)
{
    wiki_ensure_dirs();
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"articles\":[");
    int first = 1;
    wiki_scan_md_dir(&sb, &first, WIKI_MD_DB);
    /* 附加用户分类目录列表（扫 WIKI_ROOT，排除系统目录） */
    SB_LIT(&sb, "],\"categories\":[");
    int firstcat = 1;
    wiki_collect_dirs(&sb, WIKI_ROOT, "", &firstcat, 1);
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"articles\":[],\"categories\":[]}", 30);
    free(sb.data);
}

/* GET /api/wiki-read?id=xxx */
static void handle_api_wiki_read(int client_fd, const char *path_qs)
{
    char id[128] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "id=");
        if (p) { p += 3; size_t j=0; while(*p&&*p!='&'&&j<sizeof(id)-1) id[j++]=*p++; }
    }
    if (!id[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",33); return; }
    for (size_t i=0;id[i];i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    char path[768];
    if (wiki_md_find(path, sizeof(path), id) < 0) {
        send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) { send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return; }
    char line[4096]; fgets(line, sizeof(line), fp); /* skip META */
    strbuf_t body = {0};
    char buf[8192]; size_t nr;
    while ((nr = fread(buf,1,sizeof(buf),fp)) > 0) sb_append(&body,buf,nr);
    fclose(fp);
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"content\":");
    sb_json_str(&sb, body.data ? body.data : "");
    SB_LIT(&sb, "}");
    free(body.data);
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,500,"Internal Server Error","{\"ok\":false}",12);
    free(sb.data);
}

/* POST /api/wiki-save */
static void handle_api_wiki_save(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, title[512]={0}, cat[512]={0}, created[64]={0};
    json_get_str(body,"id",id,sizeof(id));
    json_get_str(body,"title",title,sizeof(title));
    json_get_str(body,"category",cat,sizeof(cat));
    json_get_str(body,"created",created,sizeof(created));
    if (!title[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing title\"}",38); return;
    }
    if (!register_subdir_safe(cat)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid category\"}",41); return;
    }
    /* validate existing id */
    if (id[0]) {
        for (size_t i=0;id[i];i++) {
            unsigned char c=(unsigned char)id[i];
            if (!isalnum(c)&&c!='_'&&c!='-') {
                send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
            }
        }
    } else {
        wiki_gen_id(id, sizeof(id));
    }
    /* 新建文章且指定了自定义 id 时，检测文件名重复（force=true 时跳过） */
    char force_str[8]={0}; json_get_str(body,"force",force_str,sizeof(force_str));
    int force = (strcmp(force_str,"true")==0);
    if (!force && id[0] && !created[0]) {
        char chk[768];
        if (wiki_md_find(chk, sizeof(chk), id) == 0) {
            send_json(client_fd, 409, "Conflict",
                      "{\"ok\":false,\"error\":\"duplicate\"}", 31); return;
        }
    }
    char now[64]; wiki_now_iso(now, sizeof(now));
    if (!created[0]) strncpy(created, now, sizeof(created)-1);

    char *content = json_get_str_alloc(body, "content");
    char *html    = json_get_str_alloc(body, "html");
    if (!content || !html) {
        free(content); free(html);
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing content/html\"}",45); return;
    }

    /* 找旧文件（如有），用于检测分类是否变更 */
    char old_md_path[768] = {0};
    char old_cat[512] = {0};
    if (wiki_md_find(old_md_path, sizeof(old_md_path), id) == 0) {
        FILE *oldfp = fopen(old_md_path, "r");
        if (oldfp) {
            char oml[4096]={0}; fgets(oml, sizeof(oml), oldfp); fclose(oldfp);
            if (strncmp(oml,"<!--META ",9)==0) {
                char *oend=strstr(oml,"-->"); if (oend) { *oend='\0'; json_get_str(oml+9,"category",old_cat,sizeof(old_cat)); }
            }
        }
    }

    /* write .md to categorized path */
    char md_path[768];
    wiki_md_write_path(md_path, sizeof(md_path), id, cat);
    FILE *fp = fopen(md_path, "wb");
    if (!fp) { free(content); free(html);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"write md\"}",33); return; }
    /* META line */
    strbuf_t meta = {0};
    SB_LIT(&meta, "<!--META ");
    SB_LIT(&meta,"{\"id\":"); sb_json_str(&meta,id);
    SB_LIT(&meta,",\"title\":"); sb_json_str(&meta,title);
    SB_LIT(&meta,",\"category\":"); sb_json_str(&meta,cat);
    SB_LIT(&meta,",\"created\":"); sb_json_str(&meta,created);
    SB_LIT(&meta,",\"updated\":"); sb_json_str(&meta,now);
    SB_LIT(&meta, "}-->\n");
    if (meta.data) fwrite(meta.data,1,meta.len,fp);
    fwrite(content,1,strlen(content),fp);
    fclose(fp);
    free(meta.data); free(content);

    /* 若分类变更，删除旧 .md（新路径已写入） */
    if (old_md_path[0] && strcmp(old_md_path, md_path) != 0)
        unlink(old_md_path);

    /* 若分类变更，删除旧 .html */
    if (old_cat[0] && strcmp(old_cat, cat) != 0) {
        char old_html[1024];
        snprintf(old_html, sizeof(old_html), "%s/%s/%s.html", WIKI_ROOT, old_cat, id);
        unlink(old_html);
    }

    /* write .html to category dir */
    char html_path[1024];
    if (cat[0]) {
        char cat_dir[768];
        snprintf(cat_dir, sizeof(cat_dir), "%s/%s", WIKI_ROOT, cat);
        mkdir_p(cat_dir);
        snprintf(html_path, sizeof(html_path), "%s/%s/%s.html", WIKI_ROOT, cat, id);
    } else {
        snprintf(html_path, sizeof(html_path), "%s/%s.html", WIKI_ROOT, id);
    }
    wiki_write_html_file(html_path, id, title, cat, now, html);
    free(html);

    /* build URL */
    const char *rel = html_path + strlen(WEB_ROOT) + 1;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"id\":"); sb_json_str(&sb, id);
    SB_LIT(&sb, ",\"url\":\"/"); sb_append(&sb, rel, strlen(rel)); SB_LIT(&sb, "\"}");
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,200,"OK","{\"ok\":true}",11);
    free(sb.data);
    LOG_INFO("wiki_save id=%s html=%s", id, html_path);
}

/* POST /api/wiki-delete */
static void handle_api_wiki_delete(int client_fd, const char *body)
{
    char id[128]={0}; json_get_str(body,"id",id,sizeof(id));
    if (!id[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",34); return; }
    for (size_t i=0;id[i];i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    /* locate .md file and read category from META */
    char cat[512]={0};
    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) == 0) {
        FILE *fp = fopen(md_path,"r");
        if (fp) {
            char line[4096]={0}; fgets(line,sizeof(line),fp); fclose(fp);
            if (strncmp(line,"<!--META ",9)==0) {
                char *end=strstr(line,"-->");
                if (end) { *end='\0'; json_get_str(line+9,"category",cat,sizeof(cat)); }
            }
        }
        unlink(md_path);
    }
    char html_path[1024];
    if (cat[0]) snprintf(html_path,sizeof(html_path),"%s/%s/%s.html",WIKI_ROOT,cat,id);
    else         snprintf(html_path,sizeof(html_path),"%s/%s.html",WIKI_ROOT,id);
    unlink(html_path);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_delete id=%s",id);
}

/* 从已生成的 HTML 文件中提取正文（article-body 内容） */
static void wiki_extract_body(const char *html_path, strbuf_t *hbody)
{
#define BODY_START "<div class=\"ab\" id=\"article-body\">\n"
#define BODY_END   "\n</div>\n</article>"
    strbuf_t hfull = {0};
    FILE *fp = fopen(html_path, "r");
    if (fp) {
        char buf[8192]; size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) sb_append(&hfull, buf, nr);
        fclose(fp);
    }
    if (hfull.data) {
        const char *start = strstr(hfull.data, BODY_START);
        if (start) {
            start += strlen(BODY_START);
            const char *end = strstr(start, BODY_END);
            if (end) sb_append(hbody, start, (size_t)(end - start));
        }
    }
    free(hfull.data);
#undef BODY_START
#undef BODY_END
}

/* 读取已有 .html 的 body 内容，用新参数重写该文件（保留正文，更新标题/分类/时间） */
static void wiki_rewrite_html(const char *id, const char *title,
                               const char *cat, const char *updated)
{
    char html_path[1024];
    if (cat && cat[0])
        snprintf(html_path, sizeof(html_path), "%s/%s/%s.html", WIKI_ROOT, cat, id);
    else
        snprintf(html_path, sizeof(html_path), "%s/%s.html", WIKI_ROOT, id);

    strbuf_t hbody = {0};
    wiki_extract_body(html_path, &hbody);
    wiki_write_html_file(html_path, id, title, cat ? cat : "", updated,
                         hbody.data ? hbody.data : "");
    free(hbody.data);
}

/* 递归全文搜索 .md 文件内容（META 行之后），将匹配项追加到 JSON 数组 */
static void wiki_search_dir(strbuf_t *sb, int *pfirst, const char *dir, const char *q)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_search_dir(sb, pfirst, child, q); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        /* 读 META 行 */
        char ml[4096] = {0}; fgets(ml, sizeof(ml), fp);
        /* 读正文 */
        strbuf_t body = {0}; char buf[8192]; size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) sb_append(&body, buf, nr);
        fclose(fp);
        /* 解析 META */
        char id[128]={0},title[512]={0},cat[512]={0},cre[64]={0},upd[64]={0};
        if (strncmp(ml,"<!--META ",9)==0) {
            char *end=strstr(ml,"-->"); if (end) { *end='\0';
                const char *mj=ml+9;
                json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
                json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"created",cre,sizeof(cre));
                json_get_str(mj,"updated",upd,sizeof(upd)); }
        }
        if (!id[0]) { free(body.data); continue; }
        /* 不重复匹配已在 title/category 中命中的（由前端合并） */
        /* 对正文做大小写不敏感搜索 */
        int found = 0;
        if (body.data) {
            /* 逐字节比较（避免分配 lowercase 副本，直接用 tolower 比较） */
            size_t qlen = strlen(q);
            for (size_t i = 0; i + qlen <= body.len && !found; i++) {
                size_t j;
                for (j = 0; j < qlen; j++) {
                    if (tolower((unsigned char)body.data[i+j]) != (unsigned char)q[j]) break;
                }
                if (j == qlen) found = 1;
            }
        }
        free(body.data);
        if (!found) continue;
        if (!*pfirst) SB_LIT(sb, ","); *pfirst = 0;
        SB_LIT(sb,"{\"id\":"); sb_json_str(sb,id);
        SB_LIT(sb,",\"title\":"); sb_json_str(sb,title);
        SB_LIT(sb,",\"category\":"); sb_json_str(sb,cat);
        SB_LIT(sb,",\"created\":"); sb_json_str(sb,cre);
        SB_LIT(sb,",\"updated\":"); sb_json_str(sb,upd);
        SB_LIT(sb,"}");
    }
    closedir(d);
}

/* GET /api/wiki-search?q=xxx — 全文搜索 */
static void handle_api_wiki_search(int client_fd, const char *path_qs)
{
    char q[256] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "q=");
        if (p) { p += 2; size_t j=0; while(*p&&*p!='&'&&j<sizeof(q)-1) q[j++]=*p++; }
    }
    url_decode_report_fn(q);
    if (!q[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing q\"}",32); return; }
    /* 转小写，用于大小写不敏感匹配 */
    for (char *p=q; *p; p++) *p=(char)tolower((unsigned char)*p);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"articles\":[");
    int first = 1;
    wiki_search_dir(&sb, &first, WIKI_MD_DB, q);
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"articles\":[]}", 24);
    free(sb.data);
}

/* GET /api/wiki-rebuild-html — 重建所有 wiki HTML 文件（修复旧版侧栏脚本等） */
static void handle_api_wiki_rebuild_html(int client_fd)
{
    wiki_ensure_dirs();
    int count = 0;
    wiki_rebuild_md_dir(&count, WIKI_MD_DB);
    char resp[64];
    int rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"rebuilt\":%d}", count);
    send_json(client_fd, 200, "OK", resp, (size_t)rlen);
    LOG_INFO("wiki_rebuild_html count=%d", count);
}

/* 递归删除目录（空目录及其子目录） */
static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { rmdir(path); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rmdir_recursive(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* POST /api/wiki-rename-article — 重命名文章标题，同步更新 .md META 和 .html */
static void handle_api_wiki_rename_article(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, new_title[512]={0};
    json_get_str(body, "id",    id,        sizeof(id));
    json_get_str(body, "title", new_title, sizeof(new_title));
    if (!id[0] || !new_title[0]) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"missing id/title\"}",40); return;
    }
    for (size_t i=0; id[i]; i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request",
                      "{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    /* 读取原 .md 文件 */
    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) < 0) {
        send_json(client_fd,404,"Not Found",
                  "{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(md_path, "r");
    if (!fp) {
        send_json(client_fd,404,"Not Found",
                  "{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    char meta_line[4096]={0};
    fgets(meta_line, sizeof(meta_line), fp);
    strbuf_t cbuf={0};
    char buf[8192]; size_t nr;
    while ((nr=fread(buf,1,sizeof(buf),fp))>0) sb_append(&cbuf,buf,nr);
    fclose(fp);

    /* 解析旧 META */
    char cat[512]={0}, created[64]={0}, updated[64]={0};
    if (strncmp(meta_line,"<!--META ",9)==0) {
        char *end=strstr(meta_line,"-->");
        if (end) { *end='\0'; const char *mj=meta_line+9;
            json_get_str(mj,"category",cat,sizeof(cat));
            json_get_str(mj,"created",created,sizeof(created));
            json_get_str(mj,"updated",updated,sizeof(updated)); }
    }
    char now[64]; wiki_now_iso(now, sizeof(now));

    /* 重写 .md（新标题） */
    fp = fopen(md_path, "wb");
    if (!fp) { free(cbuf.data);
        send_json(client_fd,500,"Internal Server Error",
                  "{\"ok\":false,\"error\":\"write md\"}",33); return; }
    strbuf_t mb={0};
    SB_LIT(&mb,"<!--META ");
    SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,id);
    SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,new_title);
    SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,cat);
    SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,created);
    SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,now);
    SB_LIT(&mb,"}-->\n");
    if (mb.data) fwrite(mb.data,1,mb.len,fp);
    if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
    fclose(fp);
    free(mb.data);

    free(cbuf.data);
    wiki_rewrite_html(id, new_title, cat, now);

    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_rename_article id=%s new_title=%s", id, new_title);
}

/* POST /api/wiki-rename-cat — 重命名目录（最后一段），更新所有相关 .md META */
static void handle_api_wiki_rename_cat(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char old_path[512]={0}, new_name[256]={0};
    json_get_str(body, "old_path", old_path, sizeof(old_path));
    json_get_str(body, "new_name", new_name, sizeof(new_name));
    if (!old_path[0] || !new_name[0]) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"missing params\"}",38); return;
    }
    if (!register_subdir_safe(old_path) || !register_subdir_safe(new_name)) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    /* 计算新路径（替换最后一段） */
    char new_path[512]={0};
    const char *slash = strrchr(old_path, '/');
    if (slash)
        snprintf(new_path,sizeof(new_path),"%.*s/%s",(int)(slash-old_path),old_path,new_name);
    else
        snprintf(new_path,sizeof(new_path),"%s",new_name);

    /* 检查新路径不存在 */
    char new_full_root[1024], old_full_root[1024];
    snprintf(old_full_root,sizeof(old_full_root),"%s/%s",WIKI_ROOT,old_path);
    snprintf(new_full_root,sizeof(new_full_root),"%s/%s",WIKI_ROOT,new_path);
    struct stat st;
    if (stat(new_full_root,&st)==0) {
        send_json(client_fd,409,"Conflict",
                  "{\"ok\":false,\"error\":\"already exists\"}",38); return;
    }
    /* 重命名 WIKI_ROOT 目录 */
    if (rename(old_full_root,new_full_root)!=0 && errno!=ENOENT) {
        char err[128]; snprintf(err,sizeof(err),"{\"ok\":false,\"error\":\"%s\"}",strerror(errno));
        send_json(client_fd,500,"Internal Server Error",err,strlen(err)); return;
    }
    /* 重命名 WIKI_MD_DB 目录 */
    char old_full_md[1024], new_full_md[1024];
    snprintf(old_full_md,sizeof(old_full_md),"%s/%s",WIKI_MD_DB,old_path);
    snprintf(new_full_md,sizeof(new_full_md),"%s/%s",WIKI_MD_DB,new_path);
    rename(old_full_md,new_full_md); /* 目录不存在时忽略错误 */

    /* 递归更新所有 .md 文件中匹配 old_path 的 category 字段 */
    size_t old_len = strlen(old_path);
    wiki_update_cat_in_dir(WIKI_MD_DB, old_path, new_path, old_len);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_rename_cat old=%s new=%s", old_path, new_path);
}

/* POST /api/wiki-delete-cat — 删除空目录 */
static void handle_api_wiki_delete_cat(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char path[512]={0};
    json_get_str(body,"path",path,sizeof(path));
    if (!path[0]) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"missing path\"}",37); return;
    }
    if (!register_subdir_safe(path)) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    /* 检查 md_db 对应子目录是否有 .md 文件 */
    char md_cat_dir[1024]; snprintf(md_cat_dir, sizeof(md_cat_dir), "%s/%s", WIKI_MD_DB, path);
    if (wiki_md_dir_has_md(md_cat_dir)) {
        send_json(client_fd,409,"Conflict",
                  "{\"ok\":false,\"error\":\"not empty\"}", 30); return;
    }
    char full_root[1024], full_md[1024];
    snprintf(full_root,sizeof(full_root),"%s/%s",WIKI_ROOT,path);
    snprintf(full_md,  sizeof(full_md),  "%s/%s",WIKI_MD_DB,path);
    rmdir_recursive(full_md);
    rmdir_recursive(full_root);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_delete_cat path=%s", path);
}

/* POST /api/wiki-move-article — 将文章移动到另一分类 */
static void handle_api_wiki_move_article(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, new_cat[512]={0};
    json_get_str(body, "id",       id,      sizeof(id));
    json_get_str(body, "category", new_cat, sizeof(new_cat));
    if (!id[0]) {
        send_json(client_fd,400,"Bad Request",
                  "{\"ok\":false,\"error\":\"missing id\"}",33); return;
    }
    for (size_t i=0; id[i]; i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request",
                      "{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }

    /* 读 .md 文件 */
    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) < 0) {
        send_json(client_fd,404,"Not Found",
                  "{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(md_path,"r");
    if (!fp) {
        send_json(client_fd,404,"Not Found",
                  "{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    char meta_line[4096]={0}; fgets(meta_line,sizeof(meta_line),fp);
    strbuf_t cbuf={0}; char buf[8192]; size_t nr;
    while ((nr=fread(buf,1,sizeof(buf),fp))>0) sb_append(&cbuf,buf,nr);
    fclose(fp);

    /* 解析旧 META */
    char old_cat[512]={0}, title[512]={0}, created[64]={0};
    if (strncmp(meta_line,"<!--META ",9)==0) {
        char *mend=strstr(meta_line,"-->");
        if (mend) { *mend='\0'; const char *mj=meta_line+9;
            json_get_str(mj,"category",old_cat,sizeof(old_cat));
            json_get_str(mj,"title",title,sizeof(title));
            json_get_str(mj,"created",created,sizeof(created)); }
    }

    /* 若分类未变，直接返回 */
    if (strcmp(old_cat,new_cat)==0) {
        free(cbuf.data);
        send_json(client_fd,200,"OK","{\"ok\":true}",11); return;
    }

    /* 旧 html 路径 */
    char old_html[1024];
    if (old_cat[0]) snprintf(old_html,sizeof(old_html),"%s/%s/%s.html",WIKI_ROOT,old_cat,id);
    else            snprintf(old_html,sizeof(old_html),"%s/%s.html",WIKI_ROOT,id);

    /* 新 html 路径 */
    char new_html[1024];
    if (new_cat[0]) snprintf(new_html,sizeof(new_html),"%s/%s/%s.html",WIKI_ROOT,new_cat,id);
    else            snprintf(new_html,sizeof(new_html),"%s/%s.html",WIKI_ROOT,id);

    /* 从旧 html 提取正文 */
    strbuf_t hbody = {0};
    wiki_extract_body(old_html, &hbody);

    /* 确保新分类目录存在 */
    if (new_cat[0]) {
        char new_dir[1024]; snprintf(new_dir,sizeof(new_dir),"%s/%s",WIKI_ROOT,new_cat);
        mkdir_p(new_dir);
    }

    char now[64]; wiki_now_iso(now,sizeof(now));

    /* 写新 html，删旧 html */
    wiki_write_html_file(new_html, id, title, new_cat, now,
                         hbody.data ? hbody.data : "");
    free(hbody.data);
    unlink(old_html);

    /* 更新 .md META（写到新分类路径，删旧路径） */
    char new_md_path[768];
    wiki_md_write_path(new_md_path, sizeof(new_md_path), id, new_cat);
    fp = fopen(new_md_path,"wb");
    if (fp) {
        strbuf_t mb={0};
        SB_LIT(&mb,"<!--META ");
        SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,id);
        SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,title);
        SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,new_cat);
        SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,created);
        SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,now);
        SB_LIT(&mb,"}-->\n");
        if (mb.data) fwrite(mb.data,1,mb.len,fp);
        if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
        fclose(fp);
        free(mb.data);
    }
    free(cbuf.data);

    /* 删除旧 .md（若路径已变） */
    if (strcmp(md_path, new_md_path) != 0) unlink(md_path);

    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_move_article id=%s old_cat=%s new_cat=%s", id, old_cat, new_cat);
}

/* POST /api/wiki-mkdir */
static void handle_api_wiki_mkdir(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char path[512]={0}; json_get_str(body,"path",path,sizeof(path));
    if (!path[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing path\"}",37); return; }
    if (!register_subdir_safe(path)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    char full[1024]; snprintf(full,sizeof(full),"%s/%s",WIKI_ROOT,path);
    if (mkdir_p(full)!=0 && errno!=EEXIST) {
        char err[128]; snprintf(err,sizeof(err),"{\"ok\":false,\"error\":\"%s\"}",strerror(errno));
        send_json(client_fd,500,"Internal Server Error",err,strlen(err)); return;
    }
    /* 在 md_db 下同步建子目录，用于 wiki-list 扫描持久化空分类 */
    char full_md[1024]; snprintf(full_md,sizeof(full_md),"%s/%s",WIKI_MD_DB,path);
    mkdir_p(full_md);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
}

/* POST /api/wiki-cleanup-uploads — 删除 uploads 中未被任何 .md 引用的文件 */

/* 简单字符串链表，用于存储引用的 uploads 相对路径 */
typedef struct _strnode { char *s; struct _strnode *next; } _strnode_t;

static void _strlist_add(_strnode_t **head, const char *s, size_t len)
{
    char *dup = malloc(len + 1);
    if (!dup) return;
    memcpy(dup, s, len); dup[len] = '\0';
    _strnode_t *n = malloc(sizeof(_strnode_t));
    if (!n) { free(dup); return; }
    n->s = dup; n->next = *head; *head = n;
}

static int _strlist_has(_strnode_t *head, const char *s)
{
    while (head) { if (strcmp(head->s, s) == 0) return 1; head = head->next; }
    return 0;
}

static void _strlist_free(_strnode_t *head)
{
    while (head) { _strnode_t *n = head->next; free(head->s); free(head); head = n; }
}

/* 递归扫描 md_db，从每个 .md 正文收集 /wiki/uploads/ 之后的路径 */
static void collect_upload_refs(const char *dir, _strnode_t **refs)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { collect_upload_refs(child, refs); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char skip[4096]; fgets(skip, sizeof(skip), fp); /* skip META */
        strbuf_t buf = {0}; char tmp[8192]; size_t nr;
        while ((nr = fread(tmp, 1, sizeof(tmp), fp)) > 0) sb_append(&buf, tmp, nr);
        fclose(fp);
        if (!buf.data) continue;
        /* 查找所有 /wiki/uploads/ 引用，提取其后的路径 */
        const char *marker = "/wiki/uploads/";
        size_t mlen = strlen(marker);
        const char *p = buf.data;
        while ((p = strstr(p, marker)) != NULL) {
            p += mlen;
            const char *end = p;
            while (*end && *end != ')' && *end != '"' && *end != '\'' && !isspace((unsigned char)*end)) end++;
            size_t plen = (size_t)(end - p);
            if (plen > 0 && plen < 768) _strlist_add(refs, p, plen);
            p = end;
        }
        free(buf.data);
    }
    closedir(d);
}

/* 递归扫描 uploads 目录，删除未被引用的文件 */
static void cleanup_unreferenced(const char *dir, const char *rel,
                                  _strnode_t *refs, int *pdel, int *pkept)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        char relc[768];
        if (rel[0]) snprintf(relc, sizeof(relc), "%s/%s", rel, de->d_name);
        else        snprintf(relc, sizeof(relc), "%s", de->d_name);
        if (S_ISDIR(st.st_mode)) {
            cleanup_unreferenced(child, relc, refs, pdel, pkept); continue;
        }
        if (_strlist_has(refs, relc)) { (*pkept)++; }
        else { unlink(child); (*pdel)++; LOG_INFO("cleanup_uploads: delete %s", child); }
    }
    closedir(d);
}

static void handle_api_wiki_cleanup_uploads(int client_fd)
{
    wiki_ensure_dirs();
    _strnode_t *refs = NULL;
    collect_upload_refs(WIKI_MD_DB, &refs);
    int deleted = 0, kept = 0;
    cleanup_unreferenced(WIKI_UPLOADS, "", refs, &deleted, &kept);
    _strlist_free(refs);
    char resp[128];
    int rlen = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"deleted\":%d,\"kept\":%d}", deleted, kept);
    send_json(client_fd, 200, "OK", resp, (size_t)rlen);
    LOG_INFO("wiki_cleanup_uploads deleted=%d kept=%d", deleted, kept);
}

/* POST /api/wiki-upload */
static void handle_api_wiki_upload(int client_fd, const char *req_headers,
                                   const char *body, size_t body_len)
{
    wiki_ensure_dirs();
    char filename[256]={0};
    http_header_value(req_headers,"X-Wiki-Filename",filename,sizeof(filename));
    url_decode_report_fn(filename);
    if (!filename[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing filename\"}",41); return;
    }
    if (!wiki_upload_safe(filename)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid filename\"}",42); return;
    }
    /* 读取分类，上传到对应子目录 */
    char cat[512]={0};
    http_header_value(req_headers,"X-Wiki-Category",cat,sizeof(cat));
    url_decode_report_fn(cat);
    char upload_dir[1024];
    if (cat[0] && register_subdir_safe(cat)) {
        snprintf(upload_dir, sizeof(upload_dir), "%s/%s", WIKI_UPLOADS, cat);
        mkdir_p(upload_dir);
    } else {
        snprintf(upload_dir, sizeof(upload_dir), "%s", WIKI_UPLOADS);
    }
    char filepath[1024]; snprintf(filepath,sizeof(filepath),"%s/%s",upload_dir,filename);
    FILE *fp = fopen(filepath,"wb");
    if (!fp) { send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"open\"}",29); return; }
    if (body_len>0) fwrite(body,1,body_len,fp);
    fclose(fp);
    /* 构造 URL（相对 WIKI_UPLOADS） */
    strbuf_t sb={0};
    SB_LIT(&sb,"{\"ok\":true,\"url\":\"/wiki/uploads/");
    if (cat[0] && register_subdir_safe(cat)) {
        sb_append(&sb,cat,strlen(cat)); SB_LIT(&sb,"/");
    }
    sb_append(&sb,filename,strlen(filename));
    SB_LIT(&sb,"\"}");
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,200,"OK","{\"ok\":true}",11);
    free(sb.data);
    LOG_INFO("wiki_upload %s (%zu bytes)",filepath,body_len);
}

/* ------------------------------------------------------------------ */
/*  POST /api/svn-log  (本地调用 svn log --xml -v)                    */
/* ------------------------------------------------------------------ */

static int svn_url_safe(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && !strchr("/:.-_~%@?=&+", (int)c)) return 0;
    }
    return 1;
}

static int svn_simple_safe(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return 0;
    }
    return 1;
}

static int svn_date_safe(const char *s)
{
    if (strlen(s) != 10) return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else { if (!isdigit((unsigned char)s[i])) return 0; }
    }
    return 1;
}

static void handle_api_svn_log(int client_fd, const char *body)
{
    char url[1024] = {0}, user[128] = {0}, pass[512] = {0};
    char author[128] = {0}, date_from[32] = {0}, date_to[32] = {0};
    int  limit = 500;

    json_get_str(body, "url",       url,       sizeof(url));
    json_get_str(body, "user",      user,      sizeof(user));
    json_api_get_pass(body,         pass,      sizeof(pass));
    json_get_str(body, "author",    author,    sizeof(author));
    json_get_str(body, "date_from", date_from, sizeof(date_from));
    json_get_str(body, "date_to",   date_to,   sizeof(date_to));
    limit = json_get_int(body, "limit", 500);
    if (limit <= 0 || limit > 5000) limit = 500;

    if (!url[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing url\"}", 37); return;
    }
    if (!svn_url_safe(url)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid url\"}", 36); return;
    }
    if (user[0] && !svn_simple_safe(user)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid user\"}", 37); return;
    }
    if (author[0] && !svn_simple_safe(author)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid author\"}", 39); return;
    }
    if (date_from[0] && !svn_date_safe(date_from)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid date_from\"}", 42); return;
    }
    if (date_to[0] && !svn_date_safe(date_to)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid date_to\"}", 40); return;
    }

    char limit_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", limit);

    char rev_arg[80] = {0};
    if (date_from[0] || date_to[0]) {
        const char *f = date_from[0] ? date_from : "1970-01-01";
        if (date_to[0])
            snprintf(rev_arg, sizeof(rev_arg), "{%s}:{%s}", f, date_to);
        else
            snprintf(rev_arg, sizeof(rev_arg), "{%s}:HEAD", f);
    }

    const char *argv[32];
    int ai = 0;
    argv[ai++] = "svn";
    argv[ai++] = "log";
    argv[ai++] = "--xml";
    argv[ai++] = "-v";
    argv[ai++] = "--no-auth-cache";
    argv[ai++] = "--non-interactive";
    if (user[0]) { argv[ai++] = "--username"; argv[ai++] = user; }
    if (pass[0]) { argv[ai++] = "--password"; argv[ai++] = pass; }
    argv[ai++] = "--limit";
    argv[ai++] = limit_str;
    if (rev_arg[0]) { argv[ai++] = "-r"; argv[ai++] = rev_arg; }
    argv[ai++] = url;
    argv[ai]   = NULL;

    int pfd[2];
    if (pipe(pfd) < 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"pipe failed\"}", 36); return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"fork failed\"}", 36); return;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp("svn", (char *const *)argv);
        fprintf(stderr, "execvp svn failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(pfd[1]);
    strbuf_t out = {0};
    char rbuf[8192];
    ssize_t rn;
    while ((rn = read(pfd[0], rbuf, sizeof(rbuf))) > 0)
        sb_append(&out, rbuf, (size_t)rn);
    close(pfd[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    strbuf_t sb = {0};
    if (exit_code == 0) {
        SB_LIT(&sb, "{\"ok\":true,\"xml\":");
        sb_json_str(&sb, out.data ? out.data : "");
        SB_LIT(&sb, "}");
    } else {
        SB_LIT(&sb, "{\"ok\":false,\"error\":");
        sb_json_str(&sb, out.data ? out.data : "svn exited with error");
        sb_appendf(&sb, ",\"exit_code\":%d}", exit_code);
    }
    free(out.data);

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"out of memory\"}", 38);
    free(sb.data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/ssh-exec-one  (单条命令，实时响应)                       */
/* ------------------------------------------------------------------ */

#define MAX_BODY_SIZE        (64 * 1024)
#define SAVE_REPORT_MAX_BODY (50 * 1024 * 1024) /* POST /api/save-report 正文上限 */
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
    char path_qs[2048]; /* 含 ?query 的原始路径（供 list-ssh-configs / procs 等） */
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);
    strncpy(path_qs, path, sizeof(path_qs) - 1);
    path_qs[sizeof(path_qs) - 1] = '\0';
    {
        char *qm = strchr(path, '?');
        if (qm)
            *qm = '\0';
        qm = strchr(path, '#');
        if (qm)
            *qm = '\0';
    }

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
            strcmp(path, "/api/save-config") == 0 ||
            strcmp(path, "/api/save-register-file") == 0 ||
            strcmp(path, "/api/wiki-save") == 0 ||
            strcmp(path, "/api/wiki-upload") == 0)
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
        } else if (strcmp(path, "/api/save-register-file") == 0) {
            if (body)
                handle_api_save_register_file(client_fd, req_buf, body,
                                              (size_t)content_length);
            else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/rename-register-file") == 0) {
            if (body) handle_api_rename_register_file(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/delete-register-file") == 0) {
            if (body) handle_api_delete_register_file(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/rename-register-dir") == 0) {
            if (body) handle_api_rename_register_dir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/delete-register-dir") == 0) {
            if (body) handle_api_delete_register_dir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/delete-report") == 0) {
            if (body)
                handle_api_delete_report(client_fd, body);
            else
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/svn-log") == 0) {
            if (body) handle_api_svn_log(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-save") == 0) {
            if (body) handle_api_wiki_save(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete") == 0) {
            if (body) handle_api_wiki_delete(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-rename-article") == 0) {
            if (body) handle_api_wiki_rename_article(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-rename-cat") == 0) {
            if (body) handle_api_wiki_rename_cat(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete-cat") == 0) {
            if (body) handle_api_wiki_delete_cat(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-move-article") == 0) {
            if (body) handle_api_wiki_move_article(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-mkdir") == 0) {
            if (body) handle_api_wiki_mkdir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-upload") == 0) {
            if (body) handle_api_wiki_upload(client_fd, req_buf, body,
                                             (size_t)content_length);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-cleanup-uploads") == 0) {
            handle_api_wiki_cleanup_uploads(client_fd);
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
            handle_api_list_ssh_configs(client_fd, path_qs);
            goto done;
        }
    }

    if (strcmp(path, "/api/list-all-configs") == 0) {
        handle_api_list_all_configs(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-list") == 0) {
        handle_api_wiki_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
        handle_api_wiki_rebuild_html(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/wiki-read", 14) == 0 &&
        (path[14] == '\0' || path[14] == '?')) {
        handle_api_wiki_read(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-search", 16) == 0 &&
        (path[16] == '\0' || path[16] == '?')) {
        handle_api_wiki_search(client_fd, path_qs);
        goto done;
    }

    if (strcmp(path, "/api/list-register-files") == 0) {
        handle_api_list_register_files(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/list-register-dirs") == 0) {
        handle_api_list_register_dirs(client_fd);
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
        const char *qs = strchr(path_qs, '?');
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
        const char *qs = strchr(path_qs, '?');
        if (qs) {
            const char *pp = strstr(qs, "port=");
            if (pp) port_num = atoi(pp + 5);
        }
        handle_api_port(client_fd, port_num);
        goto done;
    }

    /* GET /api/log-files — 返回 logs/ 目录下所有 .log 文件名列表（按名排序）。
     * 替代旧方案（前端逐个 HEAD 探测），一次请求即可填充下拉列表。 */
    if (strcmp(path, "/api/log-files") == 0) {
        strbuf_t sb = {0};
        SB_LIT(&sb, "{\"ok\":true,\"files\":[");
        DIR *ld = opendir("logs");
        if (ld) {
            /* 收集文件名 */
            char names[LOG_MAX_FILES + 2][64];
            int  nc = 0;
            struct dirent *de;
            while ((de = readdir(ld)) != NULL && nc < LOG_MAX_FILES) {
                const char *n  = de->d_name;
                size_t      nl = strlen(n);
                if (nl > 4 && strcmp(n + nl - 4, ".log") == 0) {
                    strncpy(names[nc], n, 63);
                    names[nc][63] = '\0';
                    nc++;
                }
            }
            closedir(ld);
            /* 按名称升序排列（server_0 < server_1 < …） */
            for (int i = 0; i < nc - 1; i++)
                for (int j = 0; j < nc - i - 1; j++)
                    if (strcmp(names[j], names[j + 1]) > 0) {
                        char tmp[64];
                        memcpy(tmp,        names[j],     64);
                        memcpy(names[j],   names[j + 1], 64);
                        memcpy(names[j+1], tmp,          64);
                    }
            for (int i = 0; i < nc; i++) {
                if (i) SB_LIT(&sb, ",");
                sb_json_str(&sb, names[i]);
            }
        }
        SB_LIT(&sb, "]}");
        if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
        else send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 12);
        goto done;
    }

    /* /logs/<file> — 直接从真实 logs 目录（与 html/ 同级）读取，
     * 无需在 html/ 下创建软链接。
     * ".." 已在上方统一拦截，此处无路径穿越风险。 */
    if (strncmp(path, "/logs/", 6) == 0 && path[6] != '\0') {
        char filepath[2048];
        char logs_seg[2048];
        strncpy(logs_seg, path + 6, sizeof(logs_seg) - 1);
        logs_seg[sizeof(logs_seg) - 1] = '\0';
        url_decode_report_fn(logs_seg);
        snprintf(filepath, sizeof(filepath), "logs/%s", logs_seg);
        if (send_file(client_fd, filepath) < 0)
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1>");
        goto done;
    }

    {
        char filepath[2048];
        char decoded_path[2048];
        strncpy(decoded_path, path, sizeof(decoded_path) - 1);
        decoded_path[sizeof(decoded_path) - 1] = '\0';
        url_decode_report_fn(decoded_path);
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, decoded_path);

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
