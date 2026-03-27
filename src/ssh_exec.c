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

/* 单引号转义：' → '\'' */
static void escape_sq(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 5 < out_len; i++) {
        if (in[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\';
            out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
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
