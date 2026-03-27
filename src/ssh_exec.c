#include "ssh_exec.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/*  输入校验                                                            */
/* ------------------------------------------------------------------ */

static int validate_safe(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == '@' ||
            c == '[' || c == ']' || c == ':')
            continue;
        return 0;
    }
    return 1;
}


/* ------------------------------------------------------------------ */
/*  临时文件管理                                                        */
/* ------------------------------------------------------------------ */

/* 写密码到临时文件，权限 0600 */
static int create_pass_file(const char *pass, char *path, size_t len)
{
    snprintf(path, len, "/tmp/.swps_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    write(fd, pass, strlen(pass));
    close(fd);
    return 0;
}

/*
 * 创建 SSH_ASKPASS 脚本：#!/bin/sh\ncat '<pass_file>'\n
 * 权限 0700（必须可执行，否则 SSH 不会调用）
 */
static int create_askpass_script(const char *pass_file,
                                  char *script_path, size_t len)
{
    snprintf(script_path, len, "/tmp/.swpa_XXXXXX");
    int fd = mkstemp(script_path);
    if (fd < 0) return -1;
    fchmod(fd, 0700);

    char content[256];
    int  n = snprintf(content, sizeof(content),
                      "#!/bin/sh\ncat '%s'\n", pass_file);
    write(fd, content, (size_t)n);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  单条命令执行（fork + execv，不依赖 sshpass）                       */
/* ------------------------------------------------------------------ */

/*
 * 使用 SSH_ASKPASS 机制：
 *   - DISPLAY=:0          让旧版 OpenSSH 进入 askpass 分支
 *   - SSH_ASKPASS_REQUIRE=force  新版 OpenSSH(≥8.4) 强制使用 askpass
 *   - setsid              脱离控制终端，使旧版 SSH 真正走 askpass 路径
 *
 * 通过 pipe 捕获 stdout+stderr，waitpid 获取退出码。
 */
static int run_ssh_cmd(const char *host, int port,
                        const char *user, const char *askpass_file,
                        const char *cmd,
                        char **out_buf, size_t *out_len)
{
    /* 合并 user@host */
    char userhost[320];
    snprintf(userhost, sizeof(userhost), "%s@%s", user, host);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    /* stdout+stderr 合并到 pipe */
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* ── 子进程 ── */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* stdin → /dev/null：彻底断开终端输入，
         * 防止 SSH 在 askpass 失败时回退到交互输入 */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        /* 脱离控制终端（旧版 OpenSSH 依赖此条件才会调用 SSH_ASKPASS） */
        setsid();

        /* 构造环境变量：传入 HOME 以便 SSH 找到 ~/.ssh 目录 */
        char env_askpass[300], env_display[32], env_require[32], env_home[256];
        snprintf(env_askpass, sizeof(env_askpass), "SSH_ASKPASS=%s", askpass_file);
        snprintf(env_display, sizeof(env_display), "DISPLAY=:0");
        snprintf(env_require, sizeof(env_require), "SSH_ASKPASS_REQUIRE=force");
        const char *home = getenv("HOME");
        snprintf(env_home, sizeof(env_home), "HOME=%s", home ? home : "/tmp");

        char *envp[] = {
            env_askpass,
            env_display,
            env_require,
            env_home,
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            NULL
        };

        /* SSH 参数：
         *   GSSAPIAuthentication=no        跳过 GSSAPI，避免它抢占密码提示名额
         *   PreferredAuthentications=password  只用密码认证
         *   NumberOfPasswordPrompts=1      只提示一次密码，失败即退出
         */
        char *argv[] = {
            "ssh",
            "-o", "StrictHostKeyChecking=no",
            "-o", "ConnectTimeout=15",
            "-o", "BatchMode=no",
            "-o", "PasswordAuthentication=yes",
            "-o", "PubkeyAuthentication=no",
            "-o", "GSSAPIAuthentication=no",
            "-o", "PreferredAuthentications=password",
            "-o", "NumberOfPasswordPrompts=1",
            "-o", "LogLevel=ERROR",
            "-p", port_str,
            userhost,
            (char *)cmd,
            NULL
        };

        execvpe("ssh", argv, envp);
        fprintf(stderr, "execvpe ssh failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* ── 父进程：读取输出 ── */
    close(pipefd[1]);

    char *buf = malloc(SSH_OUTPUT_MAX);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    size_t total = 0;
    char   tmp[8192];
    ssize_t nr;
    while ((nr = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        size_t copy = (size_t)nr;
        if (total + copy >= SSH_OUTPUT_MAX - 1)
            copy = SSH_OUTPUT_MAX - 1 - total;
        memcpy(buf + total, tmp, copy);
        total += copy;
        if (total >= SSH_OUTPUT_MAX - 1) break;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    *out_buf = buf;
    *out_len = total;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ------------------------------------------------------------------ */
/*  会话模式：stdin 管道 + boundary 标记                               */
/* ------------------------------------------------------------------ */

/*
 * 通过 pipe 将脚本写入 SSH stdin（bash -s 读取），
 * 脚本在每条命令后输出  <boundary>:<exit_code>\n  作为分隔。
 */
static int run_ssh_session(const char *host, int port,
                            const char *user, const char *askpass_file,
                            const char *script,
                            char **out_buf, size_t *out_len)
{
    char userhost[320], port_str[8];
    snprintf(userhost, sizeof(userhost), "%s@%s", user, host);
    snprintf(port_str,  sizeof(port_str),  "%d",   port);

    int out_pipe[2], in_pipe[2];
    if (pipe(out_pipe) < 0) return -1;
    if (pipe(in_pipe)  < 0) { close(out_pipe[0]); close(out_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(in_pipe[0]);  close(in_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* ── 子进程 ── */
        close(out_pipe[0]);
        close(in_pipe[1]);

        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);

        dup2(in_pipe[0], STDIN_FILENO);   /* 脚本通过 stdin 送入远端 bash */
        close(in_pipe[0]);

        setsid();

        char env_askpass[300], env_display[32], env_require[32], env_home[256];
        snprintf(env_askpass, sizeof(env_askpass), "SSH_ASKPASS=%s", askpass_file);
        snprintf(env_display, sizeof(env_display), "DISPLAY=:0");
        snprintf(env_require, sizeof(env_require), "SSH_ASKPASS_REQUIRE=force");
        const char *home = getenv("HOME");
        snprintf(env_home, sizeof(env_home), "HOME=%s", home ? home : "/tmp");

        char *envp[] = { env_askpass, env_display, env_require, env_home,
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            NULL };

        char *argv[] = {
            "ssh",
            "-o", "StrictHostKeyChecking=no",
            "-o", "ConnectTimeout=15",
            "-o", "BatchMode=no",
            "-o", "PasswordAuthentication=yes",
            "-o", "PubkeyAuthentication=no",
            "-o", "GSSAPIAuthentication=no",
            "-o", "PreferredAuthentications=password",
            "-o", "NumberOfPasswordPrompts=1",
            "-o", "LogLevel=ERROR",
            "-o", "ServerAliveInterval=10",   /* 每 10s 发送 keepalive */
            "-o", "ServerAliveCountMax=6",    /* 连续 6 次无响应（60s）则断开 */
            "-T",           /* 不分配 TTY */
            "-p", port_str,
            userhost,
            "bash -s",      /* 从 stdin 读取并执行脚本 */
            NULL
        };

        execvpe("ssh", argv, envp);
        fprintf(stderr, "execvpe ssh failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* ── 父进程 ── */
    close(out_pipe[1]);
    close(in_pipe[0]);

    /* 将脚本写入 stdin 管道，完成后关闭（远端 bash 收到 EOF 后退出） */
    {
        const char *p   = script;
        size_t      rem = strlen(script);
        while (rem > 0) {
            ssize_t n = write(in_pipe[1], p, rem);
            if (n <= 0) break;
            p   += n;
            rem -= (size_t)n;
        }
    }
    close(in_pipe[1]);

    /* 读取全部输出（带空闲超时，避免卡死） */
#define SESSION_IDLE_TIMEOUT_SEC 300   /* 300s 无任何输出则强制终止 */

    char *buf = malloc(SSH_OUTPUT_MAX);
    if (!buf) {
        close(out_pipe[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    size_t  total = 0;
    char    tmp[8192];

    while (total < SSH_OUTPUT_MAX - 1) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(out_pipe[0], &rfds);
        tv.tv_sec  = SESSION_IDLE_TIMEOUT_SEC;
        tv.tv_usec = 0;

        int sel = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel == 0) {
            /* 超时：强制终止 SSH 子进程 */
            LOG_INFO("ssh_session: idle timeout (%ds), killing pid=%d",
                     SESSION_IDLE_TIMEOUT_SEC, (int)pid);
            kill(pid, SIGKILL);
            break;
        }
        if (sel < 0) break;   /* select 出错 */

        ssize_t nr = read(out_pipe[0], tmp, sizeof(tmp));
        if (nr <= 0) break;   /* EOF 或读错误 */

        size_t copy = (size_t)nr;
        if (total + copy >= SSH_OUTPUT_MAX - 1)
            copy = SSH_OUTPUT_MAX - 1 - total;
        memcpy(buf + total, tmp, copy);
        total += copy;
    }
    buf[total] = '\0';
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    *out_buf = buf;
    *out_len = total;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 构建复合 shell 脚本：每条命令后输出 boundary:exitcode\n */
static char *build_session_script(const char *boundary,
                                   char **commands, int count)
{
    /* 估算脚本大小 */
    size_t sz = 32;
    for (int i = 0; i < count; i++)
        sz += strlen(commands[i]) + strlen(boundary) + 32;

    char *script = malloc(sz);
    if (!script) return NULL;

    int off = 0;
    off += snprintf(script + off, sz - (size_t)off, "set +e\n");
    for (int i = 0; i < count; i++) {
        off += snprintf(script + off, sz - (size_t)off,
                        "%s\n"
                        "printf '%s:%%d\\n' $?\n",
                        commands[i], boundary);
    }
    return script;
}

/* 解析会话输出，按 boundary 分割为每条命令的结果 */
static void parse_session_output(const char *output, const char *boundary,
                                  ssh_cmd_result_t *results, int count)
{
    size_t blen = strlen(boundary);
    const char *p = output;

    for (int i = 0; i < count; i++) {
        /* 在 p 之后寻找  boundary:<digits>\n  */
        const char *marker = NULL;
        const char *q = p;
        while (*q) {
            if (strncmp(q, boundary, blen) == 0 && q[blen] == ':') {
                const char *r = q + blen + 1;
                while (*r >= '0' && *r <= '9') r++;
                if (r > q + blen + 1 && (*r == '\n' || *r == '\0')) {
                    marker = q;
                    break;
                }
            }
            q++;
        }

        if (marker) {
            results[i].exit_code = atoi(marker + blen + 1);
            /* 命令输出 = p 到 marker 之间（去掉末尾换行） */
            size_t len = (size_t)(marker - p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
            results[i].output = malloc(len + 1);
            if (results[i].output) {
                memcpy(results[i].output, p, len);
                results[i].output[len] = '\0';
            } else {
                results[i].output = strdup("");
            }
            /* 推进 p 到标记行末尾 */
            p = marker;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        } else {
            /* 未找到标记：剩余内容全归此命令 */
            results[i].output    = strdup(p);
            results[i].exit_code = -1;
            p += strlen(p);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  公开接口                                                            */
/* ------------------------------------------------------------------ */

ssh_batch_t *ssh_batch_exec(const char *host, int port,
                             const char *user, const char *pass,
                             char **commands, int cmd_count)
{
    ssh_batch_t *b = calloc(1, sizeof(ssh_batch_t));
    if (!b) return NULL;

    /* 输入校验 */
    if (!validate_safe(host)) {
        snprintf(b->error, sizeof(b->error),
                 "非法主机名: %s（仅允许字母、数字及 . - _ : []）", host);
        return b;
    }
    if (!validate_safe(user)) {
        snprintf(b->error, sizeof(b->error), "非法用户名: %s", user);
        return b;
    }
    if (port <= 0 || port > 65535) {
        snprintf(b->error, sizeof(b->error), "非法端口: %d", port);
        return b;
    }

    /* 创建密码文件 & askpass 脚本 */
    char pass_file[64], askpass_file[64];
    if (create_pass_file(pass, pass_file, sizeof(pass_file)) < 0) {
        snprintf(b->error, sizeof(b->error),
                 "创建临时密码文件失败: %s", strerror(errno));
        return b;
    }
    if (create_askpass_script(pass_file, askpass_file, sizeof(askpass_file)) < 0) {
        unlink(pass_file);
        snprintf(b->error, sizeof(b->error),
                 "创建 askpass 脚本失败: %s", strerror(errno));
        return b;
    }

    LOG_INFO("ssh_batch_exec: %s@%s:%d  commands=%d", user, host, port, cmd_count);

    /* 分配结果数组 */
    b->results = calloc(cmd_count, sizeof(ssh_cmd_result_t));
    if (!b->results) {
        unlink(pass_file); unlink(askpass_file);
        free(b); return NULL;
    }
    b->count = cmd_count;

    for (int i = 0; i < cmd_count; i++) {
        b->results[i].cmd = strdup(commands[i]);

        char  *out = NULL;
        size_t out_len = 0;
        int    exit_code = run_ssh_cmd(host, port, user, askpass_file,
                                        commands[i], &out, &out_len);

        b->results[i].output    = out ? out : strdup("");
        b->results[i].exit_code = exit_code;

        LOG_INFO("ssh_exec[%d] exit=%d len=%zu cmd=%s",
                 i, exit_code, out_len, commands[i]);
    }

    /* 清理临时文件 */
    unlink(pass_file);
    unlink(askpass_file);
    return b;
}

ssh_batch_t *ssh_session_exec(const char *host, int port,
                               const char *user, const char *pass,
                               char **commands, int cmd_count)
{
    ssh_batch_t *b = calloc(1, sizeof(ssh_batch_t));
    if (!b) return NULL;

    if (!validate_safe(host)) {
        snprintf(b->error, sizeof(b->error),
                 "非法主机名: %s（仅允许字母、数字及 . - _ : []）", host);
        return b;
    }
    if (!validate_safe(user)) {
        snprintf(b->error, sizeof(b->error), "非法用户名: %s", user);
        return b;
    }
    if (port <= 0 || port > 65535) {
        snprintf(b->error, sizeof(b->error), "非法端口: %d", port);
        return b;
    }

    char pass_file[64], askpass_file[64];
    if (create_pass_file(pass, pass_file, sizeof(pass_file)) < 0) {
        snprintf(b->error, sizeof(b->error),
                 "创建临时密码文件失败: %s", strerror(errno));
        return b;
    }
    if (create_askpass_script(pass_file, askpass_file, sizeof(askpass_file)) < 0) {
        unlink(pass_file);
        snprintf(b->error, sizeof(b->error),
                 "创建 askpass 脚本失败: %s", strerror(errno));
        return b;
    }

    /* 分配结果数组 */
    b->results = calloc(cmd_count, sizeof(ssh_cmd_result_t));
    if (!b->results) {
        unlink(pass_file); unlink(askpass_file);
        free(b); return NULL;
    }
    b->count = cmd_count;
    for (int i = 0; i < cmd_count; i++) {
        b->results[i].cmd    = strdup(commands[i]);
        b->results[i].output = strdup("");
        b->results[i].exit_code = -1;
    }

    /* 生成唯一边界标记（避免与命令输出冲突） */
    char boundary[24];
    unsigned int token = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    FILE *rf = fopen("/dev/urandom", "r");
    if (rf) { fread(&token, sizeof(token), 1, rf); fclose(rf); }
    snprintf(boundary, sizeof(boundary), "SWBND%08X", token);

    /* 构建并执行复合脚本 */
    char *script = build_session_script(boundary, commands, cmd_count);
    if (!script) {
        snprintf(b->error, sizeof(b->error), "构建脚本失败（内存不足）");
        goto cleanup;
    }

    LOG_INFO("ssh_session_exec: %s@%s:%d  commands=%d boundary=%s",
             user, host, port, cmd_count, boundary);

    {
        char  *output = NULL;
        size_t out_len = 0;
        run_ssh_session(host, port, user, askpass_file,
                        script, &output, &out_len);

        if (output) {
            /* SSH 连接失败时输出通常含 "Permission denied" 等错误关键词 */
            if (out_len > 0 && cmd_count > 0) {
                /* 尝试解析；若未找到任何 boundary 说明是连接错误 */
                const char *has_marker = strstr(output, boundary);
                if (!has_marker) {
                    /* 无标记 = SSH 连接/认证失败，输出即为错误信息 */
                    size_t elen = out_len < sizeof(b->error) - 1
                                  ? out_len : sizeof(b->error) - 1;
                    memcpy(b->error, output, elen);
                    b->error[elen] = '\0';
                    /* 去掉末尾换行 */
                    size_t el = strlen(b->error);
                    while (el > 0 && (b->error[el-1]=='\n'||b->error[el-1]=='\r'))
                        b->error[--el] = '\0';
                } else {
                    parse_session_output(output, boundary, b->results, cmd_count);
                }
            }
            free(output);
        }
    }

    free(script);
cleanup:
    unlink(pass_file);
    unlink(askpass_file);
    return b;
}

void ssh_batch_free(ssh_batch_t *b)
{
    if (!b) return;
    for (int i = 0; i < b->count; i++) {
        free(b->results[i].cmd);
        free(b->results[i].output);
    }
    free(b->results);
    free(b);
}
