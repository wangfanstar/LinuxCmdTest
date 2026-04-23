#include "auth_db.h"

#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#ifdef ENABLE_SQLITE3
#define AUTH_SQLITE_AVAILABLE 1
#include <sqlite3.h>
#else
#define AUTH_SQLITE_AVAILABLE 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include <pthread.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#define AUTH_DB_DIR   WEB_ROOT "/wiki/sqlite_db"
#define AUTH_DB_FILE  AUTH_DB_DIR "/wiki_auth.db"
#define AUTH_DB_CONFIG AUTH_DB_DIR "/db.config"
#define AUTH_DB_PENDING_LOG AUTH_DB_DIR "/pending_logs.jsonl"
#define SESSION_NAME  "WIKI_SESS"
#define SESSION_TTL_SEC (30 * 24 * 3600)

#if AUTH_SQLITE_AVAILABLE

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_auth_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_inited = 0;
static char g_admin_user[128] = "Admin";
static char g_admin_pass[128] = "123456";
static void auth_replay_pending_locked(void);

void auth_gen_save_txn_id(char *out, size_t outsz)
{
    static unsigned long long ctr = 0;
    ctr++;
    unsigned long long t = (unsigned long long)time(NULL);
    unsigned int r1 = (unsigned int)rand();
    unsigned int r2 = (unsigned int)rand();
    if (!out || outsz < 8) return;
    snprintf(out, outsz, "%08llx-%08x-%08x-%08llx", t, r1, r2, ctr);
}

