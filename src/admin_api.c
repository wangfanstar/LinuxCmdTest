/* admin_api.c —— 管理员 API：日志文件列表 / 日志分块读取 / IP 访问统计 */

#include "admin_api.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#endif

/* ── 工具 ────────────────────────────────────────────────────── */

/* 校验日志文件名：server_N.log，N ∈ [0, LOG_MAX_FILES) */
static int log_filename_safe(const char *fn)
{
    size_t pl = strlen(LOG_FILE_PREFIX);
    if (strncmp(fn, LOG_FILE_PREFIX, pl) != 0 || fn[pl] != '_') return 0;
    const char *p = fn + pl + 1;
    if (!isdigit((unsigned char)*p)) return 0;
    int idx = 0;
    for (; isdigit((unsigned char)*p); p++) {
        idx = idx * 10 + (*p - '0');
        if (idx >= LOG_MAX_FILES) return 0;
    }
    size_t rl = strlen(".log");
    size_t len = strlen(fn);
    return len > rl && strcmp(fn + len - rl, ".log") == 0;
}

/* 列出日志目录内所有 server_N.log（按序号升序），返回数量 */
static int list_log_files(char names[][256], int cap)
{
    int n = 0;
    DIR *d = opendir(log_get_dir());
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < cap) {
        if (!log_filename_safe(de->d_name)) continue;
        snprintf(names[n], 256, "%s", de->d_name);
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) {
        char tmp[256];
        int j = i;
        snprintf(tmp, sizeof(tmp), "%s", names[j]);
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            snprintf(names[j], 256, "%s", names[j - 1]);
            j--;
        }
        snprintf(names[j], 256, "%s", tmp);
    }
    return n;
}

/* 读一行（fgets 基础上处理超长行：丢弃剩余直到换行），返回 0 成功 / 1 EOF */
static int read_log_line(FILE *f, char *buf, size_t cap)
{
    if (!fgets(buf, (int)cap, f)) return 1;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') { buf[len - 1] = '\0'; return 0; }
    int ch;
    while ((ch = fgetc(f)) != EOF && ch != '\n') {}
    return 0;
}

/* 提取行首时间戳 "[YYYY-MM-DD HH:MM:SS]"，失败返回 -1 */
static int extract_ts(const char *line, char *out, size_t cap)
{
    if (line[0] != '[' || strlen(line) < 20 || line[20] != ']') return -1;
    for (int i = 1; i <= 19; i++) {
        if (i == 5 || i == 8) { if (line[i] != '-') return -1; }
        else if (i == 11) { if (line[i] != ' ') return -1; }
        else if (i == 14 || i == 17) { if (line[i] != ':') return -1; }
        else if (!isdigit((unsigned char)line[i])) return -1;
    }
    if (cap < 20) return -1;
    memcpy(out, line + 1, 19);
    out[19] = '\0';
    return 0;
}

/* 提取行内第一个合法 IPv4（每段 0-255，前后边界非数字），返回网络序数值 */
static int extract_ipv4(const char *s, unsigned int *out)
{
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) continue;
        unsigned int a, b, c, d;
        char tail;
        int r = sscanf(p, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
        if (r < 4) continue;
        if (r == 5 && isdigit((unsigned char)tail)) continue;
        if (a > 255 || b > 255 || c > 255 || d > 255) continue;
        if (p > s && isdigit((unsigned char)p[-1])) continue;
        *out = (a << 24) | (b << 16) | (c << 8) | d;
        return 0;
    }
    return -1;
}

