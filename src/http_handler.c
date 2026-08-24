#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "svn_api.h"
#include "register_api.h"
#include "wiki.h"
#include "auth_db.h"
#include "admin_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <errno.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#define MAX_BODY_SIZE        (64 * 1024)
#define SAVE_REPORT_MAX_BODY (50 * 1024 * 1024)
#define MAX_HTML_PASTE_SIZE  (1024 * 1024)
#define MAX_HTML_PASTE_BODY  (MAX_HTML_PASTE_SIZE + 4096)

static int is_wiki_write_api(const char *path)
{
    return strcmp(path, "/api/wiki-save") == 0 ||
           strcmp(path, "/api/wiki-delete") == 0 ||
           strcmp(path, "/api/wiki-rename-article") == 0 ||
           strcmp(path, "/api/wiki-rename-cat") == 0 ||
           strcmp(path, "/api/wiki-delete-cat") == 0 ||
           strcmp(path, "/api/wiki-move-article") == 0 ||
           strcmp(path, "/api/wiki-mkdir") == 0 ||
           strcmp(path, "/api/wiki-upload") == 0 ||
           strcmp(path, "/api/wiki-rebuild-html") == 0 ||
           strcmp(path, "/api/wiki-cleanup-uploads") == 0 ||
           strcmp(path, "/api/wiki-trash-restore") == 0 ||
           strcmp(path, "/api/wiki-trash-empty") == 0 ||
           strcmp(path, "/api/wiki-restore-version") == 0;
}

/* ── GET /api/codechecker-list ───────────────────────────────── */

typedef struct {
    char name[512];
    time_t mtime;
    long long size;
} codechecker_entry_t;

static void handle_api_codechecker_list(http_sock_t client_fd)
{
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/codechecker_html", WEB_ROOT);
    DIR *d = opendir(dir_path);
    if (!d) {
        static const char err[] =
            "{\"ok\":false,\"error\":\"codechecker_html directory not found\"}";
        send_json(client_fd, 404, "Not Found", err, sizeof(err) - 1);
        return;
    }

    codechecker_entry_t *entries = NULL;
    size_t n = 0, cap = 0;
    codechecker_entry_t idx;
    int has_index = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        size_t nl = strlen(nm);
        if (nl < 5 || strcasecmp(nm + nl - 5, ".html") != 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, nm);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (strcmp(nm, "index.html") == 0) {
            memset(&idx, 0, sizeof(idx));
            snprintf(idx.name, sizeof(idx.name), "%s", nm);
            idx.mtime = st.st_mtime;
            idx.size = (long long)st.st_size;
            has_index = 1;
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            codechecker_entry_t *ne = realloc(entries, ncap * sizeof(*ne));
            if (!ne) break;
            entries = ne;
            cap = ncap;
        }
        snprintf(entries[n].name, sizeof(entries[n].name), "%s", nm);
        entries[n].mtime = st.st_mtime;
        entries[n].size = (long long)st.st_size;
        n++;
    }
    closedir(d);

    /* 按修改时间倒序，新的在前 */
    for (size_t i = 1; i < n; i++) {
        codechecker_entry_t tmp = entries[i];
        size_t j = i;
        while (j > 0 && entries[j - 1].mtime < tmp.mtime) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,");
    if (has_index) {
        SB_LIT(&sb, "\"index\":{\"name\":\"index.html\",\"mtime\":");
        sb_appendf(&sb, "%lld", (long long)idx.mtime);
        SB_LIT(&sb, ",\"size\":");
        sb_appendf(&sb, "%lld", idx.size);
        SB_LIT(&sb, "},");
    } else {
        SB_LIT(&sb, "\"index\":null,");
    }
    sb_appendf(&sb, "\"count\":%zu,", n);
    SB_LIT(&sb, "\"files\":[");
    for (size_t i = 0; i < n; i++) {
        if (i) SB_LIT(&sb, ",");
        SB_LIT(&sb, "{\"name\":");
        sb_json_str(&sb, entries[i].name);
        sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}",
                   (long long)entries[i].mtime, entries[i].size);
    }
    SB_LIT(&sb, "]}");
    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else {
        static const char empty[] = "{\"ok\":true,\"index\":null,\"count\":0,\"files\":[]}";
        send_json(client_fd, 200, "OK", empty, sizeof(empty) - 1);
    }
    free(sb.data);
    free(entries);
}