static void trim_inplace(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static void load_admin_config(void)
{
    snprintf(g_admin_user, sizeof(g_admin_user), "%s", "Admin");
    snprintf(g_admin_pass, sizeof(g_admin_pass), "%s", "123456");

    FILE *fp = fopen(AUTH_DB_CONFIG, "rb");
    if (!fp) {
        FILE *wf = fopen(AUTH_DB_CONFIG, "wb");
        if (wf) {
            fputs("admin_username=Admin\nadmin_password=123456\n", wf);
            fclose(wf);
        }
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim_inplace(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        trim_inplace(k);
        trim_inplace(v);
        if (strcmp(k, "admin_username") == 0 && v[0]) {
            snprintf(g_admin_user, sizeof(g_admin_user), "%s", v);
        } else if (strcmp(k, "admin_password") == 0 && v[0]) {
            snprintf(g_admin_pass, sizeof(g_admin_pass), "%s", v);
        }
    }
    fclose(fp);
}

static void now_iso(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static int exec_sql(const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite exec failed: %s sql=%s", errmsg ? errmsg : "unknown", sql);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int exec_sql_allow_dup_column(const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) return 0;
    if (errmsg && strstr(errmsg, "duplicate column name")) {
        sqlite3_free(errmsg);
        return 0;
    }
    LOG_ERROR("sqlite exec failed: %s sql=%s", errmsg ? errmsg : "unknown", sql);
    sqlite3_free(errmsg);
    return -1;
}

static int ensure_schema(void)
{
    const char *sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT NOT NULL UNIQUE,"
        "display_name TEXT DEFAULT '',"
        "password TEXT NOT NULL,"
        "role TEXT NOT NULL,"
        "group_name TEXT DEFAULT '',"
        "enabled INTEGER NOT NULL DEFAULT 1,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ");";
    const char *sql_sessions =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "token TEXT PRIMARY KEY,"
        "user_id INTEGER NOT NULL,"
        "username TEXT NOT NULL,"
        "role TEXT NOT NULL,"
        "ip TEXT DEFAULT '',"
        "created_at TEXT NOT NULL,"
        "last_seen_at TEXT NOT NULL,"
        "expires_at INTEGER NOT NULL"
        ");";
    const char *sql_audit =
        "CREATE TABLE IF NOT EXISTS audit_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts TEXT NOT NULL,"
        "ip TEXT DEFAULT '',"
        "username TEXT DEFAULT '',"
        "action TEXT NOT NULL,"
        "target TEXT DEFAULT '',"
        "detail TEXT DEFAULT ''"
        ");";
    const char *sql_login =
        "CREATE TABLE IF NOT EXISTS login_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts TEXT NOT NULL,"
        "ip TEXT DEFAULT '',"
        "username TEXT DEFAULT '',"
        "ok INTEGER NOT NULL DEFAULT 0,"
        "note TEXT DEFAULT ''"
        ");";
    const char *sql_md =
        "CREATE TABLE IF NOT EXISTS md_backups ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "article_id TEXT NOT NULL,"
        "save_txn_id TEXT DEFAULT '',"
        "title TEXT DEFAULT '',"
        "category TEXT DEFAULT '',"
        "content TEXT NOT NULL,"
        "html TEXT DEFAULT '',"
        "editor TEXT DEFAULT '',"
        "ip TEXT DEFAULT '',"
        "created_at TEXT NOT NULL"
        ");";
    const char *sql_idx1 = "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_logs(ts DESC);";
    const char *sql_idx2 = "CREATE INDEX IF NOT EXISTS idx_md_article ON md_backups(article_id, id DESC);";
    const char *sql_alt1 = "ALTER TABLE users ADD COLUMN display_name TEXT DEFAULT '';";
    const char *sql_alt2 = "ALTER TABLE md_backups ADD COLUMN save_txn_id TEXT DEFAULT '';";

    if (exec_sql(sql_users)) return -1;
    if (exec_sql_allow_dup_column(sql_alt1)) return -1;
    if (exec_sql_allow_dup_column(sql_alt2)) return -1;
    if (exec_sql(sql_sessions)) return -1;
    if (exec_sql(sql_audit)) return -1;
    if (exec_sql(sql_login)) return -1;
    if (exec_sql(sql_md)) return -1;
    if (exec_sql(sql_idx1)) return -1;
    if (exec_sql(sql_idx2)) return -1;
    return 0;
}

static int ensure_default_admin(void)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "UPDATE users SET password=?1,role='admin',enabled=1,updated_at=datetime('now','localtime') "
        "WHERE username=?2;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, g_admin_pass, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, g_admin_user, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    if (sqlite3_changes(g_db) > 0) return 0;

    rc = sqlite3_prepare_v2(g_db,
        "INSERT INTO users(username,display_name,password,role,group_name,enabled,created_at,updated_at) "
        "VALUES(?1,'Administrator',?2,'admin','default',1,datetime('now','localtime'),datetime('now','localtime'));",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, g_admin_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, g_admin_pass, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int auth_db_init(void)
{
    pthread_mutex_lock(&g_auth_mu);
    if (g_inited) {
        pthread_mutex_unlock(&g_auth_mu);
        return 0;
    }
    if (mkdir_p(AUTH_DB_DIR) != 0) {
        pthread_mutex_unlock(&g_auth_mu);
        return -1;
    }
    load_admin_config();
    if (sqlite3_open(AUTH_DB_FILE, &g_db) != SQLITE_OK) {
        LOG_ERROR("sqlite3_open failed: %s", sqlite3_errmsg(g_db));
        if (g_db) sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_auth_mu);
        return -1;
    }
    sqlite3_busy_timeout(g_db, 5000);
    if (ensure_schema() != 0 || ensure_default_admin() != 0) {
        sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_auth_mu);
        return -1;
    }
    auth_replay_pending_locked();
    g_inited = 1;
    pthread_mutex_unlock(&g_auth_mu);
    return 0;
}

void auth_db_close(void)
{
    pthread_mutex_lock(&g_auth_mu);
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
    g_inited = 0;
    pthread_mutex_unlock(&g_auth_mu);
}

static int get_cookie_token(const char *req_headers, char *out, size_t outsz)
{
    char cookie[4096];
    if (http_header_value(req_headers, "Cookie", cookie, sizeof(cookie)) != 0) return -1;
    const char *p = cookie;
    size_t name_len = strlen(SESSION_NAME);
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (strncmp(p, SESSION_NAME, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t i = 0;
            while (*p && *p != ';' && i + 1 < outsz) out[i++] = *p++;
            out[i] = '\0';
            return (i > 0) ? 0 : -1;
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return -1;
}

static void session_token_gen(char *out, size_t cap, const char *seed)
{
    static unsigned long long ctr = 0;
    ctr++;
    unsigned int r = (unsigned int)rand();
    unsigned int r2 = (unsigned int)rand();
    unsigned long long t = (unsigned long long)time(NULL);
    unsigned long long h = (unsigned long long)(uintptr_t)seed;
    snprintf(out, cap, "%llx%08x%08x%llx%llx",
             t, r, r2, h, ctr);
}

static void send_json_ex(http_sock_t fd, int status, const char *status_text,
                         const char *json, const char *extra_headers)
{
    size_t json_len = strlen(json);
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, json_len, extra_headers ? extra_headers : "");
    (void)http_sock_send_all(fd, header, (size_t)hlen);
    (void)http_sock_send_all(fd, json, json_len);
}

static char *json_get_str_alloc_local(const char *json, const char *key)
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
    while (*p && *p != '"' && i < len) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

static int role_is_author_or_admin(const char *role)
{
    return role && (!strcmp(role, "admin") || !strcmp(role, "author"));
}

static int sqlite_insert_audit_locked(const char *ip, const char *username, const char *action,
                                      const char *target, const char *detail, const char *save_txn_id)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO audit_logs(ts,ip,username,action,target,detail) VALUES(?1,?2,?3,?4,?5,?6);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    char ts[32];
    now_iso(ts, sizeof(ts));
    char detail_buf[1024];
    if (save_txn_id && save_txn_id[0]) {
        snprintf(detail_buf, sizeof(detail_buf), "%s%s%s",
                 detail ? detail : "", (detail && detail[0]) ? " | save_txn_id=" : "save_txn_id=", save_txn_id);
    } else {
        snprintf(detail_buf, sizeof(detail_buf), "%s", detail ? detail : "");
    }
    sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, ip ? ip : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, username ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, action ? action : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, target ? target : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, detail_buf, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static int sqlite_insert_md_backup_locked(const char *article_id, const char *title, const char *category,
                                          const char *content, const char *html, const char *editor,
                                          const char *ip, const char *save_txn_id)
{
    if (!article_id || !content) return -1;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO md_backups(article_id,save_txn_id,title,category,content,html,editor,ip,created_at)"
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    char ts[32];
    now_iso(ts, sizeof(ts));
    sqlite3_bind_text(st, 1, article_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, save_txn_id ? save_txn_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, category ? category : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, html ? html : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, editor ? editor : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, ip ? ip : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, ts, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static void append_pending_jsonl(const char *line)
{
    if (!line || !line[0]) return;
    FILE *fp = fopen(AUTH_DB_PENDING_LOG, "ab");
    if (!fp) return;
    fwrite(line, 1, strlen(line), fp);
    fwrite("\n", 1, 1, fp);
    fclose(fp);
}

static void queue_pending_audit(const char *ip, const char *username, const char *action,
                                const char *target, const char *detail, const char *save_txn_id)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"type\":\"audit\",\"ip\":"); sb_json_str(&sb, ip ? ip : "");
    SB_LIT(&sb, ",\"username\":"); sb_json_str(&sb, username ? username : "");
    SB_LIT(&sb, ",\"action\":"); sb_json_str(&sb, action ? action : "");
    SB_LIT(&sb, ",\"target\":"); sb_json_str(&sb, target ? target : "");
    SB_LIT(&sb, ",\"detail\":"); sb_json_str(&sb, detail ? detail : "");
    SB_LIT(&sb, ",\"save_txn_id\":"); sb_json_str(&sb, save_txn_id ? save_txn_id : "");
    SB_LIT(&sb, "}");
    if (sb.data) append_pending_jsonl(sb.data);
    free(sb.data);
}

static void queue_pending_md_backup(const char *article_id, const char *title, const char *category,
                                    const char *content, const char *html, const char *editor,
                                    const char *ip, const char *save_txn_id)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"type\":\"md_backup\",\"article_id\":"); sb_json_str(&sb, article_id ? article_id : "");
    SB_LIT(&sb, ",\"title\":"); sb_json_str(&sb, title ? title : "");
    SB_LIT(&sb, ",\"category\":"); sb_json_str(&sb, category ? category : "");
    SB_LIT(&sb, ",\"content\":"); sb_json_str(&sb, content ? content : "");
    SB_LIT(&sb, ",\"html\":"); sb_json_str(&sb, html ? html : "");
    SB_LIT(&sb, ",\"editor\":"); sb_json_str(&sb, editor ? editor : "");
    SB_LIT(&sb, ",\"ip\":"); sb_json_str(&sb, ip ? ip : "");
    SB_LIT(&sb, ",\"save_txn_id\":"); sb_json_str(&sb, save_txn_id ? save_txn_id : "");
    SB_LIT(&sb, "}");
    if (sb.data) append_pending_jsonl(sb.data);
    free(sb.data);
}

