#include "report_api.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>

/* ── 文件名净化 ───────────────────────────────────────────── */

static void sanitize_report_basename(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20) *p = '_';
    }
    while (name[0] == '.') memmove(name, name + 1, strlen(name));
    if (name[0] == '\0') { strncpy(name, "report.html", 256); name[255] = '\0'; return; }
    size_t len = strlen(name);
    if (len > 200) { name[200] = '\0'; len = 200; }
    if (len < 5 || strcasecmp(name + len - 5, ".html") != 0)
        if (len + 5 < 256) memcpy(name + len, ".html", 6);
}

static void sanitize_report_archive_name(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20) *p = '_';
    }
    while (name[0] == '.') memmove(name, name + 1, strlen(name));
    if (name[0] == '\0') return;
    if (strlen(name) > 200) name[200] = '\0';
}

static void sanitize_report_user_dir(char *name, size_t cap)
{
    char *slash;
    if (!name || cap < 4) return;
    if (!name[0]) { snprintf(name, cap, "root"); return; }
    slash = strrchr(name, '/');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    for (char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-') *p = '_';
    }
    while (name[0] == '.') memmove(name, name + 1, strlen(name) + 1);
    if (name[0] == '\0') { snprintf(name, cap, "root"); return; }
    if (strlen(name) > 63) name[63] = '\0';
}

static void sanitize_config_basename(char *name)
{
    char *slash = strrchr(name, '/');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(name, '\\');
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 0x20) *p = '_';
    }
    while (name[0] == '.') memmove(name, name + 1, strlen(name));
    if (name[0] == '\0') { strncpy(name, "ssh-config.json", 256); name[255] = '\0'; return; }
    size_t len = strlen(name);
    if (len > 200) name[200] = '\0';
    len = strlen(name);
    if (len < 6 || strcasecmp(name + len - 5, ".json") != 0)
        if (len + 6 < 256) memcpy(name + len, ".json", 6);
}

/* ── 验证函数 ─────────────────────────────────────────────── */

static int dir_name_is_yyyymm(const char *name)
{
    size_t i, n = strlen(name);
    if (n != 6) return 0;
    for (i = 0; i < n; i++)
        if (!isdigit((unsigned char)name[i])) return 0;
    return 1;
}

static int report_html_basename_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name) return 0;
    n = strlen(name);
    if (n < 6 || strcasecmp(name + n - 5, ".html") != 0) return 0;
    if (strstr(name, "..") != NULL) return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\') return 0;
    }
    return 1;
}

static int report_json_basename_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name) return 0;
    n = strlen(name);
    if (n < 6 || strcasecmp(name + n - 5, ".json") != 0) return 0;
    if (strstr(name, "..") != NULL) return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\') return 0;
    }
    return 1;
}

static int report_user_first_segment_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name || name[0] == '.') return 0;
    if (dir_name_is_yyyymm(name)) return 0;
    n = strlen(name);
    if (n > 80) return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-')) return 0;
    }
    return 1;
}

/* ── 创建目录 ─────────────────────────────────────────────── */