/* ── html_paste network library ─────────────────────────────── */

typedef struct {
    char name[256];
    time_t mtime;
    long long size;
    int is_json;
} html_paste_entry_t;

static int html_paste_name_safe(const char *name, int json_only)
{
    if (!name || !*name || strlen(name) >= 240 || name[0] == '.') return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || strchr(":<>\"|?*", *p)) return 0;
    }
    size_t len = strlen(name);
    if (len < 5) return 0;
    if (json_only)
        return strcasecmp(name + len - 5, ".json") == 0;
    return strcasecmp(name + len - 5, ".json") == 0 ||
           (len >= 5 && strcasecmp(name + len - 5, ".html") == 0);
}

static int html_paste_dir(char *out, size_t cap)
{
    if (snprintf(out, cap, "%s/html_paste", WEB_ROOT) >= (int)cap) return -1;
    if (mkdir_p(out) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int html_paste_file_path(char *out, size_t cap, const char *name,
                                int json_only)
{
    char dir[512];
    if (!html_paste_name_safe(name, json_only) || html_paste_dir(dir, sizeof(dir)) != 0)
        return -1;
    if (snprintf(out, cap, "%s/%s", dir, name) >= (int)cap) return -1;
    return 0;
}

static int html_paste_json_bool(const char *body, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body ? body : "", search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (*p >= '0' && *p <= '9') return atoi(p) != 0;
    return 0;
}

static void handle_api_html_paste_list(http_sock_t client_fd)
{
    char dir[512];
    if (html_paste_dir(dir, sizeof(dir)) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"html_paste directory unavailable\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        static const char err[] = "{\"ok\":false,\"error\":\"html_paste directory unavailable\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }

    html_paste_entry_t *entries = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!html_paste_name_safe(name, 0)) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size > MAX_HTML_PASTE_SIZE) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            html_paste_entry_t *grown = realloc(entries, next * sizeof(*grown));
            if (!grown) break;
            entries = grown;
            capacity = next;
        }
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        entries[count].mtime = st.st_mtime;
        entries[count].size = (long long)st.st_size;
        size_t len = strlen(name);
        entries[count].is_json = len >= 5 && strcasecmp(name + len - 5, ".json") == 0;
        count++;
    }
    closedir(d);

    for (size_t i = 1; i < count; i++) {
        html_paste_entry_t current = entries[i];
        size_t j = i;
        while (j > 0 && entries[j - 1].mtime < current.mtime) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = current;
    }

    strbuf_t result = {0};
    SB_LIT(&result, "{\"ok\":true,\"files\":[");
    for (size_t i = 0; i < count; i++) {
        if (i) SB_LIT(&result, ",");
        SB_LIT(&result, "{\"name\":");
        sb_json_str(&result, entries[i].name);
        SB_LIT(&result, ",\"type\":");
        sb_json_str(&result, entries[i].is_json ? "json" : "html");
        sb_appendf(&result, ",\"size\":%lld,\"mtime\":%lld}",
                   entries[i].size, (long long)entries[i].mtime);
    }
    SB_LIT(&result, "]}");
    if (result.data)
        send_json(client_fd, 200, "OK", result.data, result.len);
    else {
        static const char empty[] = "{\"ok\":true,\"files\":[]}";
        send_json(client_fd, 200, "OK", empty, sizeof(empty) - 1);
    }
    free(result.data);
    free(entries);
}

