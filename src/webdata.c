#include "webdata.h"
#include "http_utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_SQLITE3
#include <sqlite3.h>

static sqlite3           *g_wd_db = NULL;
static pthread_mutex_t    g_wd_mu = PTHREAD_MUTEX_INITIALIZER;
static char               g_wd_path[512];

static int wd_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(g_wd_db, sql, NULL, NULL, &err);
    if (err) {
        sqlite3_free(err);
    }
    return rc;
}

static int wd_open_schema(void)
{
    if (wd_exec(
            "CREATE TABLE IF NOT EXISTS app_logs("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ts TEXT NOT NULL,"
            "level TEXT NOT NULL,"
            "pid INTEGER NOT NULL,"
            "message TEXT NOT NULL);") != SQLITE_OK)
        return -1;
    if (wd_exec("CREATE INDEX IF NOT EXISTS idx_app_logs_ts ON app_logs(ts);") != SQLITE_OK)
        return -1;
    if (wd_exec(
            "CREATE TABLE IF NOT EXISTS login_events("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ts TEXT NOT NULL,"
            "ip TEXT NOT NULL,"
            "username TEXT NOT NULL,"
            "ok INTEGER NOT NULL,"
            "note TEXT NOT NULL);") != SQLITE_OK)
        return -1;
    if (wd_exec("CREATE INDEX IF NOT EXISTS idx_login_ip ON login_events(ip);") != SQLITE_OK)
        return -1;
    if (wd_exec("CREATE INDEX IF NOT EXISTS idx_login_user ON login_events(username);") !=
        SQLITE_OK)
        return -1;
    if (wd_exec("CREATE INDEX IF NOT EXISTS idx_login_ts ON login_events(ts);") != SQLITE_OK)
        return -1;
    (void)sqlite3_busy_timeout(g_wd_db, 5000);
    return 0;
}

int webdata_init(const char *log_dir)
{
    const char *d = (log_dir && log_dir[0]) ? log_dir : "logs";
    snprintf(g_wd_path, sizeof(g_wd_path), "%s/WebData.db", d);

    pthread_mutex_lock(&g_wd_mu);
    if (g_wd_db) {
        pthread_mutex_unlock(&g_wd_mu);
        return 0;
    }
    int rc = sqlite3_open_v2(g_wd_path, &g_wd_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK || !g_wd_db) {
        if (g_wd_db) sqlite3_close(g_wd_db);
        g_wd_db = NULL;
        pthread_mutex_unlock(&g_wd_mu);
        return -1;
    }
    if (wd_open_schema() != 0) {
        sqlite3_close(g_wd_db);
        g_wd_db = NULL;
        pthread_mutex_unlock(&g_wd_mu);
        return -1;
    }
    pthread_mutex_unlock(&g_wd_mu);
    return 0;
}

void webdata_close(void)
{
    pthread_mutex_lock(&g_wd_mu);
    if (g_wd_db) {
        sqlite3_close(g_wd_db);
        g_wd_db = NULL;
    }
    pthread_mutex_unlock(&g_wd_mu);
}

