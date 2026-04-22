#include "http_utils.h"
#include "http_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#define open _open
#define read _read
#define close _close
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#endif

/* ── MIME 类型映射 ─────────────────────────────────────── */
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

/* ── 动态字符串缓冲区 ──────────────────────────────────── */
int sb_reserve(strbuf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t nc = (b->cap + extra + 1) * 2;
    if (nc < 32768) nc = 32768;
    char *nd = realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd; b->cap = nc;
    return 0;
}

int sb_append(strbuf_t *b, const char *s, size_t n)
{
    if (sb_reserve(b, n)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int sb_appendf(strbuf_t *b, const char *fmt, ...)
{
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
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) return -1;
    va_start(ap, fmt);
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = sb_append(b, tmp, (size_t)n);
    free(tmp);
    return r;
}

int sb_json_str(strbuf_t *b, const char *s)
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

/* ── JSON 解析辅助 ──────────────────────────────────────── */
int json_get_str(const char *json, const char *key, char *out, size_t len)
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

void json_read_pass_value_at(const char *vp, char *out, size_t len)
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

const char *json_parse_skip_string(const char *p)
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

const char *json_skip_composite(const char *p)
{
    int b = 0, s = 0;
    if (*p == '{') { b++; p++; }
    else if (*p == '[') { s++; p++; }
    else return p;
    while (*p && (b > 0 || s > 0)) {
        if (*p == '"')  { p = json_parse_skip_string(p); continue; }
        if (*p == '{')  { b++; p++; continue; }
        if (*p == '}')  { b--; p++; continue; }
        if (*p == '[')  { s++; p++; continue; }
        if (*p == ']')  { s--; p++; continue; }
        p++;
    }
    return p;
}

const char *json_skip_value_full(const char *p)
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

void json_api_get_pass(const char *body, char *out, size_t len)
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
            if (*p == '\\' && p[1]) { p += 2; continue; }
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
        if (hit) { json_read_pass_value_at(p, out, len); return; }
        p = json_skip_value_full(p);
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') { p++; continue; }
        if (*p == '}') return;
    }
}

int json_get_int(const char *json, const char *key, int def)
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

int json_get_str_array(const char *json, const char *key,
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

/* ── HTTP 响应 ──────────────────────────────────────────── */
void send_response(http_sock_t fd, int status, const char *status_text,
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
    (void)http_sock_send_all(fd, header, (size_t)hlen);
    (void)http_sock_send_all(fd, body, (size_t)body_len);
}

void send_json(http_sock_t fd, int status, const char *status_text,
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
    (void)http_sock_send_all(fd, header, (size_t)hlen);
    (void)http_sock_send_all(fd, json, json_len);
}

int send_file(http_sock_t fd, const char *filepath)
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
    (void)http_sock_send_all(fd, header, (size_t)hlen);

    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
        if (http_sock_send_all(fd, buf, (size_t)n) < 0) {
            break;
        }
    }

    close(file_fd);
    return 0;
}

int http_header_value(const char *headers, const char *name,
                      char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    for (;;) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end == p) return -1;
        if ((size_t)(line_end - p) >= nlen + 1 &&
            strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(line_end - v);
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            while (vl > 0 && (out[vl - 1] == ' ' || out[vl - 1] == '\t'))
                out[--vl] = '\0';
            return 0;
        }
        p = line_end + 2;
    }
}

/* ── URL / 路径工具 ─────────────────────────────────────── */
void url_decode_report_fn(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            char hx[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hx, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int query_param_get(const char *path, const char *key, char *out, size_t cap)
{
    const char *q = strchr(path, '?');
    size_t klen;
    if (!q || cap < 2) return -1;
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
                    out[i++] = ' '; q++;
                } else {
                    out[i++] = *q++;
                }
            }
            out[i] = '\0';
            return 0;
        }
        const char *amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return -1;
}

int mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (platform_mkdir(tmp) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (platform_mkdir(tmp) != 0 && errno != EEXIST) ? -1 : 0;
}

int register_subdir_safe(const char *s)
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