static int mkdir_report_legacy_ym(char *dirpath, size_t dcap, const char *yyyymm)
{
    char tmp[512];
    if (!yyyymm || strlen(yyyymm) != 6) return -1;
    snprintf(tmp, sizeof(tmp), "%s/report", WEB_ROOT);
    if (platform_mkdir(tmp) != 0 && errno != EEXIST) return -1;
    snprintf(dirpath, dcap, "%s/report/%s", WEB_ROOT, yyyymm);
    if (platform_mkdir(dirpath) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int mkdir_report_user_ym(char *dirpath, size_t dcap,
                                  const char *user_sanitized, const char *yyyymm)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/report", WEB_ROOT);
    if (platform_mkdir(tmp) != 0 && errno != EEXIST) return -1;
    snprintf(tmp, sizeof(tmp), "%s/report/%s", WEB_ROOT, user_sanitized);
    if (platform_mkdir(tmp) != 0 && errno != EEXIST) return -1;
    snprintf(dirpath, dcap, "%s/report/%s/%s", WEB_ROOT, user_sanitized, yyyymm);
    if (platform_mkdir(dirpath) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── POST /api/save-report ───────────────────────────────── */

void handle_api_save_report(http_sock_t client_fd, const char *req_headers,
                             const char *body, size_t body_len)
{
    char filename[256], userdir[128], kind_hdr[32], legacy_hdr[32], ym_hdr[16];
    int  is_json = 0, legacy_mode = 0;

    if (http_header_value(req_headers, "X-Report-Filename", filename,
                          sizeof(filename)) != 0 || filename[0] == '\0') {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Report-Filename\"}", 45); return;
    }
    url_decode_report_fn(filename);

    if (http_header_value(req_headers, "X-Report-Kind", kind_hdr,
                          sizeof(kind_hdr)) == 0 &&
        strcasecmp(kind_hdr, "json") == 0) is_json = 1;
    if (!is_json) {
        size_t fl = strlen(filename);
        if (fl >= 5 && strcasecmp(filename + fl - 5, ".json") == 0) is_json = 1;
    }

    if (is_json) {
        sanitize_report_archive_name(filename);
        if (filename[0] == '\0') { strncpy(filename, "upload.json", sizeof(filename)); filename[sizeof(filename)-1] = '\0'; }
        {
            size_t len = strlen(filename);
            if (len > 200) { filename[200] = '\0'; len = 200; }
            if (len < 6 || strcasecmp(filename + len - 5, ".json") != 0)
                if (len + 6 < sizeof(filename)) memcpy(filename + len, ".json", 6);
        }
        if (!report_json_basename_ok(filename)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid json filename\"}", 42); return;
        }
    } else {
        sanitize_report_basename(filename);
        if (!report_html_basename_ok(filename)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid html filename\"}", 42); return;
        }
    }

    if (http_header_value(req_headers, "X-Report-Legacy", legacy_hdr,
                          sizeof(legacy_hdr)) == 0 &&
        (legacy_hdr[0] == '1' || strcasecmp(legacy_hdr, "true") == 0))
        legacy_mode = 1;

    userdir[0] = '\0';
    if (!legacy_mode) {
        if (http_header_value(req_headers, "X-Report-User", userdir,
                              sizeof(userdir)) != 0 || userdir[0] == '\0')
            strncpy(userdir, "root", sizeof(userdir));
        else url_decode_report_fn(userdir);
        userdir[sizeof(userdir) - 1] = '\0';
        sanitize_report_user_dir(userdir, sizeof(userdir));
    }

    char yyyymm[16];
    if (http_header_value(req_headers, "X-Report-YYYYMM", ym_hdr,
                          sizeof(ym_hdr)) == 0 && ym_hdr[0] != '\0') {
        url_decode_report_fn(ym_hdr);
        if (!dir_name_is_yyyymm(ym_hdr)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid X-Report-YYYYMM\"}", 44); return;
        }
        strncpy(yyyymm, ym_hdr, sizeof(yyyymm)); yyyymm[sizeof(yyyymm)-1] = '\0';
    } else {
        time_t     now     = time(NULL);
        struct tm  tm_local;
        platform_localtime_wall(&now, &tm_local);
        if (strftime(yyyymm, sizeof(yyyymm), "%Y%m", &tm_local) == 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"strftime\"}", 32); return;
        }
    }

    char dirpath[512], prefix[512];
    if (legacy_mode) {
        if (mkdir_report_legacy_ym(dirpath, sizeof(dirpath), yyyymm) != 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"mkdir\"}", 30); return;
        }
        snprintf(prefix, sizeof(prefix), "%s/report/%s/", WEB_ROOT, yyyymm);
    } else {
        if (mkdir_report_user_ym(dirpath, sizeof(dirpath), userdir, yyyymm) != 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"mkdir\"}", 30); return;
        }
        snprintf(prefix, sizeof(prefix), "%s/report/%s/%s/", WEB_ROOT, userdir, yyyymm);
    }

    char filepath[640];
    int fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38); return;
    }
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

    char resp[768]; int rl;
    if (legacy_mode)
        rl = snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"report/%s/%s\"}", yyyymm, filename);
    else
        rl = snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"report/%s/%s/%s\"}", userdir, yyyymm, filename);
    if (rl < 0 || (size_t)rl >= sizeof(resp))
        send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    else
        send_json(client_fd, 200, "OK", resp, (size_t)rl);
    LOG_INFO("save_report  %s  (%zu bytes)", filepath, body_len);
}

/* ── POST /api/save-config ───────────────────────────────── */