static void auth_replay_pending_locked(void)
{
    FILE *in = fopen(AUTH_DB_PENDING_LOG, "rb");
    if (!in) return;
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", AUTH_DB_PENDING_LOG);
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return;
    }

    int replayed = 0;
    int kept = 0;
    strbuf_t linebuf = {0};
    int ch = 0;
    while ((ch = fgetc(in)) != EOF) {
        if (ch != '\n') {
            char c = (char)ch;
            sb_append(&linebuf, &c, 1);
            continue;
        }
        if (!linebuf.data || linebuf.len == 0) continue;
        size_t n = linebuf.len;
        while (n > 0 && (linebuf.data[n - 1] == '\n' || linebuf.data[n - 1] == '\r')) n--;
        linebuf.data[n] = '\0';
        if (n == 0) { linebuf.len = 0; continue; }
        char *line = linebuf.data;
        char type[32] = {0};
        int ok = 0;
        json_get_str(line, "type", type, sizeof(type));
        if (strcmp(type, "audit") == 0) {
            char ip[64] = {0}, username[64] = {0}, action[64] = {0}, target[256] = {0}, detail[512] = {0}, save_txn_id[128] = {0};
            json_get_str(line, "ip", ip, sizeof(ip));
            json_get_str(line, "username", username, sizeof(username));
            json_get_str(line, "action", action, sizeof(action));
            json_get_str(line, "target", target, sizeof(target));
            json_get_str(line, "detail", detail, sizeof(detail));
            json_get_str(line, "save_txn_id", save_txn_id, sizeof(save_txn_id));
            ok = (sqlite_insert_audit_locked(ip, username, action, target, detail, save_txn_id) == 0);
        } else if (strcmp(type, "md_backup") == 0) {
            char article_id[256] = {0}, title[512] = {0}, category[512] = {0}, editor[128] = {0}, ip[64] = {0}, save_txn_id[128] = {0};
            char *content = json_get_str_alloc_local(line, "content");
            char *html = json_get_str_alloc_local(line, "html");
            json_get_str(line, "article_id", article_id, sizeof(article_id));
            json_get_str(line, "title", title, sizeof(title));
            json_get_str(line, "category", category, sizeof(category));
            json_get_str(line, "editor", editor, sizeof(editor));
            json_get_str(line, "ip", ip, sizeof(ip));
            json_get_str(line, "save_txn_id", save_txn_id, sizeof(save_txn_id));
            ok = (content && sqlite_insert_md_backup_locked(article_id, title, category, content, html ? html : "", editor, ip, save_txn_id) == 0);
            free(content);
            free(html);
        }
        if (ok) replayed++;
        else {
            fwrite(line, 1, n, out);
            fwrite("\n", 1, 1, out);
            kept++;
        }
        linebuf.len = 0;
    }
    if (linebuf.data && linebuf.len > 0) {
        size_t n = linebuf.len;
        while (n > 0 && (linebuf.data[n - 1] == '\n' || linebuf.data[n - 1] == '\r')) n--;
        linebuf.data[n] = '\0';
        if (n > 0) {
            fwrite(linebuf.data, 1, n, out);
            fwrite("\n", 1, 1, out);
            kept++;
        }
    }
    free(linebuf.data);

    fclose(in);
    fclose(out);
    if (kept == 0) {
        remove(AUTH_DB_PENDING_LOG);
        remove(tmp_path);
    } else {
        remove(AUTH_DB_PENDING_LOG);
        rename(tmp_path, AUTH_DB_PENDING_LOG);
    }
    if (replayed > 0) {
        LOG_INFO("auth pending replayed=%d remain=%d", replayed, kept);
    }
}

