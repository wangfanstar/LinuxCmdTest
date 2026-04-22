/* Windows 构建：SSH/PTY 依赖 fork 与 Unix 工具链，此处提供可链接存根。 */
#include "ssh_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void batch_set_err(ssh_batch_t *b, const char *msg)
{
    if (!b) {
        return;
    }
    if (msg) {
        strncpy(b->error, msg, sizeof(b->error) - 1);
        b->error[sizeof(b->error) - 1] = '\0';
    }
}

ssh_batch_t *ssh_batch_exec(const char *host, int port, const char *user,
                            const char *pass, char **commands, int cmd_count)
{
    (void)host;
    (void)port;
    (void)user;
    (void)pass;
    (void)commands;
    (void)cmd_count;
    ssh_batch_t *b = calloc(1, sizeof(ssh_batch_t));
    if (!b) {
        return NULL;
    }
    batch_set_err(b, "SSH not available in this Windows build (stub).");
    return b;
}

ssh_batch_t *ssh_session_exec(const char *host, int port, const char *user,
                              const char *pass, char **commands, int cmd_count,
                              int idle_timeout_sec)
{
    (void)idle_timeout_sec;
    return ssh_batch_exec(host, port, user, pass, commands, cmd_count);
}

void ssh_session_exec_stream(const char *host, int port, const char *user,
                             const char *pass, char **commands, int cmd_count,
                             ssh_stream_cb_t cb, void *ud, char *error_buf,
                             size_t error_buf_sz, int idle_timeout_sec, int *out_timed_out,
                             int *out_timeout_cmd_idx, char *out_partial_buf,
                             size_t out_partial_sz, int net_device_mode, int pty_debug)
{
    (void)host;
    (void)port;
    (void)user;
    (void)pass;
    (void)commands;
    (void)cmd_count;
    (void)cb;
    (void)ud;
    (void)idle_timeout_sec;
    (void)out_partial_buf;
    (void)out_partial_sz;
    (void)net_device_mode;
    (void)pty_debug;
    if (out_timed_out) {
        *out_timed_out = 0;
    }
    if (out_timeout_cmd_idx) {
        *out_timeout_cmd_idx = -1;
    }
    if (error_buf && error_buf_sz) {
        const char *m = "SSH stream not available in this Windows build (stub).";
        snprintf(error_buf, error_buf_sz, "%s", m);
    }
}

void ssh_batch_free(ssh_batch_t *b)
{
    if (!b) {
        return;
    }
    if (b->results) {
        for (int i = 0; i < b->count; i++) {
            free(b->results[i].cmd);
            free(b->results[i].output);
            free(b->results[i].workdir);
        }
        free(b->results);
    }
    free(b);
}

void ssh_cancel_current(void) {}