void handle_api_save_config(http_sock_t client_fd, const char *req_headers,
                             const char *body, size_t body_len)
{
    char filename[256], userdir[128];

    if (http_header_value(req_headers, "X-Config-Filename", filename,
                          sizeof(filename)) != 0 || filename[0] == '\0') {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing X-Config-Filename\"}", 46); return;
    }
    url_decode_report_fn(filename);
    sanitize_config_basename(filename);

    if (http_header_value(req_headers, "X-Report-User", userdir,
                          sizeof(userdir)) != 0 || userdir[0] == '\0')
        strncpy(userdir, "root", sizeof(userdir));
    else url_decode_report_fn(userdir);
    userdir[sizeof(userdir) - 1] = '\0';
    sanitize_report_user_dir(userdir, sizeof(userdir));

    time_t     now = time(NULL);
    struct tm  tm_local;
    platform_localtime_wall(&now, &tm_local);
    char yyyymm[16];
    if (strftime(yyyymm, sizeof(yyyymm), "%Y%m", &tm_local) == 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"strftime\"}", 32); return;
    }

    char dirpath[512];
    if (mkdir_report_user_ym(dirpath, sizeof(dirpath), userdir, yyyymm) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir\"}", 30); return;
    }

    char filepath[640];
    int fn = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
    if (fn < 0 || (size_t)fn >= sizeof(filepath)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"path too long\"}", 38); return;
    }
    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/report/%s/%s/", WEB_ROOT, userdir, yyyymm);
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

    char resp[768];
    int rl = snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"report/%s/%s/%s\"}", userdir, yyyymm, filename);
    if (rl < 0 || (size_t)rl >= sizeof(resp))
        send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    else
        send_json(client_fd, 200, "OK", resp, (size_t)rl);
    LOG_INFO("save_config  %s  (%zu bytes)", filepath, body_len);
}

/* ── GET /api/reports ────────────────────────────────────── */

#define REPORTS_MAX_GROUPS     400
#define REPORTS_FILES_PER_GRP  200

typedef struct {
    char      name[256];
    char      kind[8];
    long long mtime;
    long long size;
} report_one_t;

typedef struct {
    char         user[96];
    int          legacy;
    char         ym[8];
    report_one_t files[REPORTS_FILES_PER_GRP];
    int          nf;
} report_grp_t;

static int cmp_report_one_desc(const void *a, const void *b)
{
    const report_one_t *x = (const report_one_t *)a;
    const report_one_t *y = (const report_one_t *)b;
    if (y->mtime > x->mtime) return 1;
    if (y->mtime < x->mtime) return -1;
    return strcmp(x->name, y->name);
}

static int cmp_report_grp_desc(const void *a, const void *b)
{
    const report_grp_t *x = (const report_grp_t *)a;
    const report_grp_t *y = (const report_grp_t *)b;
    int c = strcmp(y->ym, x->ym);
    if (c != 0) return c;
    if (x->legacy != y->legacy) return x->legacy - y->legacy;
    return strcmp(x->user, y->user);
}

static void scan_report_dir_archive_files(const char *sub, report_grp_t *g)
{
    DIR *sd = opendir(sub);
    struct dirent *de;
    if (!sd) return;
    while (g->nf < REPORTS_FILES_PER_GRP) {
        de = readdir(sd); if (!de) break;
        int is_html = report_html_basename_ok(de->d_name);
        int is_json = !is_html && report_json_basename_ok(de->d_name);
        if (!is_html && !is_json) continue;
        {
            char fp[768]; struct stat st;
            snprintf(fp, sizeof(fp), "%s/%s", sub, de->d_name);
            if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            strncpy(g->files[g->nf].name, de->d_name, sizeof(g->files[g->nf].name) - 1);
            g->files[g->nf].name[sizeof(g->files[g->nf].name) - 1] = '\0';
            strncpy(g->files[g->nf].kind, is_json ? "json" : "html", sizeof(g->files[g->nf].kind) - 1);
            g->files[g->nf].kind[sizeof(g->files[g->nf].kind) - 1] = '\0';
            g->files[g->nf].mtime = (long long)st.st_mtime;
            g->files[g->nf].size  = (long long)st.st_size;
            g->nf++;
        }
    }
    closedir(sd);
}