void webdata_append_log(const char *ts, const char *level, int pid, const char *message)
{
    if (!ts || !level || !message) return;
    pthread_mutex_lock(&g_wd_mu);
    if (!g_wd_db) {
        pthread_mutex_unlock(&g_wd_mu);
        return;
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO app_logs(ts,level,pid,message) VALUES(?1,?2,?3,?4);";
    if (sqlite3_prepare_v2(g_wd_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, level, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, pid);
        sqlite3_bind_text(st, 4, message, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_wd_mu);
}

void webdata_login_event(const char *ts, const char *ip, const char *username, int ok,
                         const char *note)
{
    pthread_mutex_lock(&g_wd_mu);
    if (!g_wd_db) {
        pthread_mutex_unlock(&g_wd_mu);
        return;
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO login_events(ts,ip,username,ok,note) VALUES(?1,?2,?3,?4,?5);";
    if (sqlite3_prepare_v2(g_wd_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ts ? ts : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, ip ? ip : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, username ? username : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, ok ? 1 : 0);
        sqlite3_bind_text(st, 5, note ? note : "", -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_wd_mu);
}

typedef struct {
    char a[256];
    char b[256];
} wd_pair_t;

/* a=ip b=username */
static size_t wd_load_ip_users(sqlite3 *db, wd_pair_t **out_pairs)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT DISTINCT ip, username FROM login_events WHERE ok=1 AND "
                      "IFNULL(username,'')!='' ORDER BY ip, username;";
    *out_pairs = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    size_t n = 0, cap = 0;
    wd_pair_t *pairs = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *ip = (const char *)sqlite3_column_text(st, 0);
        const char *un = (const char *)sqlite3_column_text(st, 1);
        if (n >= cap) {
            cap = cap ? cap * 2 : 64;
            wd_pair_t *np = (wd_pair_t *)realloc(pairs, cap * sizeof(wd_pair_t));
            if (!np) break;
            pairs = np;
        }
        snprintf(pairs[n].a, sizeof(pairs[n].a), "%s", ip ? ip : "");
        snprintf(pairs[n].b, sizeof(pairs[n].b), "%s", un ? un : "");
        n++;
    }
    sqlite3_finalize(st);
    *out_pairs = pairs;
    return n;
}

/* a=username b=ip */
static size_t wd_load_user_ips(sqlite3 *db, wd_pair_t **out_pairs)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT DISTINCT username, ip FROM login_events WHERE ok=1 AND "
                      "IFNULL(username,'')!='' AND IFNULL(ip,'')!='' ORDER BY username, ip;";
    *out_pairs = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    size_t n = 0, cap = 0;
    wd_pair_t *pairs = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *un = (const char *)sqlite3_column_text(st, 0);
        const char *ip = (const char *)sqlite3_column_text(st, 1);
        if (n >= cap) {
            cap = cap ? cap * 2 : 64;
            wd_pair_t *np = (wd_pair_t *)realloc(pairs, cap * sizeof(wd_pair_t));
            if (!np) break;
            pairs = np;
        }
        snprintf(pairs[n].a, sizeof(pairs[n].a), "%s", un ? un : "");
        snprintf(pairs[n].b, sizeof(pairs[n].b), "%s", ip ? ip : "");
        n++;
    }
    sqlite3_finalize(st);
    *out_pairs = pairs;
    return n;
}

static void sb_append_users_for_ip(strbuf_t *sb, const wd_pair_t *pairs, size_t npairs,
                                   const char *ip)
{
    SB_LIT(sb, "\"users\":[");
    int fu = 1;
    for (size_t i = 0; i < npairs; i++) {
        if (strcmp(pairs[i].a, ip) != 0) continue;
        if (!fu) SB_LIT(sb, ",");
        fu = 0;
        sb_json_str(sb, pairs[i].b);
    }
    SB_LIT(sb, "]");
}

static void sb_append_ips_for_user(strbuf_t *sb, const wd_pair_t *pairs, size_t npairs,
                                   const char *user)
{
    SB_LIT(sb, "\"ips\":[");
    int fi = 1;
    for (size_t i = 0; i < npairs; i++) {
        if (strcmp(pairs[i].a, user) != 0) continue;
        if (!fi) SB_LIT(sb, ",");
        fi = 0;
        sb_json_str(sb, pairs[i].b);
    }
    SB_LIT(sb, "]");
}