/* 大小写不敏感子串匹配 */
static int str_contains_nocase(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* 拼接 目录/文件名 到固定缓冲（有界，截断安全） */
static void log_build_path(char *buf, size_t cap, const char *dir, const char *name)
{
    if (cap == 0) return;
    strncpy(buf, dir, cap - 1);
    buf[cap - 1] = '\0';
    size_t used = strlen(buf);
    if (used + 1 < cap) {
        buf[used] = '/';
        buf[used + 1] = '\0';
        used++;
    }
    strncat(buf, name, cap - used - 1);
}

/* ── GET /api/admin-log-files ───────────────────────────────── */

void handle_api_admin_log_files(http_sock_t client_fd)
{
    char names[LOG_MAX_FILES][256];
    int n = list_log_files(names, LOG_MAX_FILES);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"dir\":");
    sb_json_str(&sb, log_get_dir());
    SB_LIT(&sb, ",\"files\":[");
    for (int i = 0; i < n; i++) {
        char full[1024];
        log_build_path(full, sizeof(full), log_get_dir(), names[i]);
        struct stat st;
        long long size = 0, mtime = 0;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            size = (long long)st.st_size;
            mtime = (long long)st.st_mtime;
        }
        if (i) SB_LIT(&sb, ",");
        SB_LIT(&sb, "{\"name\":");
        sb_json_str(&sb, names[i]);
        sb_appendf(&sb, ",\"size\":%lld,\"mtime\":%lld}", size, mtime);
    }
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
    free(sb.data);
}

/* ── GET /api/admin-log-read ────────────────────────────────── */

void handle_api_admin_log_read(http_sock_t client_fd, const char *path_qs)
{
    char file[256] = {0}, off_s[32] = {0}, lim_s[32] = {0};
    if (query_param_get(path_qs, "file", file, sizeof(file)) != 0 ||
        !log_filename_safe(file)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid file\"}", 36);
        return;
    }
    long long offset = 0, limit = 200 * 1024;
    if (query_param_get(path_qs, "offset", off_s, sizeof(off_s)) == 0)
        offset = strtoll(off_s, NULL, 10);
    if (query_param_get(path_qs, "limit", lim_s, sizeof(lim_s)) == 0)
        limit = strtoll(lim_s, NULL, 10);
    if (offset < 0) offset = 0;
    if (limit < 1024) limit = 1024;
    if (limit > 2 * 1024 * 1024) limit = 2 * 1024 * 1024;

    char fpath[1024];
    log_build_path(fpath, sizeof(fpath), log_get_dir(), file);
    FILE *f = fopen(fpath, "rb");
    if (!f) {
        send_json(client_fd, 404, "Not Found",
                  "{\"ok\":false,\"error\":\"file not found\"}", 42);
        return;
    }
    fseek(f, 0, SEEK_END);
    long long total = ftell(f);
    if (offset > total) offset = total;

    char *buf = malloc((size_t)limit + 1);
    if (!buf) {
        fclose(f);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
        return;
    }
    fseek(f, offset, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)limit, f);
    fclose(f);
    buf[got] = '\0';

    /* offset>0 时丢弃块首可能截断的半行 */
    long long start = offset;
    if (offset > 0 && got > 0) {
        size_t skip = 0;
        while (skip < got && buf[skip] != '\n') skip++;
        if (skip >= got) { got = 0; start = offset + (long long)skip; }
        else {
            skip++;
            got -= skip;
            memmove(buf, buf + skip, got);
            start = offset + (long long)skip;
        }
    }

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"file\":");
    sb_json_str(&sb, file);
    sb_appendf(&sb, ",\"offset\":%lld,\"size\":%lld,\"total\":%lld,\"data\":",
               start, (long long)got, total);
    sb_json_str(&sb, buf);
    SB_LIT(&sb, "}");
    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);
    free(buf);
}

/* ── GET /api/admin-ip-stats ────────────────────────────────── */

#define IP_HASH_SIZE 4096

typedef struct ip_node {
    unsigned int ip;
    long long count;
    char last_ts[20];
    struct ip_node *next;
} ip_node_t;

static unsigned int hash_ip(unsigned int ip)
{
    ip ^= ip >> 16;
    ip *= 0x7feb352dU;
    ip ^= ip >> 15;
    ip *= 0x846ca68bU;
    ip ^= ip >> 16;
    return ip & (IP_HASH_SIZE - 1);
}

static int ip_cmp(const void *pa, const void *pb)
{
    const ip_node_t *a = *(const ip_node_t * const *)pa;
    const ip_node_t *b = *(const ip_node_t * const *)pb;
    if (a->count != b->count) return a->count < b->count ? 1 : -1;
    return a->ip < b->ip ? -1 : (a->ip > b->ip ? 1 : 0);
}