static int resolve_user_by_token(const char *token, auth_user_t *out_user)
{
    if (!token || !*token || !out_user) return -1;
    memset(out_user, 0, sizeof(*out_user));

    const char *sql =
        "SELECT user_id,username,role,"
        "(SELECT group_name FROM users WHERE users.id=sessions.user_id),"
        "(SELECT display_name FROM users WHERE users.id=sessions.user_id) "
        "FROM sessions WHERE token=?1 AND expires_at>?2 LIMIT 1;";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out_user->logged_in = 1;
        out_user->user_id = sqlite3_column_int(st, 0);
        snprintf(out_user->username, sizeof(out_user->username), "%s",
                 (const char *)sqlite3_column_text(st, 1));
        snprintf(out_user->role, sizeof(out_user->role), "%s",
                 (const char *)sqlite3_column_text(st, 2));
        const unsigned char *g = sqlite3_column_text(st, 3);
        snprintf(out_user->group_name, sizeof(out_user->group_name), "%s", g ? (const char *)g : "");
        const unsigned char *dn = sqlite3_column_text(st, 4);
        snprintf(out_user->display_name, sizeof(out_user->display_name), "%s", dn ? (const char *)dn : "");
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return -1;
}

int auth_resolve_user_from_headers(const char *req_headers, auth_user_t *out_user)
{
    if (auth_db_init() != 0) return -1;
    memset(out_user, 0, sizeof(*out_user));
    char token[256] = {0};
    if (get_cookie_token(req_headers, token, sizeof(token)) != 0) return -1;
    pthread_mutex_lock(&g_auth_mu);
    int rc = resolve_user_by_token(token, out_user);
    pthread_mutex_unlock(&g_auth_mu);
    return rc;
}

int auth_require_author(const char *req_headers, http_sock_t fd, auth_user_t *out_user)
{
    auth_user_t u;
    if (auth_resolve_user_from_headers(req_headers, &u) != 0) {
        send_json(fd, 401, "Unauthorized",
                  "{\"ok\":false,\"error\":\"login required\",\"needLogin\":true}", 55);
        return -1;
    }
    if (!role_is_author_or_admin(u.role)) {
        send_json(fd, 403, "Forbidden",
                  "{\"ok\":false,\"error\":\"author or admin required\"}", 47);
        return -1;
    }
    if (out_user) *out_user = u;
    return 0;
}

int auth_require_admin(const char *req_headers, http_sock_t fd, auth_user_t *out_user)
{
    auth_user_t u;
    if (auth_resolve_user_from_headers(req_headers, &u) != 0 || strcmp(u.role, "admin") != 0) {
        send_json(fd, 403, "Forbidden",
                  "{\"ok\":false,\"error\":\"admin required\"}", 37);
        return -1;
    }
    if (out_user) *out_user = u;
    return 0;
}

void auth_audit_txn(const char *ip, const char *username, const char *action,
                    const char *target, const char *detail, const char *save_txn_id)
{
    if (auth_db_init() != 0) {
        queue_pending_audit(ip, username, action, target, detail, save_txn_id);
        return;
    }
    pthread_mutex_lock(&g_auth_mu);
    auth_replay_pending_locked();
    int ok = sqlite_insert_audit_locked(ip, username, action, target, detail, save_txn_id);
    pthread_mutex_unlock(&g_auth_mu);
    if (ok != 0) {
        LOG_WARN("auth_audit sqlite failed, queued action=%s", action ? action : "");
        queue_pending_audit(ip, username, action, target, detail, save_txn_id);
    }
}

void auth_audit(const char *ip, const char *username, const char *action,
                const char *target, const char *detail)
{
    auth_audit_txn(ip, username, action, target, detail, NULL);
}

void auth_md_backup_txn(const char *article_id, const char *title, const char *category,
                        const char *content, const char *html, const char *editor,
                        const char *ip, const char *save_txn_id)
{
    if (!article_id || !content) return;
    if (auth_db_init() != 0) {
        queue_pending_md_backup(article_id, title, category, content, html, editor, ip, save_txn_id);
        return;
    }
    pthread_mutex_lock(&g_auth_mu);
    auth_replay_pending_locked();
    int ok = sqlite_insert_md_backup_locked(article_id, title, category, content, html, editor, ip, save_txn_id);
    pthread_mutex_unlock(&g_auth_mu);
    if (ok != 0) {
        LOG_WARN("auth_md_backup sqlite failed, queued article=%s", article_id);
        queue_pending_md_backup(article_id, title, category, content, html, editor, ip, save_txn_id);
    }
}

void auth_md_backup(const char *article_id, const char *title, const char *category,
                    const char *content, const char *html, const char *editor,
                    const char *ip)
{
    auth_md_backup_txn(article_id, title, category, content, html, editor, ip, NULL);
}