void handle_api_reports(http_sock_t client_fd)
{
    char base[384]; DIR *d; strbuf_t sb = {0};
    report_grp_t groups[REPORTS_MAX_GROUPS]; int ng = 0;
    struct dirent *de;

    snprintf(base, sizeof(base), "%s/report", WEB_ROOT);
    d = opendir(base);
    if (!d) { send_json(client_fd, 200, "OK", "{\"months\":[],\"groups\":[]}", 28); return; }

    while (ng < REPORTS_MAX_GROUPS) {
        de = readdir(d); if (!de) break;
        if (de->d_name[0] == '.') continue;
        if (!dir_name_is_yyyymm(de->d_name)) continue;
        {
            report_grp_t *g = &groups[ng]; char sub[512];
            memset(g, 0, sizeof(*g)); g->legacy = 1;
            strncpy(g->ym, de->d_name, 7); g->ym[7] = '\0';
            snprintf(sub, sizeof(sub), "%s/%s", base, de->d_name);
            scan_report_dir_archive_files(sub, g);
            if (g->nf > 0) ng++;
        }
    }
    rewinddir(d);

    while (ng < REPORTS_MAX_GROUPS) {
        de = readdir(d); if (!de) break;
        if (de->d_name[0] == '.') continue;
        if (!report_user_first_segment_ok(de->d_name)) continue;
        {
            char userbase[512]; DIR *ud;
            snprintf(userbase, sizeof(userbase), "%s/%s", base, de->d_name);
            ud = opendir(userbase); if (!ud) continue;
            while (ng < REPORTS_MAX_GROUPS) {
                struct dirent *ue = readdir(ud); if (!ue) break;
                if (ue->d_name[0] == '.') continue;
                if (!dir_name_is_yyyymm(ue->d_name)) continue;
                {
                    report_grp_t *g = &groups[ng]; char sub[640];
                    memset(g, 0, sizeof(*g)); g->legacy = 0;
                    strncpy(g->user, de->d_name, sizeof(g->user) - 1);
                    g->user[sizeof(g->user) - 1] = '\0';
                    strncpy(g->ym, ue->d_name, 7); g->ym[7] = '\0';
                    snprintf(sub, sizeof(sub), "%s/%s", userbase, ue->d_name);
                    scan_report_dir_archive_files(sub, g);
                    if (g->nf > 0) ng++;
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
            if (!first_g) SB_LIT(&sb, ",");
            first_g = 0;
            SB_LIT(&sb, "{\"legacy\":"); sb_appendf(&sb, "%s", g->legacy ? "true" : "false");
            SB_LIT(&sb, ",\"user\":"); sb_json_str(&sb, g->user);
            SB_LIT(&sb, ",\"ym\":\""); sb_append(&sb, g->ym, strlen(g->ym));
            SB_LIT(&sb, "\",\"files\":[");
            qsort(g->files, (size_t)g->nf, sizeof(g->files[0]), cmp_report_one_desc);
            for (fi = 0; fi < g->nf; fi++) {
                if (fi) SB_LIT(&sb, ",");
                SB_LIT(&sb, "{\"name\":"); sb_json_str(&sb, g->files[fi].name);
                SB_LIT(&sb, ",\"kind\":"); sb_json_str(&sb, g->files[fi].kind[0] ? g->files[fi].kind : "html");
                sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}", (long long)g->files[fi].mtime, (long long)g->files[fi].size);
            }
            SB_LIT(&sb, "]}");
        }
    }
    SB_LIT(&sb, "]}");

    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"months\":[],\"groups\":[]}", 28);
}

/* ── POST /api/delete-report ─────────────────────────────── */

void handle_api_delete_report(http_sock_t client_fd, const char *body)
{
    char name[256] = {0}, user[128] = {0}, ym[16] = {0}, filepath[800];
    int legacy = 0;

    if (!body || !body[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"empty body\"}", 35); return;
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
                  "{\"ok\":false,\"error\":\"missing name\"}", 37); return;
    }
    sanitize_report_archive_name(name);
    if (!report_html_basename_ok(name) && !report_json_basename_ok(name)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid filename\"}", 40); return;
    }
    if (json_get_str(body, "ym", ym, sizeof(ym)) < 0 || !dir_name_is_yyyymm(ym)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid ym\"}", 33); return;
    }

    if (legacy) {
        snprintf(filepath, sizeof(filepath), "%s/report/%s/%s", WEB_ROOT, ym, name);
    } else {
        if (json_get_str(body, "user", user, sizeof(user)) < 0 || !user[0]) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"missing user\"}", 37); return;
        }
        sanitize_report_user_dir(user, sizeof(user));
        snprintf(filepath, sizeof(filepath), "%s/report/%s/%s/%s", WEB_ROOT, user, ym, name);
    }

    {
        char prefix[400];
        snprintf(prefix, sizeof(prefix), "%s/report/", WEB_ROOT);
        if (strncmp(filepath, prefix, strlen(prefix)) != 0) {
            send_json(client_fd, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"path\"}", 28); return;
        }
    }

    if (unlink(filepath) != 0) {
        int code = (errno == ENOENT) ? 404 : 500;
        send_json(client_fd, code, code == 404 ? "Not Found" : "Internal Server Error",
                  "{\"ok\":false,\"error\":\"unlink failed\"}", 33); return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
}