static void ip_str(unsigned int ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
             ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

void handle_api_admin_ip_stats(http_sock_t client_fd, const char *path_qs)
{
    char file[256] = {0}, from[24] = {0}, to[24] = {0};
    char path_kw[256] = {0}, ip_kw[64] = {0}, top_s[16] = {0};
    query_param_get(path_qs, "file", file, sizeof(file));
    query_param_get(path_qs, "from", from, sizeof(from));
    query_param_get(path_qs, "to", to, sizeof(to));
    query_param_get(path_qs, "path", path_kw, sizeof(path_kw));
    query_param_get(path_qs, "ip", ip_kw, sizeof(ip_kw));
    int top = 100;
    if (query_param_get(path_qs, "top", top_s, sizeof(top_s)) == 0) {
        top = atoi(top_s);
        if (top < 1) top = 1;
        if (top > 500) top = 500;
    }

    char names[LOG_MAX_FILES][256];
    int nfiles = 0;
    if (file[0]) {
        if (!log_filename_safe(file)) {
            send_json(client_fd, 400, "Bad Request",
                      "{\"ok\":false,\"error\":\"invalid file\"}", 36);
            return;
        }
        snprintf(names[0], 256, "%s", file);
        nfiles = 1;
    } else {
        nfiles = list_log_files(names, LOG_MAX_FILES);
    }
    if (nfiles == 0) {
        static const char empty[] =
            "{\"ok\":true,\"scanned\":0,\"matched\":0,\"unique\":0,\"ips\":[]}";
        send_json(client_fd, 200, "OK", empty, sizeof(empty) - 1);
        return;
    }

    ip_node_t **buckets = calloc(IP_HASH_SIZE, sizeof(ip_node_t *));
    if (!buckets) {
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
        return;
    }

    long long scanned = 0, matched = 0;
    char line[8192];
    for (int fi = 0; fi < nfiles; fi++) {
        char fpath[1024];
        log_build_path(fpath, sizeof(fpath), log_get_dir(), names[fi]);
        FILE *f = fopen(fpath, "rb");
        if (!f) continue;
        while (read_log_line(f, line, sizeof(line)) == 0) {
            scanned++;
            char ts[20];
            if (extract_ts(line, ts, sizeof(ts)) != 0) continue;
            if (from[0] && strncmp(ts, from, strlen(from)) < 0) continue;
            if (to[0] && strncmp(ts, to, strlen(to)) > 0) continue;
            if (path_kw[0] && !str_contains_nocase(line, path_kw)) continue;
            if (ip_kw[0] && !str_contains_nocase(line, ip_kw)) continue;
            unsigned int ip;
            if (extract_ipv4(line, &ip) != 0) continue;
            matched++;
            unsigned int h = hash_ip(ip);
            ip_node_t *nd = buckets[h];
            while (nd && nd->ip != ip) nd = nd->next;
            if (!nd) {
                nd = malloc(sizeof(*nd));
                if (!nd) continue;
                nd->ip = ip;
                nd->count = 0;
                nd->last_ts[0] = '\0';
                nd->next = buckets[h];
                buckets[h] = nd;
            }
            nd->count++;
            snprintf(nd->last_ts, sizeof(nd->last_ts), "%s", ts);
        }
        fclose(f);
    }

    /* 收集并排序 */
    int unique = 0;
    for (int i = 0; i < IP_HASH_SIZE; i++)
        for (ip_node_t *nd = buckets[i]; nd; nd = nd->next) unique++;
    ip_node_t **arr = malloc((size_t)(unique > 0 ? unique : 1) * sizeof(ip_node_t *));
    if (!arr) {
        for (int i = 0; i < IP_HASH_SIZE; i++) {
            ip_node_t *nd = buckets[i];
            while (nd) { ip_node_t *nx = nd->next; free(nd); nd = nx; }
        }
        free(buckets);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
        return;
    }
    int k = 0;
    for (int i = 0; i < IP_HASH_SIZE; i++)
        for (ip_node_t *nd = buckets[i]; nd; nd = nd->next) arr[k++] = nd;
    qsort(arr, (size_t)unique, sizeof(ip_node_t *), ip_cmp);
    if (top > unique) top = unique;

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,");
    sb_appendf(&sb, "\"scanned\":%lld,\"matched\":%lld,\"unique\":%d,", scanned, matched, unique);
    SB_LIT(&sb, "\"ips\":[");
    char ipbuf[16];
    for (int i = 0; i < top; i++) {
        if (i) SB_LIT(&sb, ",");
        ip_str(arr[i]->ip, ipbuf, sizeof(ipbuf));
        SB_LIT(&sb, "{\"ip\":");
        sb_json_str(&sb, ipbuf);
        sb_appendf(&sb, ",\"count\":%lld,\"last\":", arr[i]->count);
        sb_json_str(&sb, arr[i]->last_ts);
        SB_LIT(&sb, "}");
    }
    SB_LIT(&sb, "]}");
    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);

    free(arr);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_node_t *nd = buckets[i];
        while (nd) { ip_node_t *nx = nd->next; free(nd); nd = nx; }
    }
    free(buckets);
}

