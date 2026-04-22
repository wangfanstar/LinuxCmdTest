#include "ssh_api.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "ssh_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#endif

/* ── POST /api/ssh-exec-one ──────────────────────────────────── */

void handle_api_ssh_exec_one(http_sock_t client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    char command[CMD_BUF_SIZE] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_api_get_pass(body, pass, sizeof(pass));
    port = json_get_int(body, "port", 22);
    if (!user[0]) strcpy(user, "root");

    if (json_get_str(body, "command", command, sizeof(command)) < 0 || !command[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing command\"}", 26); return;
    }

    LOG_INFO("api_ssh_exec_one: %s@%s:%d  cmd=%s", user, host, port, command);

    char *cmds[1] = { command };
    ssh_batch_t *result = ssh_batch_exec(host, port, user, pass, cmds, 1);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    if (result && !result->error[0] && result->count > 0) {
        sb_appendf(&sb, ",\"exit_code\":%d", result->results[0].exit_code);
        SB_LIT(&sb, ",\"output\":");
        sb_json_str(&sb, result->results[0].output ? result->results[0].output : "");
    } else {
        SB_LIT(&sb, ",\"exit_code\":-1,\"output\":\"\"");
    }
    SB_LIT(&sb, "}");

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\",\"exit_code\":-1,\"output\":\"\"}", 51);
    free(sb.data);
    if (result) ssh_batch_free(result);
}

/* ── POST /api/ssh-exec ──────────────────────────────────────── */

void handle_api_ssh_exec(http_sock_t client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_api_get_pass(body, pass, sizeof(pass));
    port    = json_get_int(body, "port",    22);
    int timeout = json_get_int(body, "timeout", 0);
    if (!user[0]) strcpy(user, "root");

    char (*cmd_bufs)[CMD_BUF_SIZE] = calloc(MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (!cmd_bufs) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\"}", 24); return;
    }
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];

    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        free(cmd_bufs);
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec: %s@%s:%d  commands=%d timeout=%d",
             user, host, port, cmd_count, timeout);

    ssh_batch_t *result = ssh_session_exec(host, port, user, pass,
                                           cmd_ptrs, cmd_count, timeout);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"error\":");
    sb_json_str(&sb, (result && result->error[0]) ? result->error : "");
    SB_LIT(&sb, ",\"results\":[");

    if (result && !result->error[0]) {
        for (int i = 0; i < result->count; i++) {
            if (i > 0) SB_LIT(&sb, ",");
            SB_LIT(&sb, "{\"cmd\":");
            sb_json_str(&sb, result->results[i].cmd     ? result->results[i].cmd     : "");
            sb_appendf(&sb, ",\"exit_code\":%d", result->results[i].exit_code);
            SB_LIT(&sb, ",\"output\":");
            sb_json_str(&sb, result->results[i].output  ? result->results[i].output  : "");
            SB_LIT(&sb, ",\"workdir\":");
            sb_json_str(&sb, result->results[i].workdir ? result->results[i].workdir : "");
            SB_LIT(&sb, "}");
        }
    }
    SB_LIT(&sb, "]}");

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\",\"results\":[]}", 38);

    free(sb.data);
    free(cmd_bufs);
    if (result) ssh_batch_free(result);
}

/* ── POST /api/ssh-exec-stream ───────────────────────────────── */