static void log_login(const char *ip, const char *username, int ok, const char *note)
{
    if (auth_db_init() != 0) return;
    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO login_logs(ts,ip,username,ok,note) VALUES(?1,?2,?3,?4,?5);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        char ts[32];
        now_iso(ts, sizeof(ts));
        sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, ip ? ip : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, username ? username : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, ok ? 1 : 0);
        sqlite3_bind_text(st, 5, note ? note : "", -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);
}

void handle_api_wiki_login(http_sock_t fd, const char *req_headers, const char *body, const char *ip)
{
    (void)req_headers;
    if (auth_db_init() != 0) {
        send_json(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"sqlite init failed\"}", 41);
        return;
    }
    char username[64] = {0}, password[128] = {0};
    if (json_get_str(body, "username", username, sizeof(username)) != 0 ||
        json_get_str(body, "password", password, sizeof(password)) != 0 ||
        username[0] == '\0') {
        send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"username/password required\"}", 49);
        return;
    }

    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id,password,role,group_name,enabled,display_name FROM users WHERE username=?1 LIMIT 1;";
    int ok = 0;
    int uid = 0;
    char role[16] = {0};
    char group_name[64] = {0};
    char display_name[128] = {0};
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *dbpass = (const char *)sqlite3_column_text(st, 1);
            int enabled = sqlite3_column_int(st, 4);
            if (enabled && dbpass && strcmp(dbpass, password) == 0) {
                ok = 1;
                uid = sqlite3_column_int(st, 0);
                snprintf(role, sizeof(role), "%s", (const char *)sqlite3_column_text(st, 2));
                const unsigned char *g = sqlite3_column_text(st, 3);
                snprintf(group_name, sizeof(group_name), "%s", g ? (const char *)g : "");
                const unsigned char *dn = sqlite3_column_text(st, 5);
                snprintf(display_name, sizeof(display_name), "%s", dn ? (const char *)dn : "");
            }
        }
    }
    if (st) sqlite3_finalize(st);

    if (!ok) {
        pthread_mutex_unlock(&g_auth_mu);
        log_login(ip, username, 0, "invalid username or password");
        send_json(fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"invalid username or password\"}", 51);
        return;
    }

    char token[128];
    session_token_gen(token, sizeof(token), username);
    time_t now = time(NULL);
    time_t exp = now + SESSION_TTL_SEC;
    const char *ins = "INSERT OR REPLACE INTO sessions(token,user_id,username,role,ip,created_at,last_seen_at,expires_at)"
                      "VALUES(?1,?2,?3,?4,?5,datetime('now','localtime'),datetime('now','localtime'),?6);";
    st = NULL;
    if (sqlite3_prepare_v2(g_db, ins, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, uid);
        sqlite3_bind_text(st, 3, username, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, role, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, ip ? ip : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)exp);
        (void)sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);

    log_login(ip, username, 1, "login success");
    auth_audit(ip, username, "login", "", "");

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"user\":{");
    SB_LIT(&sb, "\"username\":"); sb_json_str(&sb, username); SB_LIT(&sb, ",");
    SB_LIT(&sb, "\"displayName\":"); sb_json_str(&sb, display_name); SB_LIT(&sb, ",");
    SB_LIT(&sb, "\"role\":"); sb_json_str(&sb, role); SB_LIT(&sb, ",");
    SB_LIT(&sb, "\"group\":"); sb_json_str(&sb, group_name);
    SB_LIT(&sb, "}}");

    char extra[512];
    snprintf(extra, sizeof(extra),
             "Set-Cookie: %s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d\r\n",
             SESSION_NAME, token, SESSION_TTL_SEC);
    send_json_ex(fd, 200, "OK", sb.data ? sb.data : "{\"ok\":true}", extra);
    free(sb.data);
}

void handle_api_wiki_logout(http_sock_t fd, const char *req_headers)
{
    char token[256] = {0};
    if (auth_db_init() == 0 && get_cookie_token(req_headers, token, sizeof(token)) == 0) {
        pthread_mutex_lock(&g_auth_mu);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE token=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        pthread_mutex_unlock(&g_auth_mu);
    }
    send_json_ex(fd, 200, "OK", "{\"ok\":true}",
                 "Set-Cookie: " SESSION_NAME "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n");
}

void handle_api_wiki_auth_status(http_sock_t fd, const char *req_headers)
{
    auth_user_t u;
    if (auth_resolve_user_from_headers(req_headers, &u) == 0 && u.logged_in) {
        strbuf_t sb = {0};
        SB_LIT(&sb, "{\"ok\":true,\"loggedIn\":true,\"user\":{");
        SB_LIT(&sb, "\"id\":"); sb_appendf(&sb, "%d,", u.user_id);
        SB_LIT(&sb, "\"username\":"); sb_json_str(&sb, u.username); SB_LIT(&sb, ",");
        SB_LIT(&sb, "\"displayName\":"); sb_json_str(&sb, u.display_name); SB_LIT(&sb, ",");
        SB_LIT(&sb, "\"role\":"); sb_json_str(&sb, u.role); SB_LIT(&sb, ",");
        SB_LIT(&sb, "\"group\":"); sb_json_str(&sb, u.group_name);
        SB_LIT(&sb, "}}");
        send_json(fd, 200, "OK", sb.data, sb.len);
        free(sb.data);
        return;
    }
    send_json(fd, 200, "OK", "{\"ok\":true,\"loggedIn\":false}", 28);
}

