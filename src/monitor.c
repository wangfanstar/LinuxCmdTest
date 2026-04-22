#include "monitor.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>

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
static double          g_last_scan_dt     = 3.0;

/* ── UID→用户名缓存 ── */
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

/* ── /api/procs 响应缓存 ── */
#define PROCS_CACHE_SEC 4
static char           *g_procs_json     = NULL;
static size_t          g_procs_json_len = 0;
static time_t          g_procs_json_ts  = 0;
static char            g_procs_cache_key[160] = "";
static pthread_mutex_t g_procs_json_mtx = PTHREAD_MUTEX_INITIALIZER;

void stats_init(void) { g_start_time = time(NULL); }

void stats_req_start(const char *ip)
{
    __sync_fetch_and_add(&g_total_reqs, 1);
    int active = __sync_add_and_fetch(&g_active_conns, 1);
    int peak = g_peak_active;
    while (active > peak) {
        if (__sync_bool_compare_and_swap(&g_peak_active, peak, active)) break;
        peak = g_peak_active;
    }
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

void stats_req_end(void) { __sync_fetch_and_sub(&g_active_conns, 1); }

static int snap_sys_and_cores(cpu_snap_t *sys, cpu_snap_t *cores,
                               int max_cores, int *count)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    *count = 0;
    int sys_done = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        unsigned long long u=0, n=0, sy=0, id=0, iow=0, irq=0, si=0, st=0;
        char *p = line + 3;
        if (*p == ' ' || *p == '\t') {
            if (!sys_done &&
                sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &sy, &id, &iow, &irq, &si, &st) >= 4) {
                sys->work  = u + n + sy + irq + si + st;
                sys->total = sys->work + id + iow;
                sys_done = 1;
            }
        } else if (*p >= '0' && *p <= '9' && *count < max_cores) {
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

static void scan_proc_top(double dt, int core_count)
{
    if (dt < 0.3 || core_count <= 0) return;

    static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
    static time_t          last_scan = 0;
    if (pthread_mutex_trylock(&scan_lock) != 0) return;
    time_t scan_now = time(NULL);
    if (last_scan != 0 && difftime(scan_now, last_scan) < 2.0) {
        pthread_mutex_unlock(&scan_lock);
        return;
    }
    last_scan = scan_now;

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double scale = 100.0 / ((double)clk * dt);

    static proc_snap_t prev_copy[MAX_PROCS];
    int prev_count;
    pthread_mutex_lock(&g_proc_mutex);
    prev_count = g_prev_proc_count;
    memcpy(prev_copy, g_prev_procs, sizeof(proc_snap_t) * (size_t)prev_count);
    pthread_mutex_unlock(&g_proc_mutex);

    static proc_snap_t cur[MAX_PROCS];
    int cur_count = 0;

    top_proc_t tmp_core[MAX_CORES][TOP_PER_CORE];
    int        tmp_core_cnt[MAX_CORES];
    memset(tmp_core_cnt, 0, sizeof(int) * (size_t)core_count);

    top_proc_t tmp_global[TOP_GLOBAL];
    int        tmp_global_cnt = 0;

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

        for (int i = 0; i < core_count && i < g_cpu_core_count; i++) {
            unsigned long long cd = core_now[i].total - g_prev_core[i].total;
            unsigned long long cw = core_now[i].work  - g_prev_core[i].work;
            g_cpu_core_pct[i] = (cd > 0) ? (float)cw / (float)cd * 100.0f : 0.0f;
        }
    }
    g_prev_sys  = sys_now;
    g_prev_proc = proc_now;
    g_prev_ts   = now;
    for (int i = 0; i < core_count; i++) g_prev_core[i] = core_now[i];
    if (core_count > 0) {
        if (g_cpu_core_count == 0)
            for (int i = 0; i < core_count; i++) g_cpu_core_pct[i] = 0.0f;
        g_cpu_core_count = core_count;
    }
    pthread_mutex_unlock(&g_cpu_mutex);

    scan_proc_top(dt, core_count);
}

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

/* ── /proc/net/tcp[6] inode → 本地端口 ── */
#define TCP_INODE_MAP_MAX 4096
typedef struct { unsigned long ino; int port; } tcp_ino_port_t;

static int cmp_tcp_ino(const void *a, const void *b)
{
    unsigned long x = ((const tcp_ino_port_t *)a)->ino;
    unsigned long y = ((const tcp_ino_port_t *)b)->ino;
    return (x > y) - (x < y);
}

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

/* ── GET /api/port ───────────────────────────────────────────── */

void handle_api_port(http_sock_t client_fd, int query_port)
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

    unsigned long inodes[256];
    int           istates[256];
    int           inode_cnt = 0;

    const char *tcp_files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int fi = 0; fi < 2; fi++) {
        FILE *f = fopen(tcp_files[fi], "r");
        if (!f) continue;
        char line[512];
        fgets(line, sizeof(line), f);
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

/* ── GET /api/procs ──────────────────────────────────────────── */

void handle_api_procs(http_sock_t client_fd, const char *query, int include_ports)
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

            if (qlo[0]) {
                char nlo[32], ulo[20];
                size_t nl = strlen(name), ul = strlen(user);
                for (size_t k = 0; k < nl; k++) nlo[k] = (char)tolower((unsigned char)name[k]);
                nlo[nl] = '\0';
                for (size_t k = 0; k < ul; k++) ulo[k] = (char)tolower((unsigned char)user[k]);
                ulo[ul] = '\0';
                if (!strstr(nlo, qlo) && !strstr(ulo, qlo)) continue;
            }

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

/* ── GET /api/monitor ────────────────────────────────────────── */

void handle_api_monitor(http_sock_t client_fd)
{
    pthread_mutex_lock(&g_monitor_json_mtx);
    if (g_monitor_json &&
        difftime(time(NULL), g_monitor_json_ts) < MONITOR_CACHE_SEC) {
        send_json(client_fd, 200, "OK", g_monitor_json, g_monitor_json_len);
        pthread_mutex_unlock(&g_monitor_json_mtx);
        return;
    }
    pthread_mutex_unlock(&g_monitor_json_mtx);

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

    float  cpu_sys, cpu_proc;
    float  core_pct[MAX_CORES];
    int    core_count;
    pthread_mutex_lock(&g_cpu_mutex);
    cpu_sys    = g_cpu_sys_pct;
    cpu_proc   = g_cpu_proc_pct;
    core_count = g_cpu_core_count;
    for (int i = 0; i < core_count; i++) core_pct[i] = g_cpu_core_pct[i];
    pthread_mutex_unlock(&g_cpu_mutex);

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