/* ── GET /api/admin-ip-host ────────────────────────────────── */

#define HOST_CACHE_SIZE 512

typedef struct {
    unsigned int ip;
    char host[256];   /* 空串 = 无 PTR 记录 */
    time_t expire;
} host_cache_t;

static host_cache_t g_host_cache[HOST_CACHE_SIZE];
static size_t g_host_cache_n = 0;
static pthread_mutex_t g_host_mu = PTHREAD_MUTEX_INITIALIZER;

/* 严格 IPv4 校验：整个字符串必须是合法点分十进制 */
static int ipv4_parse(const char *s, unsigned int *out)
{
    if (!s || !*s) return -1;
    unsigned int a, b, c, d;
    char tail;
    int r = sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
    if (r != 4) return -1;   /* r==5 表示后面还有多余字符 */
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static void ipv4_str(unsigned int ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
             ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

/* 反解主机名（带缓存；未命中时 getaddrinfo 可能阻塞数秒） */
static void hostname_lookup(unsigned int ip, char *out, size_t cap)
{
    out[0] = '\0';
    char ipstr[16];
    ipv4_str(ip, ipstr, sizeof(ipstr));

    time_t now = time(NULL);
    pthread_mutex_lock(&g_host_mu);
    for (size_t i = 0; i < g_host_cache_n; i++) {
        if (g_host_cache[i].ip == ip) {
            if (g_host_cache[i].expire > now) {
                snprintf(out, cap, "%s", g_host_cache[i].host);
                pthread_mutex_unlock(&g_host_mu);
                return;
            }
            memmove(&g_host_cache[i], &g_host_cache[i + 1],
                    (g_host_cache_n - i - 1) * sizeof(host_cache_t));
            g_host_cache_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_host_mu);

    /* getnameinfo 走 NSS（/etc/hosts + DNS），可解析局域网 hosts 条目 */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ip);
    char hostbuf[NI_MAXHOST];
    if (getnameinfo((struct sockaddr *)&sa, sizeof(sa),
                    hostbuf, sizeof(hostbuf), NULL, 0, NI_NAMEREQD) == 0) {
        if (strcmp(hostbuf, ipstr) != 0)
            snprintf(out, cap, "%s", hostbuf);
    }

    /* 成功缓存 24h，失败缓存 10min（避免无 PTR 的 IP 反复阻塞） */
    time_t ttl = out[0] ? 86400 : 600;
    pthread_mutex_lock(&g_host_mu);
    size_t i;
    if (g_host_cache_n >= HOST_CACHE_SIZE) {
        memmove(&g_host_cache[0], &g_host_cache[1],
                (HOST_CACHE_SIZE - 1) * sizeof(host_cache_t));
        i = HOST_CACHE_SIZE - 1;
    } else {
        i = g_host_cache_n++;
    }
    g_host_cache[i].ip = ip;
    snprintf(g_host_cache[i].host, sizeof(g_host_cache[i].host), "%s", out);
    g_host_cache[i].expire = now + ttl;
    pthread_mutex_unlock(&g_host_mu);
}

void handle_api_admin_ip_host(http_sock_t client_fd, const char *path_qs)
{
    char ipstr[64] = {0};
    if (query_param_get(path_qs, "ip", ipstr, sizeof(ipstr)) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"ip required\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }
    unsigned int ip;
    if (ipv4_parse(ipstr, &ip) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"invalid ip\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }

    char host[256];
    hostname_lookup(ip, host, sizeof(host));

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"ip\":");
    sb_json_str(&sb, ipstr);
    SB_LIT(&sb, ",\"host\":");
    if (host[0]) sb_json_str(&sb, host);
    else SB_LIT(&sb, "null");
    SB_LIT(&sb, "}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
    free(sb.data);
}

/* ── GET /api/admin-ip-logs ─────────────────────────────────── */

void handle_api_admin_ip_logs(http_sock_t client_fd, const char *path_qs)
{
    char ipstr[64] = {0};
    if (query_param_get(path_qs, "ip", ipstr, sizeof(ipstr)) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"ip required\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }
    unsigned int ipnum;
    if (ipv4_parse(ipstr, &ipnum) != 0) {
        static const char err[] = "{\"ok\":false,\"error\":\"invalid ip\"}";
        send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
        return;
    }

    char file[256] = {0}, lim_s[16] = {0};
    query_param_get(path_qs, "file", file, sizeof(file));
    int limit = 500;
    if (query_param_get(path_qs, "limit", lim_s, sizeof(lim_s)) == 0) {
        limit = atoi(lim_s);
        if (limit < 1) limit = 1;
        if (limit > 2000) limit = 2000;
    }

    char names[LOG_MAX_FILES][256];
    int nfiles;
    if (file[0]) {
        if (!log_filename_safe(file)) {
            static const char err[] = "{\"ok\":false,\"error\":\"invalid file\"}";
            send_json(client_fd, 400, "Bad Request", err, sizeof(err) - 1);
            return;
        }
        snprintf(names[0], 256, "%s", file);
        nfiles = 1;
    } else {
        nfiles = list_log_files(names, LOG_MAX_FILES);
    }

    /* 环形缓冲只保留最后 limit 条匹配行 */
    char **ring = calloc((size_t)limit, sizeof(char *));
    if (!ring) {
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 11);
        return;
    }
    int ring_pos = 0, ring_n = 0;
    long long scanned = 0, matched = 0;
    char line[8192];
    for (int fi = 0; fi < nfiles; fi++) {
        char fpath[1024];
        log_build_path(fpath, sizeof(fpath), log_get_dir(), names[fi]);
        FILE *f = fopen(fpath, "rb");
        if (!f) continue;
        while (read_log_line(f, line, sizeof(line)) == 0) {
            scanned++;
            if (!str_contains_nocase(line, ipstr)) continue;
            matched++;
            size_t len = strlen(line);
            if (len > 2048) len = 2048;
            char *copy = malloc(len + 2);
            if (!copy) continue;
            memcpy(copy, line, len);
            copy[len] = '\n';
            copy[len + 1] = '\0';
            if (ring[ring_pos]) free(ring[ring_pos]);
            ring[ring_pos] = copy;
            ring_pos = (ring_pos + 1) % limit;
            if (ring_n < limit) ring_n++;
        }
        fclose(f);
    }

    strbuf_t dsb = {0};
    int start = (ring_n < limit) ? 0 : ring_pos;
    for (int i = 0; i < ring_n; i++) {
        int idx = (start + i) % limit;
        sb_append(&dsb, ring[idx], strlen(ring[idx]));
    }

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"ip\":");
    sb_json_str(&sb, ipstr);
    sb_appendf(&sb, ",\"scanned\":%lld,\"matched\":%lld,\"returned\":%d,\"truncated\":%d,\"data\":",
               scanned, matched, ring_n, matched > ring_n ? 1 : 0);
    sb_json_str(&sb, dsb.data ? dsb.data : "");
    SB_LIT(&sb, "}");
    send_json(client_fd, 200, "OK", sb.data, sb.len);
    free(sb.data);
    free(dsb.data);
    for (int i = 0; i < limit; i++) free(ring[i]);
    free(ring);
}