void handle_api_wiki_users_list(http_sock_t fd)
{
    if (auth_db_init() != 0) {
        send_json(fd, 500, "Internal Server Error", "{\"ok\":false}", 12);
        return;
    }
    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"users\":[");
    if (sqlite3_prepare_v2(g_db, "SELECT id,username,display_name,role,group_name,enabled,created_at,updated_at FROM users WHERE username<>?1 ORDER BY id ASC;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, g_admin_user, -1, SQLITE_TRANSIENT);
        int first = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) SB_LIT(&sb, ",");
            first = 0;
            SB_LIT(&sb, "{");
            sb_appendf(&sb, "\"id\":%d,", sqlite3_column_int(st, 0));
            SB_LIT(&sb, "\"username\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 1)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"displayName\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 2)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"role\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 3)); SB_LIT(&sb, ",");
            const unsigned char *g = sqlite3_column_text(st, 4);
            SB_LIT(&sb, "\"group\":"); sb_json_str(&sb, g ? (const char *)g : ""); SB_LIT(&sb, ",");
            sb_appendf(&sb, "\"enabled\":%d,", sqlite3_column_int(st, 5));
            SB_LIT(&sb, "\"createdAt\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 6)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"updatedAt\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 7));
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);
    SB_LIT(&sb, "]}");
    send_json(fd, 200, "OK", sb.data ? sb.data : "{\"ok\":true,\"users\":[]}",
              sb.data ? sb.len : 22);
    free(sb.data);
}

void handle_api_wiki_user_save(http_sock_t fd, const char *body)
{
    if (auth_db_init() != 0) {
        send_json(fd, 500, "Internal Server Error", "{\"ok\":false}", 12);
        return;
    }
    char username[128] = {0}, display_name[128] = {0}, password[128] = {0}, role[16] = {0}, group_name[64] = {0};
    char account[128] = {0}, origin_account[128] = {0};
    int enabled = json_get_int(body, "enabled", 1);
    int id = json_get_int(body, "id", 0);
    json_get_str(body, "username", username, sizeof(username));
    json_get_str(body, "account", account, sizeof(account));
    json_get_str(body, "origin_account", origin_account, sizeof(origin_account));
    json_get_str(body, "password", password, sizeof(password));
    json_get_str(body, "display_name", display_name, sizeof(display_name));
    json_get_str(body, "role", role, sizeof(role));
    json_get_str(body, "group", group_name, sizeof(group_name));
    if (!username[0] && account[0]) snprintf(username, sizeof(username), "%s", account);
    if (role[0] == '\0') snprintf(role, sizeof(role), "author");
    if (group_name[0] == '\0') snprintf(group_name, sizeof(group_name), "default");
    if (strcmp(role, "admin") != 0 &&
        strcmp(role, "author") != 0 &&
        strcmp(role, "guest") != 0) {
        send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"invalid role\"}", 35);
        return;
    }

    pthread_mutex_lock(&g_auth_mu);
    int ok = 1;
    sqlite3_stmt *st = NULL;
    if (origin_account[0]) {
        if (username[0] && strcmp(username, origin_account) != 0) {
            pthread_mutex_unlock(&g_auth_mu);
            send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"account cannot be changed\"}", 51);
            return;
        }
        if (!username[0]) snprintf(username, sizeof(username), "%s", origin_account);
        if (strcmp(origin_account, g_admin_user) == 0 || strcmp(username, g_admin_user) == 0) {
            snprintf(role, sizeof(role), "%s", "admin");
            enabled = 1;
        }
        if (password[0]) {
            const char *sql = "UPDATE users SET username=?1,display_name=?2,password=?3,role=?4,group_name=?5,enabled=?6,updated_at=datetime('now','localtime') WHERE username=?7;";
            if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, display_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, password, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, role, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, group_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 6, enabled ? 1 : 0);
                sqlite3_bind_text(st, 7, origin_account, -1, SQLITE_TRANSIENT);
                ok = (sqlite3_step(st) == SQLITE_DONE);
            } else ok = 0;
        } else {
            const char *sql = "UPDATE users SET username=?1,display_name=?2,role=?3,group_name=?4,enabled=?5,updated_at=datetime('now','localtime') WHERE username=?6;";
            if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, display_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, role, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, group_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 5, enabled ? 1 : 0);
                sqlite3_bind_text(st, 6, origin_account, -1, SQLITE_TRANSIENT);
                ok = (sqlite3_step(st) == SQLITE_DONE);
            } else ok = 0;
        }
    } else if (id > 0) {
        if (strcmp(username, g_admin_user) == 0) {
            snprintf(role, sizeof(role), "%s", "admin");
            enabled = 1;
        }
        if (password[0]) {
            const char *sql = "UPDATE users SET username=?1,password=?2,role=?3,group_name=?4,enabled=?5,updated_at=datetime('now','localtime') WHERE id=?6;";
            if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, password, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, role, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, group_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 5, enabled ? 1 : 0);
                sqlite3_bind_int(st, 6, id);
                ok = (sqlite3_step(st) == SQLITE_DONE);
            } else ok = 0;
        } else {
            const char *sql = "UPDATE users SET username=?1,role=?2,group_name=?3,enabled=?4,updated_at=datetime('now','localtime') WHERE id=?5;";
            if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, role, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, group_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 4, enabled ? 1 : 0);
                sqlite3_bind_int(st, 5, id);
                ok = (sqlite3_step(st) == SQLITE_DONE);
            } else ok = 0;
        }
    } else {
        if (username[0] == '\0' || password[0] == '\0') ok = 0;
        else {
            const char *sql = "INSERT INTO users(username,display_name,password,role,group_name,enabled,created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?6,datetime('now','localtime'),datetime('now','localtime'));";
            if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, display_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, password, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, role, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, group_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 6, enabled ? 1 : 0);
                ok = (sqlite3_step(st) == SQLITE_DONE);
            } else ok = 0;
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);

    if (!ok) {
        send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"save user failed\"}", 39);
        return;
    }
    send_json(fd, 200, "OK", "{\"ok\":true}", 11);
}