/* ── GET /api/list-ssh-configs ───────────────────────────── */

void handle_api_list_ssh_configs(http_sock_t client_fd, const char *path)
{
    char userdir[128], ubase[512]; DIR *d; strbuf_t sb = {0};

    if (query_param_get(path, "user", userdir, sizeof(userdir)) != 0)
        strncpy(userdir, "root", sizeof(userdir));
    userdir[sizeof(userdir) - 1] = '\0';
    sanitize_report_user_dir(userdir, sizeof(userdir));

    snprintf(ubase, sizeof(ubase), "%s/report/%s", WEB_ROOT, userdir);
    SB_LIT(&sb, "{\"ok\":true,\"user\":"); sb_json_str(&sb, userdir);
    SB_LIT(&sb, ",\"files\":[");

    d = opendir(ubase);
    if (d) {
        int first = 1; struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char sub[640]; DIR *sd;
            if (de->d_name[0] == '.') continue;
            if (!dir_name_is_yyyymm(de->d_name)) continue;
            snprintf(sub, sizeof(sub), "%s/%s", ubase, de->d_name);
            sd = opendir(sub); if (!sd) continue;
            while (1) {
                struct dirent *fe = readdir(sd);
                if (!fe) break;
                if (!report_json_basename_ok(fe->d_name)) continue;
                char fp[768]; struct stat st;
                snprintf(fp, sizeof(fp), "%s/%s", sub, fe->d_name);
                if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                if (!first) SB_LIT(&sb, ",");
                first = 0;
                SB_LIT(&sb, "{\"ym\":\""); sb_append(&sb, de->d_name, strlen(de->d_name));
                SB_LIT(&sb, "\",\"name\":"); sb_json_str(&sb, fe->d_name);
                sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}", (long long)st.st_mtime, (long long)st.st_size);
            }
            closedir(sd);
        }
        closedir(d);
    }
    SB_LIT(&sb, "]}");

    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"user\":\"root\",\"files\":[]}", 40);
}

/* ── GET /api/list-all-configs ───────────────────────────── */

void handle_api_list_all_configs(http_sock_t client_fd)
{
    char rbase[512]; DIR *d; strbuf_t sb = {0};

    snprintf(rbase, sizeof(rbase), "%s/report", WEB_ROOT);
    SB_LIT(&sb, "{\"ok\":true,\"files\":[");

    d = opendir(rbase);
    if (d) {
        int first = 1; struct dirent *ue;
        while ((ue = readdir(d)) != NULL) {
            char udir[640]; DIR *ud; struct dirent *de;
            if (!report_user_first_segment_ok(ue->d_name)) continue;
            snprintf(udir, sizeof(udir), "%s/%s", rbase, ue->d_name);
            ud = opendir(udir); if (!ud) continue;
            while ((de = readdir(ud)) != NULL) {
                char sub[768]; DIR *sd; struct dirent *fe;
                if (!dir_name_is_yyyymm(de->d_name)) continue;
                snprintf(sub, sizeof(sub), "%s/%s", udir, de->d_name);
                sd = opendir(sub); if (!sd) continue;
                while ((fe = readdir(sd)) != NULL) {
                    char fp[896]; struct stat st;
                    if (!report_json_basename_ok(fe->d_name)) continue;
                    snprintf(fp, sizeof(fp), "%s/%s", sub, fe->d_name);
                    if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                    if (!first) SB_LIT(&sb, ",");
                    first = 0;
                    SB_LIT(&sb, "{\"user\":"); sb_json_str(&sb, ue->d_name);
                    SB_LIT(&sb, ",\"ym\":\""); sb_append(&sb, de->d_name, strlen(de->d_name));
                    SB_LIT(&sb, "\",\"name\":"); sb_json_str(&sb, fe->d_name);
                    sb_appendf(&sb, ",\"mtime\":%lld,\"size\":%lld}", (long long)st.st_mtime, (long long)st.st_size);
                }
                closedir(sd);
            }
            closedir(ud);
        }
        closedir(d);
    }
    SB_LIT(&sb, "]}");

    if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"files\":[]}", 22);
}
