/* Windows 构建：/api/monitor 等依赖 /proc，此处保留请求统计与存根 API。 */
#include "monitor.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define ONLINE_WINDOW_SEC 300
#define MAX_IP_SLOTS 512

static volatile long  g_total_reqs   = 0;
static volatile int   g_active_conns = 0;
static volatile int   g_peak_active  = 0;
static time_t         g_start_time   = 0;

typedef struct {
    char     ip[16];
    time_t   ts;
} ip_slot_t;
static ip_slot_t       g_ip_slots[MAX_IP_SLOTS];
static int             g_ip_count    = 0;
static int             g_peak_online = 0;
static pthread_mutex_t g_ip_mutex    = PTHREAD_MUTEX_INITIALIZER;

void stats_init(void) { g_start_time = time(NULL); }

void stats_req_start(const char *ip)
{
    __sync_fetch_and_add(&g_total_reqs, 1);
    int active = __sync_add_and_fetch(&g_active_conns, 1);
    int peak   = g_peak_active;
    while (active > peak) {
        if (__sync_bool_compare_and_swap(&g_peak_active, peak, active)) {
            break;
        }
        peak = g_peak_active;
    }
    time_t now = time(NULL);
    pthread_mutex_lock(&g_ip_mutex);
    int found   = 0;
    int oldest_i = 0;
    for (int i = 0; i < g_ip_count; i++) {
        if (g_ip_slots[i].ts < g_ip_slots[oldest_i].ts) {
            oldest_i = i;
        }
        if (strncmp(g_ip_slots[i].ip, ip, 15) == 0) {
            g_ip_slots[i].ts = now;
            found            = 1;
            break;
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
    for (int i = 0; i < g_ip_count; i++) {
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) {
            online++;
        }
    }
    if (online > g_peak_online) {
        g_peak_online = online;
    }
    pthread_mutex_unlock(&g_ip_mutex);
}

void stats_req_end(void) { __sync_fetch_and_sub(&g_active_conns, 1); }

static int count_online(void)
{
    time_t now   = time(NULL);
    int    cnt   = 0;
    pthread_mutex_lock(&g_ip_mutex);
    for (int i = 0; i < g_ip_count; i++) {
        if (now - g_ip_slots[i].ts <= ONLINE_WINDOW_SEC) {
            cnt++;
        }
    }
    pthread_mutex_unlock(&g_ip_mutex);
    return cnt;
}

void handle_api_port(http_sock_t client_fd, int query_port)
{
    (void)query_port;
    send_json(client_fd, 200, "OK", "{\"port\":0,\"procs\":[]}", 22);
}

void handle_api_procs(http_sock_t client_fd, const char *query, int include_ports)
{
    (void)query;
    (void)include_ports;
    send_json(client_fd, 200, "OK", "{\"procs\":[]}", 12);
}

void handle_api_monitor(http_sock_t client_fd)
{
    int   online_now = count_online();
    long  uptime_sec = (long)difftime(time(NULL), g_start_time);
    strbuf_t sb = {0};
    sb_appendf(
        &sb,
        "{"
        "\"sys_cpu_pct\":0.0,"
        "\"sys_mem_total_kb\":0,"
        "\"sys_mem_used_kb\":0,"
        "\"sys_mem_pct\":0.0,"
        "\"proc_cpu_pct\":0.0,"
        "\"proc_mem_kb\":0,"
        "\"online_now\":%d,"
        "\"online_peak\":%d,"
        "\"total_requests\":%ld,"
        "\"active_conns\":%d,"
        "\"uptime_sec\":%ld,"
        "\"cores\":[],"
        "\"top10\":[]"
        "}",
        online_now, g_peak_online, g_total_reqs, g_active_conns, uptime_sec);
    if (sb.data) {
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    } else {
        send_json(client_fd, 500, "Internal Server Error", "{}", 2);
    }
    free(sb.data);
}