void handle_api_wiki_user_delete(http_sock_t fd, const char *body)
{
    char account[128] = {0};
    json_get_str(body, "account", account, sizeof(account));
    if (!account[0]) json_get_str(body, "username", account, sizeof(account));
    int id = json_get_int(body, "id", 0);
    if (id <= 0 && !account[0]) {
        send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"account required\"}", 40);
        return;
    }
    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (account[0]) {
        if (sqlite3_prepare_v2(g_db, "DELETE FROM users WHERE username=?1 AND username<>?2;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, account, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, g_admin_user, -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(st) == SQLITE_DONE);
        }
    } else if (sqlite3_prepare_v2(g_db, "DELETE FROM users WHERE id=?1 AND username<>?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_bind_text(st, 2, g_admin_user, -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(st) == SQLITE_DONE);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);
    if (!ok) {
        send_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"delete failed\"}", 36);
        return;
    }
    send_json(fd, 200, "OK", "{\"ok\":true}", 11);
}

void handle_api_wiki_audit_logs(http_sock_t fd, const char *path_qs)
{
    int limit = 10;
    char lim[16];
    if (query_param_get(path_qs, "limit", lim, sizeof(lim)) == 0) {
        int v = atoi(lim);
        if (v > 0 && v <= 1000) limit = v;
    }
    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"logs\":[");
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT ts,ip,username,action,target,detail FROM audit_logs ORDER BY id DESC LIMIT %d;", limit);
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) SB_LIT(&sb, ",");
            first = 0;
            SB_LIT(&sb, "{");
            SB_LIT(&sb, "\"ts\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 0)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"ip\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 1)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"username\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 2)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"action\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 3)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"target\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 4)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"detail\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 5));
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);
    SB_LIT(&sb, "]}");
    send_json(fd, 200, "OK", sb.data ? sb.data : "{\"ok\":true,\"logs\":[]}",
              sb.data ? sb.len : 21);
    free(sb.data);
}

void handle_api_wiki_md_history(http_sock_t fd, const char *path_qs)
{
    char article_id[256] = {0};
    int limit = 10;
    char lim[16];
    char with_content_s[8] = {0};
    int with_content = 0;
    query_param_get(path_qs, "id", article_id, sizeof(article_id));
    if (query_param_get(path_qs, "with_content", with_content_s, sizeof(with_content_s)) == 0) {
        with_content = (atoi(with_content_s) != 0);
    }
    if (!article_id[0]) with_content = 0;
    if (query_param_get(path_qs, "limit", lim, sizeof(lim)) == 0) {
        int v = atoi(lim);
        if (v > 0 && v <= 500) limit = v;
    }
    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"items\":[");
    const char *sql_by_id =
        "SELECT id,article_id,save_txn_id,title,category,editor,ip,created_at,length(content),content "
        "FROM md_backups WHERE article_id=?1 ORDER BY id DESC LIMIT ?2;";
    const char *sql_all =
        "SELECT id,article_id,save_txn_id,title,category,editor,ip,created_at,length(content) "
        "FROM md_backups ORDER BY id DESC LIMIT ?1;";
    if (sqlite3_prepare_v2(g_db, (article_id[0] ? sql_by_id : sql_all), -1, &st, NULL) == SQLITE_OK) {
        if (article_id[0]) {
            sqlite3_bind_text(st, 1, article_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, limit);
        } else {
            sqlite3_bind_int(st, 1, limit);
        }
        int first = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) SB_LIT(&sb, ",");
            first = 0;
            SB_LIT(&sb, "{");
            sb_appendf(&sb, "\"id\":%d,", sqlite3_column_int(st, 0));
            SB_LIT(&sb, "\"articleId\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 1)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"saveTxnId\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 2)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"title\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 3)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"category\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 4)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"editor\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 5)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"ip\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 6)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"createdAt\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 7)); SB_LIT(&sb, ",");
            sb_appendf(&sb, "\"contentLength\":%d", sqlite3_column_int(st, 8));
            if (with_content && article_id[0]) {
                SB_LIT(&sb, ",\"content\":");
                sb_json_str(&sb, (const char *)sqlite3_column_text(st, 9));
            }
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);
    SB_LIT(&sb, "]}");
    send_json(fd, 200, "OK", sb.data ? sb.data : "{\"ok\":true,\"items\":[]}",
              sb.data ? sb.len : 22);
    free(sb.data);
}

