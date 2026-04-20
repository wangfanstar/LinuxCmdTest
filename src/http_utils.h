#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>
#include <stdarg.h>

/* ── 动态字符串缓冲区 ──────────────────────────────────── */
typedef struct { char *data; size_t len; size_t cap; } strbuf_t;

int  sb_reserve(strbuf_t *b, size_t extra);
int  sb_append(strbuf_t *b, const char *s, size_t n);
int  sb_appendf(strbuf_t *b, const char *fmt, ...);
int  sb_json_str(strbuf_t *b, const char *s);

#define SB_LIT(b, s)  sb_append(b, s, sizeof(s) - 1)

/* ── JSON 解析辅助 ──────────────────────────────────────── */
int         json_get_str(const char *json, const char *key,
                         char *out, size_t len);
void        json_read_pass_value_at(const char *vp, char *out, size_t len);
const char *json_parse_skip_string(const char *p);
const char *json_skip_composite(const char *p);
const char *json_skip_value_full(const char *p);
void        json_api_get_pass(const char *body, char *out, size_t len);
int         json_get_int(const char *json, const char *key, int def);
int         json_get_str_array(const char *json, const char *key,
                               char **out, int max_count, size_t item_len);

/* ── HTTP 响应 ──────────────────────────────────────────── */
void send_response(int fd, int status, const char *status_text,
                   const char *body);
void send_json(int fd, int status, const char *status_text,
               const char *json, size_t json_len);
int  send_file(int fd, const char *filepath);
int  http_header_value(const char *headers, const char *name,
                       char *out, size_t cap);

/* ── URL / 路径工具 ─────────────────────────────────────── */
void url_decode_report_fn(char *s);
int  query_param_get(const char *path, const char *key,
                     char *out, size_t cap);
int  mkdir_p(const char *path);

/* 路径安全检查（无 ..、非绝对路径，支持多级 a/b/c） */
int  register_subdir_safe(const char *s);

#endif /* HTTP_UTILS_H */