void handle_api_webdata_login_stats(http_sock_t client_fd, const char *path_with_query)
{
    char buf_sort[16], buf_usort[16];
    int ip_by_count = 0, user_by_count = 0;
    if (query_param_get(path_with_query, "ip_sort", buf_sort, sizeof(buf_sort)) == 0 &&
        strcmp(buf_sort, "count") == 0)
        ip_by_count = 1;
    if (query_param_get(path_with_query, "user_sort", buf_usort, sizeof(buf_usort)) == 0 &&
        strcmp(buf_usort, "count") == 0)
        user_by_count = 1;

    pthread_mutex_lock(&g_wd_mu);
    if (!g_wd_db) {
        pthread_mutex_unlock(&g_wd_mu);
        send_json(client_fd, 200, "OK",
                  "{\"ok\":true,\"byIp\":[],\"byUser\":[],\"error\":\"webdata unavailable\"}", 66);
        return;
    }

    wd_pair_t *ip_users = NULL;
    wd_pair_t *user_ips = NULL;
    size_t nu = wd_load_ip_users(g_wd_db, &ip_users);
    size_t ni = wd_load_user_ips(g_wd_db, &user_ips);

    const char *order_ip =
        ip_by_count ? " ORDER BY cnt DESC, ip ASC" : " ORDER BY last_ts DESC, ip ASC";
    char sql_ip[512];
    snprintf(sql_ip, sizeof(sql_ip),
             "SELECT ip, MAX(ts) AS last_ts, MAX(CASE WHEN ok=1 THEN ts END) AS last_ok_ts,"
             " COUNT(*) AS cnt, SUM(ok) AS successes FROM login_events GROUP BY ip%s;",
             order_ip);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"ip_sort\":");
    SB_LIT(&sb, ip_by_count ? "\"count\"" : "\"last\"");
    SB_LIT(&sb, ",\"user_sort\":");
    SB_LIT(&sb, user_by_count ? "\"count\"" : "\"last\"");
    SB_LIT(&sb, ",\"byIp\":[");

    sqlite3_stmt *st = NULL;
    int first_row = 1;
    if (sqlite3_prepare_v2(g_wd_db, sql_ip, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *ip = (const char *)sqlite3_column_text(st, 0);
            const char *last_ts = (const char *)sqlite3_column_text(st, 1);
            const char *last_ok_ts =
                sqlite3_column_type(st, 2) == SQLITE_NULL
                    ? NULL
                    : (const char *)sqlite3_column_text(st, 2);
            sqlite3_int64 cnt = sqlite3_column_int64(st, 3);
            sqlite3_int64 succ = sqlite3_column_int64(st, 4);

            if (!first_row) SB_LIT(&sb, ",");
            first_row = 0;
            SB_LIT(&sb, "{");
            SB_LIT(&sb, "\"ip\":");
            sb_json_str(&sb, ip ? ip : "");
            SB_LIT(&sb, ",\"last_ts\":");
            sb_json_str(&sb, last_ts ? last_ts : "");
            SB_LIT(&sb, ",\"last_success_ts\":");
            if (last_ok_ts)
                sb_json_str(&sb, last_ok_ts);
            else
                SB_LIT(&sb, "null");
            SB_LIT(&sb, ",\"attempts\":");
            sb_appendf(&sb, "%lld", (long long)cnt);
            SB_LIT(&sb, ",\"successes\":");
            sb_appendf(&sb, "%lld", (long long)succ);
            SB_LIT(&sb, ",");
            sb_append_users_for_ip(&sb, ip_users, nu, ip ? ip : "");
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);

    SB_LIT(&sb, "],\"byUser\":[");

    const char *order_user = user_by_count ? " ORDER BY cnt DESC, username ASC"
                                           : " ORDER BY last_ts DESC, username ASC";
    char sql_u[512];
    snprintf(
        sql_u, sizeof(sql_u),
        "SELECT username, MAX(ts) AS last_ts, MAX(CASE WHEN ok=1 THEN ts END) AS last_ok_ts,"
        " COUNT(*) AS cnt, SUM(ok) AS successes FROM login_events WHERE IFNULL(username,'')!='' "
        "GROUP BY username%s;",
        order_user);

    st = NULL;
    first_row = 1;
    if (sqlite3_prepare_v2(g_wd_db, sql_u, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *username = (const char *)sqlite3_column_text(st, 0);
            const char *last_ts = (const char *)sqlite3_column_text(st, 1);
            const char *last_ok_ts =
                sqlite3_column_type(st, 2) == SQLITE_NULL
                    ? NULL
                    : (const char *)sqlite3_column_text(st, 2);
            sqlite3_int64 cnt = sqlite3_column_int64(st, 3);
            sqlite3_int64 succ = sqlite3_column_int64(st, 4);

            if (!first_row) SB_LIT(&sb, ",");
            first_row = 0;
            SB_LIT(&sb, "{");
            SB_LIT(&sb, "\"username\":");
            sb_json_str(&sb, username ? username : "");
            SB_LIT(&sb, ",\"last_ts\":");
            sb_json_str(&sb, last_ts ? last_ts : "");
            SB_LIT(&sb, ",\"last_success_ts\":");
            if (last_ok_ts)
                sb_json_str(&sb, last_ok_ts);
            else
                SB_LIT(&sb, "null");
            SB_LIT(&sb, ",\"attempts\":");
            sb_appendf(&sb, "%lld", (long long)cnt);
            SB_LIT(&sb, ",\"successes\":");
            sb_appendf(&sb, "%lld", (long long)succ);
            SB_LIT(&sb, ",");
            sb_append_ips_for_user(&sb, user_ips, ni, username ? username : "");
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);

    SB_LIT(&sb, "]}");

    free(ip_users);
    free(user_ips);
    pthread_mutex_unlock(&g_wd_mu);

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 13);
    free(sb.data);
}