static int sse_write_json(http_sock_t fd, const char *json)
{
    if (http_sock_send_all(fd, "data: ", 6) < 0) {
        return -1;
    }
    if (http_sock_send_all(fd, json, strlen(json)) < 0) {
        return -1;
    }
    if (http_sock_send_all(fd, "\n\n", 2) < 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    http_sock_t         fd;
    char (*cmds)[CMD_BUF_SIZE];
    int                 completed;
    int                 conn_broken;
} stream_ctx_t;

static void sse_on_write_fail(stream_ctx_t *ctx)
{
    if (!ctx->conn_broken) {
        ctx->conn_broken = 1;
        LOG_INFO("sse_stream: client disconnected, cancelling SSH session");
        ssh_cancel_current();
    }
}

static void on_stream_result(int idx, const char *cmd, const char *output,
                              int exit_code, const char *prompt_after, void *ud)
{
    stream_ctx_t *ctx = (stream_ctx_t *)ud;
    http_sock_t   fd  = ctx->fd;
    strbuf_t      sb  = {0};

    if (ctx->conn_broken) return;

    if (idx == -3) {
        int cmd_i = (prompt_after && *prompt_after) ? atoi(prompt_after) : 0;
        sb_appendf(&sb, "{\"type\":\"partial\",\"i\":%d,\"output\":", cmd_i);
        sb_json_str(&sb, output ? output : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }
    if (idx == -2) {
        sb_appendf(&sb, "{\"type\":\"diag\",\"info\":");
        sb_json_str(&sb, cmd ? cmd : "");
        sb_appendf(&sb, ",\"tail\":");
        sb_json_str(&sb, output ? output : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }
    if (idx == -1) {
        SB_LIT(&sb, "{\"type\":\"session_prompt\",\"prompt\":");
        sb_json_str(&sb, prompt_after ? prompt_after : "");
        SB_LIT(&sb, "}");
        if (sb.data) {
            if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
            free(sb.data);
        }
        return;
    }

    ctx->completed++;
    sb_appendf(&sb, "{\"type\":\"result\",\"i\":%d,\"cmd\":", idx);
    sb_json_str(&sb, cmd ? cmd : "");
    sb_appendf(&sb, ",\"exit_code\":%d,\"output\":", exit_code);
    sb_json_str(&sb, output ? output : "");
    SB_LIT(&sb, ",\"prompt_after\":");
    sb_json_str(&sb, prompt_after ? prompt_after : "");
    SB_LIT(&sb, "}");
    if (sb.data) {
        if (sse_write_json(fd, sb.data) < 0) sse_on_write_fail(ctx);
        free(sb.data);
    }
}

void handle_api_ssh_exec_stream(http_sock_t client_fd, const char *body)
{
    char host[256] = {0}, user[64] = {0}, pass[256] = {0};
    int  port = 22;

    if (json_get_str(body, "host", host, sizeof(host)) < 0 || !host[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"missing host\"}", 24); return;
    }
    json_get_str(body, "user", user, sizeof(user));
    json_api_get_pass(body, pass, sizeof(pass));
    port             = json_get_int(body, "port",       22);
    int timeout      = json_get_int(body, "timeout",    0);
    int net_device   = json_get_int(body, "net_device", 0);
    int pty_debug    = json_get_int(body, "pty_debug",  0);
    if (!user[0]) strcpy(user, "root");

    char (*cmd_bufs)[CMD_BUF_SIZE] = calloc(MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (!cmd_bufs) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"error\":\"out of memory\"}", 24); return;
    }
    char *cmd_ptrs[MAX_CMD_COUNT];
    for (int i = 0; i < MAX_CMD_COUNT; i++) cmd_ptrs[i] = cmd_bufs[i];
    int cmd_count = json_get_str_array(body, "commands",
                                        cmd_ptrs, MAX_CMD_COUNT, CMD_BUF_SIZE);
    if (cmd_count <= 0) {
        free(cmd_bufs);
        send_json(client_fd, 400, "Bad Request",
                  "{\"error\":\"no commands\"}", 22); return;
    }

    LOG_INFO("api_ssh_exec_stream: %s@%s:%d  commands=%d timeout_req=%d pty_debug=%d",
             user, host, port, cmd_count, timeout, pty_debug);

    {
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&flag, sizeof(flag));
    }

    const char *sse_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Accel-Buffering: no\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)http_sock_send_all(client_fd, sse_hdr, strlen(sse_hdr));

    if (pty_debug) {
        int eff_to = (timeout > 0) ? timeout : 300;
        strbuf_t ds = {0};
        sb_appendf(&ds,
                   "{\"type\":\"debug\",\"pty_debug\":1,\"timeout_req\":%d,"
                   "\"effective_timeout_sec\":%d,"
                   "\"log\":\"server file logs: ssh_pty_diag / ssh_session_exec_stream\"}",
                   timeout, eff_to);
        if (ds.data) {
            sse_write_json(client_fd, ds.data);
            free(ds.data);
        }
    }

#define PARTIAL_BUF_MAX (64 * 1024)
    stream_ctx_t ctx = { client_fd, cmd_bufs, 0, 0 };
    char  error_buf[512] = {0};
    int   timed_out = 0;
    int   timeout_cmd_idx = -1;
    char *partial_buf = calloc(1, PARTIAL_BUF_MAX);
    ssh_session_exec_stream(host, port, user, pass,
                            cmd_ptrs, cmd_count,
                            on_stream_result, &ctx,
                            error_buf, sizeof(error_buf), timeout,
                            &timed_out,
                            &timeout_cmd_idx,
                            partial_buf, partial_buf ? PARTIAL_BUF_MAX : 0,
                            net_device,
                            pty_debug);

    if (!ctx.conn_broken) {
        int timeout_sec = (timeout > 0) ? timeout : 300;
        strbuf_t sb = {0};
        if (timed_out) {
            sb_appendf(&sb, "{\"type\":\"timeout\",\"completed\":%d,\"total\":%d,\"timeout_sec\":%d",
                       ctx.completed, cmd_count, timeout_sec);
            if (timeout_cmd_idx >= 0)
                sb_appendf(&sb, ",\"i\":%d", timeout_cmd_idx);
            if (partial_buf && partial_buf[0]) {
                SB_LIT(&sb, ",\"partial\":");
                sb_json_str(&sb, partial_buf);
            }
            SB_LIT(&sb, "}");
        } else if (error_buf[0]) {
            SB_LIT(&sb, "{\"type\":\"error\",\"message\":");
            sb_json_str(&sb, error_buf);
            SB_LIT(&sb, "}");
        } else {
            sb_appendf(&sb, "{\"type\":\"done\",\"total\":%d}", cmd_count);
        }
        if (sb.data) { sse_write_json(client_fd, sb.data); free(sb.data); }
    }
    free(partial_buf);
    free(cmd_bufs);
}
