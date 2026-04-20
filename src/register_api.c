#include "register_api.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>

/* ── 内部工具 ─────────────────────────────────────────────── */

static int register_filename_safe(const char *fn)
{
    if (!fn || !*fn || fn[0] == '.') return 0;
    if (strchr(fn, '/') || strchr(fn, '\\') || strchr(fn, ':')) return 0;
    size_t l = strlen(fn);
    return (l > 5 && strcasecmp(fn + l - 5, ".json") == 0) ||
           (l > 4 && strcasecmp(fn + l - 4, ".xml")  == 0);
}

static int register_relpath_safe(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '.') return 0;
    const char *slash = strrchr(p, '/');
    const char *fname = slash ? slash + 1 : p;
    if (!register_filename_safe(fname)) return 0;
    if (slash) {
        char dir[512]; size_t dlen = (size_t)(slash - p);
        if (dlen >= sizeof(dir)) return 0;
        memcpy(dir, p, dlen); dir[dlen] = '\0';
        return register_subdir_safe(dir);
    }
    return 1;
}

static int rmdir_r(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path) == 0 ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sub[1024]; snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir_r(sub);
        else unlink(sub);
    }
    closedir(d);
    return rmdir(path);
}

static void scan_register_dir(const char *dir_path, const char *rel_prefix,
                               strbuf_t *sb, int *first)
{
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st; if (stat(full, &st) != 0) continue;
        char rel[512];
        if (rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, de->d_name);
        else                snprintf(rel, sizeof(rel), "%s", de->d_name);
        if (S_ISDIR(st.st_mode)) { scan_register_dir(full, rel, sb, first); }
        else if (S_ISREG(st.st_mode)) {
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

static void scan_register_subdirs(const char *dir_path, const char *rel_prefix,
                                  strbuf_t *sb, int *first)
{
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st; if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char rel[512];
        if (rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, de->d_name);
        else                snprintf(rel, sizeof(rel), "%s", de->d_name);
        if (!*first) SB_LIT(sb, ",");
        *first = 0;
        sb_json_str(sb, rel);
        scan_register_subdirs(full, rel, sb, first);
    }
    closedir(d);
}

/* ── GET /api/list-register-files ────────────────────────── */

void handle_api_list_register_files(int client_fd)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"files\":[");
    int first = 1;
    scan_register_dir(WEB_ROOT "/register", "", &sb, &first);
    SB_LIT(&sb, "]}");
    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"files\":[]}", 22);
}

/* ── GET /api/list-register-dirs ─────────────────────────── */

void handle_api_list_register_dirs(int client_fd)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"dirs\":[");
    int first = 1;
    scan_register_subdirs(WEB_ROOT "/register", "", &sb, &first);
    SB_LIT(&sb, "]}");
    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"dirs\":[]}", 21);
}

/* ── POST /api/save-register-file ────────────────────────── */

void handle_api_save_register_file(int client_fd, const char *req_headers,
                                    const char *body, size_t body_len)
{
    char subdir[256] = "", filename[256] = "";

    http_header_value(req_headers, "X-Register-Subdir", subdir, sizeof(subdir));
    url_decode_report_fn(subdir);

    if (http_header_value(req_headers, "X-Register-Filename",
                          filename, sizeof(filename)) != 0 || !filename[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Register-Filename\"}", 51); return;
    }
    url_decode_report_fn(filename);

    if (!register_subdir_safe(subdir)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid subdir\"}", 38); return;
    }
    if (!register_filename_safe(filename)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid filename\"}", 40); return;
    }

    char dirpath[1024];
    if (subdir[0]) snprintf(dirpath, sizeof(dirpath), WEB_ROOT "/register/%s", subdir);
    else           snprintf(dirpath, sizeof(dirpath), WEB_ROOT "/register");

    if (mkdir_p(dirpath) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir\"}", 30); return;
    }

    char filepath[1280];
    int fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38); return;
    }
    char prefix[512];
    snprintf(prefix, sizeof(prefix), WEB_ROOT "/register/");
    if (strncmp(filepath, prefix, strlen(prefix)) != 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"bad path\"}", 33); return;
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"open file\"}", 35); return;
    }
    if (body_len > 0 && fwrite(body, 1, body_len, fp) != body_len) {
        fclose(fp); unlink(filepath);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write\"}", 30); return;
    }
    fclose(fp);

    char resp[640];
    const char *relpath = filepath + strlen(WEB_ROOT) + 1;
    int rl = snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\"}", relpath);
    send_json(client_fd, 200, "OK", resp, (size_t)(rl > 0 ? rl : 11));
    LOG_INFO("save_register  %s  (%zu bytes)", filepath, body_len);
}