static void handle_api_html_paste_read(http_sock_t client_fd, const char *path_qs)
{
    char name[256];
    char filepath[1024];
    if (query_param_get(path_qs, "name", name, sizeof(name)) != 0 ||
        html_paste_file_path(filepath, sizeof(filepath), name, 1) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"invalid JSON file name\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }
    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
        static const char err[] = "{\"ok\":false,\"error\":\"JSON file not found\"}";
        send_json(client_fd, 404, "Not Found", err, sizeof(err) - 1);
        return;
    }
    if (st.st_size < 0 || st.st_size > MAX_HTML_PASTE_SIZE) {
        static const char err[] = "{\"ok\":false,\"error\":\"JSON file is too large\"}";
        send_json(client_fd, 413, "Payload Too Large", err, sizeof(err) - 1);
        return;
    }
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        static const char err[] = "{\"ok\":false,\"error\":\"cannot read JSON file\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    size_t size = (size_t)st.st_size;
    char *content = calloc(size + 1, 1);
    if (!content || fread(content, 1, size, fp) != size) {
        fclose(fp);
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"cannot read JSON file\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    fclose(fp);
    send_json(client_fd, 200, "OK", content, size);
    free(content);
}

static void handle_api_html_paste_save(http_sock_t client_fd, const char *body)
{
    char name[256];
    if (!body || json_get_str(body, "name", name, sizeof(name)) != 0 ||
        !html_paste_name_safe(name, 1)) {
        static const char err[] = "{\"ok\":false,\"error\":\"invalid JSON file name\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }
    char *content = calloc(MAX_HTML_PASTE_SIZE + 1, 1);
    if (!content || json_get_str(body, "content", content, MAX_HTML_PASTE_SIZE + 1) != 0) {
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"missing JSON content\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }
    size_t content_len = strlen(content);
    if (content_len > MAX_HTML_PASTE_SIZE) {
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"JSON content is too large\"}";
        send_json(client_fd, 413, "Payload Too Large", err, sizeof(err) - 1);
        return;
    }

    char filepath[1024];
    if (html_paste_file_path(filepath, sizeof(filepath), name, 1) != 0) {
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"html_paste directory unavailable\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    int overwrite = html_paste_json_bool(body, "overwrite");
    struct stat existing;
    if (!overwrite && stat(filepath, &existing) == 0) {
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"JSON file already exists\"}";
        send_json(client_fd, 409, "Conflict", err, sizeof(err) - 1);
        return;
    }

    char dir[512];
    if (html_paste_dir(dir, sizeof(dir)) != 0) {
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"html_paste directory unavailable\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    char temp[1100];
    snprintf(temp, sizeof(temp), "%s/.%s.%p.tmp", dir, name, (void *)body);
    FILE *fp = fopen(temp, "wb");
    int write_ok = fp != NULL;
    if (fp) {
        if (fwrite(content, 1, content_len, fp) != content_len) write_ok = 0;
        if (fclose(fp) != 0) write_ok = 0;
    }
    if (!write_ok) {
        remove(temp);
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"cannot save JSON file\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    if (rename(temp, filepath) != 0) {
        remove(temp);
        free(content);
        static const char err[] = "{\"ok\":false,\"error\":\"cannot finalize JSON file\"}";
        send_json(client_fd, 500, "Internal Server Error", err, sizeof(err) - 1);
        return;
    }
    free(content);
    static const char ok[] = "{\"ok\":true}";
    send_json(client_fd, 200, "OK", ok, sizeof(ok) - 1);
}

/* ── 主处理入口 ──────────────────────────────────────────────── */

void handle_client(http_sock_t client_fd, struct sockaddr_in *addr)
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
        n = http_sock_recv_buf(client_fd, req_buf + total, sizeof(req_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) { http_sock_close(client_fd); return; }

    /* 解析请求行 */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    char path_qs[2048];
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);
    strncpy(path_qs, path, sizeof(path_qs) - 1);
    path_qs[sizeof(path_qs) - 1] = '\0';
    {
        char *qm = strchr(path, '?');
        if (qm) *qm = '\0';
        qm = strchr(path, '#');
        if (qm) *qm = '\0';
    }

    LOG_INFO("request  %s:%d \"%s %s %s\"",
             client_ip, client_port, method, path, version);

    /* ── POST ──────────────────────────────────────────────────── */
    if (strcasecmp(method, "POST") == 0) {
        long content_length = 0;
        const char *cl = platform_strcasestr(req_buf, "\r\nContent-Length:");
        if (cl) {
            cl += strlen("\r\nContent-Length:");
            while (*cl == ' ') cl++;
            content_length = atol(cl);
        }

        long max_body_allowed = MAX_BODY_SIZE;
        if (strcmp(path, "/api/save-register-file") == 0 ||
            strcmp(path, "/api/wiki-save") == 0 ||
            strcmp(path, "/api/wiki-upload") == 0 ||
            strcmp(path, "/api/wiki-export-pdf") == 0)
            max_body_allowed = SAVE_REPORT_MAX_BODY;
        else if (strcmp(path, "/api/html-paste/save") == 0)
            max_body_allowed = MAX_HTML_PASTE_BODY;

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
                    n = http_sock_recv_buf(client_fd, body + rcvd,
                             (size_t)content_length - rcvd);
                    if (n <= 0) break;
                    rcvd += (size_t)n;
                }
            }
        }

        auth_user_t req_user;
        memset(&req_user, 0, sizeof(req_user));
        if (is_wiki_write_api(path)) {
            if (auth_require_author(req_buf, client_fd, &req_user) != 0) {
                auth_audit(client_ip, "", "denied", path, "guest write blocked");
                free(body);
                goto done;
            }
        }
        if (strcmp(path, "/api/wiki-user-save") == 0 ||
            strcmp(path, "/api/wiki-user-delete") == 0) {
            if (auth_require_admin(req_buf, client_fd, &req_user) != 0) {
                auth_audit(client_ip, "", "denied", path, "admin required");
                free(body);
                goto done;
            }
        }

        if (strcmp(path, "/api/html-paste/save") == 0) {
            if (body) handle_api_html_paste_save(client_fd, body);
            else send_json(client_fd, 413, "Payload Too Large",
                           "{\"ok\":false,\"error\":\"request body is too large or empty\"}", 61);
        } else if (strcmp(path, "/api/wiki-login") == 0) {
            if (body) handle_api_wiki_login(client_fd, req_buf, body, client_ip);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-logout") == 0) {
            handle_api_wiki_logout(client_fd, req_buf);
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
        } else if (strcmp(path, "/api/svn-log") == 0) {
            if (body) handle_api_svn_log(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-save") == 0) {
            if (body) {
                handle_api_wiki_save(client_fd, body, req_user.username, client_ip);
            }
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "wiki_delete", "", "");
                handle_api_wiki_delete(client_fd, body,
                                       req_user.username, client_ip);
            }
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
        } else if (strcmp(path, "/api/wiki-export-pdf") == 0) {
            if (body) handle_api_wiki_export_pdf(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-cleanup-uploads") == 0) {
            auth_audit(client_ip, req_user.username, "wiki_cleanup_uploads", "", "");
            handle_api_wiki_cleanup_uploads(client_fd);
        } else if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
            auth_audit(client_ip, req_user.username, "wiki_rebuild_html", "", "");
            handle_api_wiki_rebuild_html(client_fd);
        } else if (strcmp(path, "/api/wiki-trash-restore") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username,
                           "wiki_trash_restore", "", "");
                handle_api_wiki_trash_restore(client_fd, body);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-trash-empty") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username,
                           "wiki_trash_empty", "", "");
                handle_api_wiki_trash_empty(client_fd, body);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-restore-version") == 0) {
            if (body) {
                handle_api_wiki_restore_version(client_fd, body,
                                                 req_user.username, client_ip);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-user-save") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "user_save", "", "");
                handle_api_wiki_user_save(client_fd, body);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/wiki-user-delete") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "user_delete", "", "");
                handle_api_wiki_user_delete(client_fd, body);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/wiki-notewiki-prefs") == 0) {
            if (body)
                handle_api_wiki_notewiki_prefs_post(client_fd, req_buf, body);
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

    /* ── GET ───────────────────────────────────────────────────── */
    if (strcasecmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed",
                      "<h1>405 Method Not Allowed</h1>");
        goto done;
    }

    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden", "<h1>403 Forbidden</h1>");
        goto done;
    }

    if (strcmp(path, "/api/html-paste/list") == 0) {
        handle_api_html_paste_list(client_fd);
        goto done;
    }
    if (strcmp(path, "/api/html-paste/read") == 0) {
        handle_api_html_paste_read(client_fd, path_qs);
        goto done;
    }

    if (strcmp(path, "/api/gt-sdk-doc") == 0) {
        char cidir[512];
        snprintf(cidir, sizeof(cidir), "%s/wiki/ci_html", WEB_ROOT);
        DIR *d = opendir(cidir);
        if (!d) {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1><p>GT SDK document not found</p>");
            goto done;
        }
        char best[256] = {0};
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            size_t nml = strlen(nm);
            if (nml < 11) continue;  /* GT_SDK_ + .html = 11 min */
            if (strncmp(nm, "GT_SDK_", 7) != 0) continue;
            if (nml < 5 || strcasecmp(nm + nml - 5, ".html") != 0) continue;
            if (!best[0] || strcmp(nm, best) < 0)
                strncpy(best, nm, sizeof(best) - 1);
        }
        closedir(d);
        if (!best[0]) {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1><p>GT SDK document not found</p>");
            goto done;
        }
        char loc[640];
        snprintf(loc, sizeof(loc), "/wiki/ci_html/%s", best);
        send_redirect(client_fd, loc);
        goto done;
    }

    if (strcmp(path, "/api/wiki-list") == 0) {
        handle_api_wiki_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-trash-list") == 0) {
        handle_api_wiki_trash_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-notewiki-prefs") == 0) {
        handle_api_wiki_notewiki_prefs_get(client_fd, req_buf);
        goto done;
    }

    if (strcmp(path, "/api/wiki-auth-status") == 0) {
        handle_api_wiki_auth_status(client_fd, req_buf);
        goto done;
    }

    if (strcmp(path, "/api/wiki-users") == 0) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_users_list(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/wiki-audit-logs", 20) == 0 &&
        (path[20] == '\0' || path[20] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_audit_logs(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-md-history", 20) == 0 &&
        (path[20] == '\0' || path[20] == '?')) {
        char hist_id[256] = {0};
        query_param_get(path_qs, "id", hist_id, sizeof(hist_id));
        if (!hist_id[0]) {
            auth_user_t u;
            if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        }
        handle_api_wiki_md_history(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-user-article-rank", 27) == 0 &&
        (path[27] == '\0' || path[27] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_user_article_rank(client_fd, path_qs);
        goto done;
    }

    if (strcmp(path, "/api/wiki-refresh-index") == 0) {
        auth_user_t u;
        if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        auth_audit(client_ip, u.username, "wiki_refresh_index", "", "");
        handle_api_wiki_refresh_index(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
        auth_user_t u;
        if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        auth_audit(client_ip, u.username, "wiki_rebuild_html", "", "");
        handle_api_wiki_rebuild_html(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/wiki-export-md-zip", 23) == 0 &&
        (path[23] == '\0' || path[23] == '?')) {
        handle_api_wiki_export_md_zip(client_fd, path_qs);
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

    if (strcmp(path, "/api/codechecker-list") == 0) {
        handle_api_codechecker_list(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/admin-log-files", 20) == 0 &&
        (path[20] == '\0' || path[20] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_admin_log_files(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/admin-log-read", 19) == 0 &&
        (path[19] == '\0' || path[19] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_admin_log_read(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/admin-ip-stats", 19) == 0 &&
        (path[19] == '\0' || path[19] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_admin_ip_stats(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/admin-ip-host", 18) == 0 &&
        (path[18] == '\0' || path[18] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_admin_ip_host(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/admin-ip-logs", 18) == 0 &&
        (path[18] == '\0' || path[18] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_admin_ip_logs(client_fd, path_qs);
        goto done;
    }

    /* 浏览器默认请求 /favicon.ico；无 .ico 时回退到 html/favicon.svg，避免 404 */
    if (strcmp(path, "/favicon.ico") == 0) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/favicon.ico", WEB_ROOT);
        if (send_file(client_fd, fpath, req_buf) < 0) {
            snprintf(fpath, sizeof(fpath), "%s/favicon.svg", WEB_ROOT);
            if (send_file(client_fd, fpath, req_buf) < 0) {
                static const char nofav[] =
                    "HTTP/1.1 204 No Content\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                (void)http_sock_send_all(client_fd, nofav, sizeof(nofav) - 1);
            }
        }
        goto done;
    }

    {
        auth_user_t u;
        if (strncmp(path, "/wiki/", 6) == 0 && auth_resolve_user_from_headers(req_buf, &u) != 0) {
            auth_audit(client_ip, "guest", "guest_view", path, "");
        }
        char filepath[2048];
        char decoded_path[2048];
        strncpy(decoded_path, path, sizeof(decoded_path) - 1);
        decoded_path[sizeof(decoded_path) - 1] = '\0';
        url_decode_report_fn(decoded_path);
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, decoded_path);

        if (send_file(client_fd, filepath, req_buf) < 0) {
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

    http_sock_close(client_fd);
}
