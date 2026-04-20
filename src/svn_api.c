#include "svn_api.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>

/* ── 输入校验工具 ─────────────────────────────────────────────── */

static int svn_url_safe(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && !strchr("/:.-_~%@?=&+", (int)c)) return 0;
    }
    return 1;
}

static int svn_simple_safe(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return 0;
    }
    return 1;
}

static int svn_date_safe(const char *s)
{
    if (strlen(s) != 10) return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else { if (!isdigit((unsigned char)s[i])) return 0; }
    }
    return 1;
}

/* ── POST /api/svn-log ───────────────────────────────────────── */

void handle_api_svn_log(int client_fd, const char *body)
{
    char url[1024] = {0}, user[128] = {0}, pass[512] = {0};
    char author[128] = {0}, date_from[32] = {0}, date_to[32] = {0};
    int  limit = 500;

    json_get_str(body, "url",       url,       sizeof(url));
    json_get_str(body, "user",      user,      sizeof(user));
    json_api_get_pass(body,         pass,      sizeof(pass));
    json_get_str(body, "author",    author,    sizeof(author));
    json_get_str(body, "date_from", date_from, sizeof(date_from));
    json_get_str(body, "date_to",   date_to,   sizeof(date_to));
    limit = json_get_int(body, "limit", 500);
    if (limit <= 0 || limit > 5000) limit = 500;

    if (!url[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing url\"}", 37); return;
    }
    if (!svn_url_safe(url)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid url\"}", 36); return;
    }
    if (user[0] && !svn_simple_safe(user)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid user\"}", 37); return;
    }
    if (author[0] && !svn_simple_safe(author)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid author\"}", 39); return;
    }
    if (date_from[0] && !svn_date_safe(date_from)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid date_from\"}", 42); return;
    }
    if (date_to[0] && !svn_date_safe(date_to)) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"invalid date_to\"}", 40); return;
    }

    char limit_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", limit);

    char rev_arg[80] = {0};
    if (date_from[0] || date_to[0]) {
        const char *f = date_from[0] ? date_from : "1970-01-01";
        if (date_to[0])
            snprintf(rev_arg, sizeof(rev_arg), "{%s}:{%s}", f, date_to);
        else
            snprintf(rev_arg, sizeof(rev_arg), "{%s}:HEAD", f);
    }

    const char *argv[32];
    int ai = 0;
    argv[ai++] = "svn";
    argv[ai++] = "log";
    argv[ai++] = "--xml";
    argv[ai++] = "-v";
    argv[ai++] = "--no-auth-cache";
    argv[ai++] = "--non-interactive";
    if (user[0]) { argv[ai++] = "--username"; argv[ai++] = user; }
    if (pass[0]) { argv[ai++] = "--password"; argv[ai++] = pass; }
    argv[ai++] = "--limit";
    argv[ai++] = limit_str;
    if (rev_arg[0]) { argv[ai++] = "-r"; argv[ai++] = rev_arg; }
    argv[ai++] = url;
    argv[ai]   = NULL;

    int pfd[2];
    if (pipe(pfd) < 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"pipe failed\"}", 36); return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"fork failed\"}", 36); return;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp("svn", (char *const *)argv);
        fprintf(stderr, "execvp svn failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(pfd[1]);
    strbuf_t out = {0};
    char rbuf[8192];
    ssize_t rn;
    while ((rn = read(pfd[0], rbuf, sizeof(rbuf))) > 0)
        sb_append(&out, rbuf, (size_t)rn);
    close(pfd[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    strbuf_t sb = {0};
    if (exit_code == 0) {
        SB_LIT(&sb, "{\"ok\":true,\"xml\":");
        sb_json_str(&sb, out.data ? out.data : "");
        SB_LIT(&sb, "}");
    } else {
        SB_LIT(&sb, "{\"ok\":false,\"error\":");
        sb_json_str(&sb, out.data ? out.data : "svn exited with error");
        sb_appendf(&sb, ",\"exit_code\":%d}", exit_code);
    }
    free(out.data);

    if (sb.data)
        send_json(client_fd, 200, "OK", sb.data, sb.len);
    else
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"out of memory\"}", 38);
    free(sb.data);
}