void handle_api_webdata_app_logs(http_sock_t client_fd, const char *path_with_query)
{
    char lim_buf[32];
    long limit = 500;
    if (query_param_get(path_with_query, "limit", lim_buf, sizeof(lim_buf)) == 0) {
        long v = atol(lim_buf);
        if (v > 0 && v <= 10000) limit = v;
    }

    pthread_mutex_lock(&g_wd_mu);
    if (!g_wd_db) {
        pthread_mutex_unlock(&g_wd_mu);
        send_json(client_fd, 200, "OK",
                  "{\"ok\":true,\"items\":[],\"error\":\"webdata unavailable\"}", 58);
        return;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, ts, level, pid, message FROM app_logs ORDER BY id DESC LIMIT ?1;";
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"items\":[");
    int first = 1;
    if (sqlite3_prepare_v2(g_wd_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            sqlite3_int64 id = sqlite3_column_int64(st, 0);
            const char *ts = (const char *)sqlite3_column_text(st, 1);
            const char *lv = (const char *)sqlite3_column_text(st, 2);
            int pid = sqlite3_column_int(st, 3);
            const char *msg = (const char *)sqlite3_column_text(st, 4);
            if (!first) SB_LIT(&sb, ",");
            first = 0;
            SB_LIT(&sb, "{");
            SB_LIT(&sb, "\"id\":");
            sb_appendf(&sb, "%lld", (long long)id);
            SB_LIT(&sb, ",\"ts\":");
            sb_json_str(&sb, ts ? ts : "");
            SB_LIT(&sb, ",\"level\":");
            sb_json_str(&sb, lv ? lv : "");
            SB_LIT(&sb, ",\"pid\":");
            sb_appendf(&sb, "%d", pid);
            SB_LIT(&sb, ",\"message\":");
            sb_json_str(&sb, msg ? msg : "");
            SB_LIT(&sb, "}");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&g_wd_mu);

    SB_LIT(&sb, "]}");
    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 13);
    free(sb.data);
}

#else /* !ENABLE_SQLITE3 */

int webdata_init(const char *log_dir)
{
    (void)log_dir;
    return 0;
}

void webdata_close(void) {}

void webdata_append_log(const char *ts, const char *level, int pid, const char *message)
{
    (void)ts;
    (void)level;
    (void)pid;
    (void)message;
}

void webdata_login_event(const char *ts, const char *ip, const char *username, int ok,
                         const char *note)
{
    (void)ts;
    (void)ip;
    (void)username;
    (void)ok;
    (void)note;
}

void handle_api_webdata_login_stats(http_sock_t client_fd, const char *path_with_query)
{
    (void)path_with_query;
    send_json(client_fd, 200, "OK",
              "{\"ok\":true,\"byIp\":[],\"byUser\":[],\"error\":\"build without SQLITE3\"}", 68);
}

void handle_api_webdata_app_logs(http_sock_t client_fd, const char *path_with_query)
{
    (void)path_with_query;
    send_json(client_fd, 200, "OK",
              "{\"ok\":true,\"items\":[],\"error\":\"build without SQLITE3\"}", 58);
}

#endif /* ENABLE_SQLITE3 */