/* ── POST /api/rename-register-dir ──────────────────────── */

void handle_api_rename_register_dir(int client_fd, const char *body)
{
    char from_rel[512] = "", to_rel[512] = "";
    if (json_get_str(body, "from", from_rel, sizeof(from_rel)) < 0 || !from_rel[0] ||
        json_get_str(body, "to",   to_rel,   sizeof(to_rel))   < 0 || !to_rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing from/to\"}", 40); return;
    }
    if (!register_subdir_safe(from_rel) || !register_subdir_safe(to_rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37); return;
    }
    char from_full[1024], to_full[1024];
    snprintf(from_full, sizeof(from_full), WEB_ROOT "/register/%s", from_rel);
    snprintf(to_full,   sizeof(to_full),   WEB_ROOT "/register/%s", to_rel);
    if (rename(from_full, to_full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err)); return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("rename_register_dir  %s -> %s", from_full, to_full);
}

/* ── POST /api/delete-register-dir ──────────────────────── */

void handle_api_delete_register_dir(int client_fd, const char *body)
{
    char rel[512] = "";
    if (json_get_str(body, "path", rel, sizeof(rel)) < 0 || !rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing path\"}", 37); return;
    }
    if (!register_subdir_safe(rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37); return;
    }
    char full[1024];
    snprintf(full, sizeof(full), WEB_ROOT "/register/%s", rel);
    if (rmdir_r(full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err)); return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("delete_register_dir  %s", full);
}

/* ── POST /api/rename-register-file ─────────────────────── */

void handle_api_rename_register_file(int client_fd, const char *body)
{
    char from_rel[512] = "", to_rel[512] = "";
    if (json_get_str(body, "from", from_rel, sizeof(from_rel)) < 0 || !from_rel[0] ||
        json_get_str(body, "to",   to_rel,   sizeof(to_rel))   < 0 || !to_rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing from/to\"}", 40); return;
    }
    if (!register_relpath_safe(from_rel) || !register_relpath_safe(to_rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37); return;
    }
    char from_full[1024], to_full[1024];
    snprintf(from_full, sizeof(from_full), WEB_ROOT "/register/%s", from_rel);
    snprintf(to_full,   sizeof(to_full),   WEB_ROOT "/register/%s", to_rel);

    char to_dir[1024]; snprintf(to_dir, sizeof(to_dir), "%s", to_full);
    char *slash = strrchr(to_dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(to_dir); }

    if (rename(from_full, to_full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err)); return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("rename_register  %s -> %s", from_full, to_full);
}

/* ── POST /api/delete-register-file ─────────────────────── */

void handle_api_delete_register_file(int client_fd, const char *body)
{
    char rel[512] = "";
    if (json_get_str(body, "path", rel, sizeof(rel)) < 0 || !rel[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing path\"}", 37); return;
    }
    if (!register_relpath_safe(rel)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid path\"}", 37); return;
    }
    char full[1024];
    snprintf(full, sizeof(full), WEB_ROOT "/register/%s", rel);
    if (unlink(full) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", strerror(errno));
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err)); return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("delete_register  %s", full);
}

/* ── GET /api/client-info ────────────────────────────────── */

void handle_api_client_info(int client_fd, const char *client_ip)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ip\":"); sb_json_str(&sb, client_ip ? client_ip : "");
    SB_LIT(&sb, "}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else         send_json(client_fd, 500, "Internal Server Error", "{\"ip\":\"\"}", 9);
    free(sb.data);
}