void handle_api_wiki_user_article_rank(http_sock_t fd, const char *path_qs)
{
    int limit = 10;
    char lim[16];
    if (query_param_get(path_qs, "limit", lim, sizeof(lim)) == 0) {
        int v = atoi(lim);
        if (v > 0 && v <= 1000) limit = v;
    }

    pthread_mutex_lock(&g_auth_mu);
    sqlite3_stmt *st = NULL;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"items\":[");

    const char *sql =
        "SELECT u.username,u.role,u.group_name,u.enabled,"
        "COALESCE(x.article_count,0),COALESCE(x.last_edit_at,'') "
        "FROM users u "
        "LEFT JOIN ("
        "  SELECT editor,COUNT(DISTINCT article_id) AS article_count,MAX(created_at) AS last_edit_at "
        "  FROM md_backups WHERE editor<>'' GROUP BY editor"
        ") x ON x.editor=u.username "
        "ORDER BY COALESCE(x.article_count,0) DESC, u.username ASC "
        "LIMIT ?1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        int first = 1;
        int rank = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) SB_LIT(&sb, ",");
            first = 0;
            SB_LIT(&sb, "{");
            sb_appendf(&sb, "\"rank\":%d,", rank++);
            SB_LIT(&sb, "\"username\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 0)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"role\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 1)); SB_LIT(&sb, ",");
            SB_LIT(&sb, "\"group\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 2)); SB_LIT(&sb, ",");
            sb_appendf(&sb, "\"enabled\":%d,", sqlite3_column_int(st, 3));
            sb_appendf(&sb, "\"articleCount\":%d,", sqlite3_column_int(st, 4));
            SB_LIT(&sb, "\"lastEditAt\":"); sb_json_str(&sb, (const char *)sqlite3_column_text(st, 5));
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_auth_mu);

    SB_LIT(&sb, "]}");
    send_json(fd, 200, "OK", sb.data ? sb.data : "{\"ok\":true,\"items\":[]}",
              sb.data ? sb.len : 22);
    free(sb.data);
}

#else

int auth_db_init(void) { return -1; }
void auth_db_close(void) {}

void auth_gen_save_txn_id(char *out, size_t outsz)
{
    static unsigned long long ctr = 0;
    ctr++;
    unsigned long long t = (unsigned long long)time(NULL);
    if (!out || outsz < 8) return;
    snprintf(out, outsz, "%08llx-%08llx", t, ctr);
}

int auth_resolve_user_from_headers(const char *req_headers, auth_user_t *out_user)
{
    (void)req_headers;
    if (out_user) memset(out_user, 0, sizeof(*out_user));
    return -1;
}

int auth_require_author(const char *req_headers, http_sock_t fd, auth_user_t *out_user)
{
    (void)req_headers; (void)out_user;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, auth disabled\"}", 67);
    return -1;
}

int auth_require_admin(const char *req_headers, http_sock_t fd, auth_user_t *out_user)
{
    (void)req_headers; (void)out_user;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, auth disabled\"}", 67);
    return -1;
}

void auth_audit(const char *ip, const char *username, const char *action,
                const char *target, const char *detail)
{
    (void)ip; (void)username; (void)action; (void)target; (void)detail;
}

void auth_audit_txn(const char *ip, const char *username, const char *action,
                    const char *target, const char *detail, const char *save_txn_id)
{
    (void)ip; (void)username; (void)action; (void)target; (void)detail; (void)save_txn_id;
}

void auth_md_backup(const char *article_id, const char *title, const char *category,
                    const char *content, const char *html, const char *editor,
                    const char *ip)
{
    (void)article_id; (void)title; (void)category; (void)content; (void)html; (void)editor; (void)ip;
}

void auth_md_backup_txn(const char *article_id, const char *title, const char *category,
                        const char *content, const char *html, const char *editor,
                        const char *ip, const char *save_txn_id)
{
    (void)article_id; (void)title; (void)category; (void)content; (void)html;
    (void)editor; (void)ip; (void)save_txn_id;
}

void handle_api_wiki_login(http_sock_t fd, const char *req_headers, const char *body, const char *ip)
{
    (void)req_headers; (void)body; (void)ip;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, login unavailable\"}", 72);
}

void handle_api_wiki_logout(http_sock_t fd, const char *req_headers)
{
    (void)req_headers;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, logout unavailable\"}", 73);
}

void handle_api_wiki_auth_status(http_sock_t fd, const char *req_headers)
{
    (void)req_headers;
    send_json(fd, 200, "OK",
              "{\"ok\":true,\"loggedIn\":false,\"error\":\"sqlite3 dev headers not found\"}", 69);
}

void handle_api_wiki_users_list(http_sock_t fd)
{
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, users unavailable\"}", 72);
}

void handle_api_wiki_user_save(http_sock_t fd, const char *body)
{
    (void)body;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, users unavailable\"}", 72);
}

void handle_api_wiki_user_delete(http_sock_t fd, const char *body)
{
    (void)body;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, users unavailable\"}", 72);
}

void handle_api_wiki_audit_logs(http_sock_t fd, const char *path_qs)
{
    (void)path_qs;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, logs unavailable\"}", 71);
}

void handle_api_wiki_md_history(http_sock_t fd, const char *path_qs)
{
    (void)path_qs;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, history unavailable\"}", 74);
}

void handle_api_wiki_user_article_rank(http_sock_t fd, const char *path_qs)
{
    (void)path_qs;
    send_json(fd, 500, "Internal Server Error",
              "{\"ok\":false,\"error\":\"sqlite3 dev headers not found, rank unavailable\"}", 71);
}

#endif
