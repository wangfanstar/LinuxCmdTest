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
#include <pthread.h>
#include <time.h>
/* ------------------------------------------------------------------ */
/*  全局会话 PID 追踪（供 ssh_cancel_current 使用）                   */
/* ------------------------------------------------------------------ */
static volatile pid_t  g_session_pid   = (pid_t)-1;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;

static void session_pid_set(pid_t pid)
{
    pthread_mutex_lock(&g_session_mutex);
    g_session_pid = pid;
    pthread_mutex_unlock(&g_session_mutex);
}

void ssh_cancel_current(void)
{
    pthread_mutex_lock(&g_session_mutex);
    pid_t pid = g_session_pid;
    pthread_mutex_unlock(&g_session_mutex);
    if (pid > (pid_t)0) {
        kill(pid, SIGKILL);
        LOG_INFO("ssh_cancel_current: SIGKILL -> pid=%d", (int)pid);
    }
}

/* ------------------------------------------------------------------ */
/*  网络设备模式辅助函数                                               */
/* ------------------------------------------------------------------ */

/* 去掉 ANSI/VT100 转义序列（PTY 模式下 bash 彩色提示符含转义码）。
 * 仅在栈上临时使用；dst 须足够大（len+1 即可）。返回新长度。 */
static size_t strip_ansi(const char *src, size_t len, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t i = 0; i < len; ) {
        if (src[i] == '\033' && i + 1 < len) {
            if (src[i+1] == '[') {
                /* CSI 序列：ESC [ <参数字节> <终止字节 0x40-0x7E> */
                i += 2;
                while (i < len) {
                    unsigned char c = (unsigned char)src[i++];
                    if (c >= 0x40 && c <= 0x7e) break;
                }
            } else if (src[i+1] == ']') {
                /* OSC 序列：ESC ] ... BEL 或 ST */
                i += 2;
                while (i < len &&
                       !(src[i] == '\007') &&
                       !(src[i] == '\033' && i+1 < len && src[i+1] == '\\'))
                    i++;
                if (i < len) i++;
            } else {
                /* 其他双字节 ESC 序列 */
                i += 2;
            }
        } else {
            if (di + 1 < dst_sz) dst[di++] = src[i];
            i++;
        }
    }
    if (dst_sz > 0) dst[di < dst_sz ? di : dst_sz - 1] = '\0';
    return di;
}

/* 单调时钟秒数（用于 PTY 墙钟超时，不受系统对时影响） */
static double monotonic_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (double)time(NULL);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 判断 buf 末尾是否为 CLI 提示符（> # $ ] %）。
 * VRP 提示符示例：<GT>  [GT-GigabitEthernet0/0/0]
 * Linux 提示符示例：root@host:~#   user@host:~$
 * PTY 模式下 bash 彩色提示符含 ANSI 转义码，先剥离再判断。
 * 注意：许多交换机用 "%% ..." 报错，末行若以单个 % 结尾易误判（如进度 100%），
 * 故仅当末行较短且不含 "%%" 时才把 % 当作提示符。 */
static int is_prompt(const char *buf, size_t len)
{
    if (!len) return 0;
    /* 跳过末尾换行，找最后一个实际内容字符 */
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
    if (!len) return 0;
    /* 找最后一行的起始位置 */
    size_t i = len;
    while (i > 0 && buf[i-1] != '\n') i--;
    const char *last = buf + i;
    size_t ll = len - i;
    /* 在最后一行中逐字符扫描（跳过 ANSI 序列），找最后一个非空实际字符 */
    char last_real = 0;
    for (size_t j = 0; j < ll; ) {
        if (last[j] == '\033' && j + 1 < ll) {
            if (last[j+1] == '[') {
                j += 2;
                while (j < ll) {
                    unsigned char c = (unsigned char)last[j++];
                    if (c >= 0x40 && c <= 0x7e) break;
                }
            } else { j += 2; }
        } else {
            char c = last[j];
            if (c != ' ' && c != '\r') last_real = c;
            j++;
        }
    }
    if (!last_real) return 0;
    if (last_real == '>' || last_real == '#' || last_real == '$' ||
        last_real == ']')
        return 1;
    if (last_real != '%')
        return 0;
    /* 末行含 %% → 多为设备报错/说明，勿当提示符（否则会提前发下一条命令导致会话错乱） */
    for (size_t j = 0; j + 1 < ll; j++) {
        if (last[j] == '%' && last[j + 1] == '%')
            return 0;
    }
    /* 过长末行以 % 结尾多为百分比等，非 hostname% 类提示符 */
    if (ll > 56)
        return 0;
    return 1;
}

/* 清理命令输出：去掉首行回显和末尾提示符行，返回堆分配字符串（调用方 free）。 */
static char *strip_cmd_output(const char *buf, size_t len)
{
    size_t start = 0, end = len;

    /* 去首行（设备回显的命令行），找第一个 \n */
    while (start < end && buf[start] != '\n') start++;
    if (start < end) start++;   /* 跳过 \n 本身 */

    /* 去末尾提示符行（以 > # $ ] % 结尾的行） */
    while (end > start) {
        /* 去末尾空白 */
        while (end > start && (buf[end-1]=='\n'||buf[end-1]=='\r'||buf[end-1]==' '))
            end--;
        if (end <= start) break;
        char c = buf[end-1];
        if (c == '>' || c == '#' || c == '$' || c == ']' || c == '%') {
            /* 找该行起始 */
            while (end > start && buf[end-1] != '\n') end--;
        } else {
            break;
        }
    }
    /* 去末尾空行 */
    while (end > start && (buf[end-1]=='\n'||buf[end-1]=='\r'||buf[end-1]==' '))
        end--;

    size_t rlen = (end > start) ? (end - start) : 0;
    /* 去掉 ANSI 转义序列（PTY 模式下 bash/自定义 CLI 可能含彩色码）*/
    char *result = malloc(rlen + 1);
    if (!result) return NULL;
    if (rlen) {
        size_t clean_len = strip_ansi(buf + start, rlen, result, rlen + 1);
        result[clean_len] = '\0';
    } else {
        result[0] = '\0';
    }
    return result;
}

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
                            char **out_buf, size_t *out_len,
                            int idle_timeout_sec)
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

    /* 注册当前 SSH 子进程 PID，供 ssh_cancel_current() 使用 */
    session_pid_set(pid);

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
#define SESSION_IDLE_TIMEOUT_DEFAULT 300   /* 默认空闲超时 300s */
    if (idle_timeout_sec <= 0) idle_timeout_sec = SESSION_IDLE_TIMEOUT_DEFAULT;

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
        tv.tv_sec  = idle_timeout_sec;
        tv.tv_usec = 0;

        int sel = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel == 0) {
            /* 超时：强制终止 SSH 子进程 */
            LOG_INFO("ssh_session: idle timeout (%ds), killing pid=%d",
                     idle_timeout_sec, (int)pid);
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

    /* 清除全局 PID（会话结束） */
    session_pid_set((pid_t)-1);

    int status = 0;
    waitpid(pid, &status, 0);

    *out_buf = buf;
    *out_len = total;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 与 linux_cmd_test 导入规则一致：去掉行首「~ 」「! 」比对标记后再交给 shell。
 * PTY/bash 下行首 ~ 会被当作波浪线展开，导致命令异常、无提示符、空闲超时行为混乱。 */
static const char *skip_shell_marker_prefix(const char *cmd)
{
    if (!cmd) return "";
    if (cmd[0] == '~' && cmd[1] == ' ') return cmd + 2;
    if (cmd[0] == '!' && cmd[1] == ' ') return cmd + 2;
    return cmd;
}

/* 构建复合 shell 脚本：每条命令后输出 boundary:exitcode\n */
static char *build_session_script(const char *boundary,
                                   char **commands, int count)
{
    /* 估算脚本大小 */
    size_t sz = 256;  /* 为 trap 行预留空间 */
    for (int i = 0; i < count; i++)
        sz += strlen(skip_shell_marker_prefix(commands[i])) + strlen(boundary) + 64;

    char *script = malloc(sz);
    if (!script) return NULL;

    int off = 0;
    /* 会话结束（正常退出/连接断开/信号）时，向整个进程组发送 SIGTERM，
     * 确保命令启动的子进程（及其后代）随 SSH 会话一起终止，
     * 避免测试结束或用户复位后仍有进程在远端占用 CPU。
     * kill -- -$$  向 PGID=$$ 的全部进程发 SIGTERM（bash 是进程组组长）。
     * kill -9 -- -$$ 兜底，防止子进程忽略 SIGTERM。               */
    off += snprintf(script + off, sz - (size_t)off,
        "trap 'trap \"\" HUP EXIT INT TERM;"
        "kill -- -$$ 2>/dev/null;"
        "kill -9 -- -$$ 2>/dev/null' HUP EXIT INT TERM\n"
        "set +e\n");
    for (int i = 0; i < count; i++) {
        const char *c = skip_shell_marker_prefix(commands[i]);
        off += snprintf(script + off, sz - (size_t)off,
                        "%s\n"
                        "printf '%s:%%d\\n' $?\n",
                        c, boundary);
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
        /* 在 p 之后寻找  BOUNDARY:<digits>\n  */
        const char *marker = NULL;
        const char *q = p;
        while (*q) {
            if (strncmp(q, boundary, blen) == 0 && q[blen] == ':') {
                const char *r = q + blen + 1;
                while (*r >= '0' && *r <= '9') r++;
                if (r > q + blen + 1 && (*r == '\n' || *r == '\r' || *r == '\0')) {
                    marker = q;
                    break;
                }
            }
            q++;
        }

        if (marker) {
            /* 解析边界行：BOUNDARY:<exit_code>\n  （不含 pwd） */
            const char *after_colon = marker + blen + 1;
            results[i].exit_code = atoi(after_colon);
            results[i].workdir   = strdup("");

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
            results[i].workdir   = strdup("");
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
                               char **commands, int cmd_count,
                               int idle_timeout_sec)
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
        b->results[i].cmd       = strdup(commands[i]);
        b->results[i].output    = strdup("");
        b->results[i].workdir   = strdup("");
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
                        script, &output, &out_len, idle_timeout_sec);

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

/* ------------------------------------------------------------------ */
/*  流式执行：每条命令完成后立即回调                                   */
/* ------------------------------------------------------------------ */

void ssh_session_exec_stream(const char *host, int port,
                              const char *user, const char *pass,
                              char **commands, int cmd_count,
                              ssh_stream_cb_t cb, void *ud,
                              char *error_buf, size_t error_buf_sz,
                              int idle_timeout_sec,
                              int *out_timed_out,
                              int *out_timeout_cmd_idx,
                              char *out_partial_buf, size_t out_partial_sz,
                              int net_device_mode)
{
    error_buf[0] = '\0';
    if (out_timed_out)    *out_timed_out = 0;
    if (out_timeout_cmd_idx) *out_timeout_cmd_idx = -1;
    if (out_partial_buf && out_partial_sz > 0) out_partial_buf[0] = '\0';
    int timed_out = 0;

    /* 输入校验 */
    if (!validate_safe(host)) {
        snprintf(error_buf, error_buf_sz,
                 "非法主机名: %s（仅允许字母、数字及 . - _ : []）", host);
        return;
    }
    if (!validate_safe(user)) {
        snprintf(error_buf, error_buf_sz, "非法用户名: %s", user);
        return;
    }
    if (port <= 0 || port > 65535) {
        snprintf(error_buf, error_buf_sz, "非法端口: %d", port);
        return;
    }

    char   pass_file[64] = {0}, askpass_file[64] = {0};
    int    has_pass_file = 0, has_askpass = 0;
    char  *script = NULL;
    char   userhost[320] = {0}, port_str[8] = {0};
    int    out_pipe[2] = {-1,-1}, in_pipe[2] = {-1,-1};
    pid_t  pid = (pid_t)-1;
    char   boundary[24] = {0};
    size_t blen = 0;

    if (idle_timeout_sec <= 0) idle_timeout_sec = SESSION_IDLE_TIMEOUT_DEFAULT;

    if (create_pass_file(pass, pass_file, sizeof(pass_file)) < 0) {
        snprintf(error_buf, error_buf_sz,
                 "创建临时密码文件失败: %s", strerror(errno));
        return;
    }
    has_pass_file = 1;

    if (create_askpass_script(pass_file, askpass_file, sizeof(askpass_file)) < 0) {
        snprintf(error_buf, error_buf_sz,
                 "创建 askpass 脚本失败: %s", strerror(errno));
        has_askpass = 0;
        goto cleanup;
    }
    has_askpass = 1;

    /* 生成唯一边界标记 */
    {
        unsigned int token = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        FILE *rf = fopen("/dev/urandom", "r");
        if (rf) { fread(&token, sizeof(token), 1, rf); fclose(rf); }
        snprintf(boundary, sizeof(boundary), "SWBND%08X", token);
    }
    blen = strlen(boundary);

    script = build_session_script(boundary, commands, cmd_count);
    if (!script) {
        snprintf(error_buf, error_buf_sz, "构建脚本失败（内存不足）");
        goto cleanup;
    }

    LOG_INFO("ssh_session_exec_stream: %s@%s:%d  commands=%d boundary=%s",
             user, host, port, cmd_count, boundary);

    /* 创建管道 + fork */
    snprintf(userhost, sizeof(userhost), "%s@%s", user, host);
    snprintf(port_str,  sizeof(port_str),  "%d",   port);

    if (pipe(out_pipe) < 0) {
        snprintf(error_buf, error_buf_sz, "pipe() 失败: %s", strerror(errno));
        goto cleanup;
    }
    if (pipe(in_pipe) < 0) {
        snprintf(error_buf, error_buf_sz, "pipe() 失败: %s", strerror(errno));
        close(out_pipe[0]); close(out_pipe[1]);
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
        snprintf(error_buf, error_buf_sz, "fork() 失败: %s", strerror(errno));
        close(out_pipe[0]); close(out_pipe[1]);
        close(in_pipe[0]);  close(in_pipe[1]);
        out_pipe[0] = out_pipe[1] = in_pipe[0] = in_pipe[1] = -1;
        goto cleanup;
    }

    if (pid == 0) {
        /* ── 子进程 ── */
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);

        close(in_pipe[1]);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]);
        setsid();

        char env_askpass[300], env_display[32], env_require[32], env_home[256];
        snprintf(env_askpass, sizeof(env_askpass), "SSH_ASKPASS=%s", askpass_file);
        snprintf(env_display, sizeof(env_display), "DISPLAY=:0");
        snprintf(env_require, sizeof(env_require), "SSH_ASKPASS_REQUIRE=force");
        const char *home = getenv("HOME");
        snprintf(env_home, sizeof(env_home), "HOME=%s", home ? home : "/tmp");

        /* 所有模式共用同一份环境变量：SSH_ASKPASS 处理密码认证 */
        char *envp_askpass[] = { env_askpass, env_display, env_require, env_home,
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            NULL };

        /* 网络设备模式：不指定远端命令，直接打开交互式 shell；
         * Linux 模式：指定 "bash -s"，通过 stdin 管道输入脚本。   */
        char *argv_linux[] = {
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
            "-o", "ServerAliveInterval=10",
            "-o", "ServerAliveCountMax=6",
            "-T",
            "-p", port_str,
            userhost,
            "bash -s",
            NULL
        };
        /* 网络设备模式（不分配 PTY）：适用于 Huawei VRP 等网络设备 CLI */
        char *argv_netdev[] = {
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
            "-o", "ServerAliveInterval=10",
            "-o", "ServerAliveCountMax=6",
            "-T",
            "-p", port_str,
            userhost,
            NULL
        };
        /* PTY 模式：SSH_ASKPASS 处理密码，-t -t 强制向服务端申请 PTY。
         * 双 -t 的作用：即使客户端 stdin 不是 tty（当前为管道），
         * 也强制发送 pty-req，服务端 bash 获得真实 PTY，
         * isatty(0)=1，build.sh 等需要 TTY 的脚本可正常运行。
         * SSH_ASKPASS_REQUIRE=force + setsid（无控制终端）保证密码由
         * askpass 脚本提供，而非等待用户在终端输入。 */
        char *argv_netdev_pty[] = {
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
            "-o", "ServerAliveInterval=10",
            "-o", "ServerAliveCountMax=6",
            "-t", "-t",    /* 强制申请服务端 PTY，即使客户端无本地 tty */
            "-e", "none",  /* 禁用 SSH 转义字符，防止命令中 ~ 被拦截卡住 */
            "-p", port_str,
            userhost,
            NULL
        };
        {
            char **argv_sel = (net_device_mode == 2) ? argv_netdev_pty :
                              (net_device_mode == 1) ? argv_netdev :
                              argv_linux;
            execvpe("ssh", argv_sel, envp_askpass);
        }
        _exit(127);
    }

    /* ── 父进程 ── */
    close(out_pipe[1]);
    close(in_pipe[0]);
    session_pid_set(pid);

    if (!net_device_mode) {
    /* ── Linux 模式：一次性写入脚本 ── */
        const char *p   = script;
        size_t      rem = strlen(script);
        while (rem > 0) {
            ssize_t n = write(in_pipe[1], p, rem);
            if (n <= 0) break;
            p += n; rem -= (size_t)n;
        }
        close(in_pipe[1]);
        in_pipe[1] = -1;
    }

    /* ── 流式读取 + 实时边界解析（Linux）/ 提示符检测（网络设备）── */
    char *accum = malloc(SSH_OUTPUT_MAX);
    if (!accum) {
        kill(pid, SIGKILL);
        close(out_pipe[0]);
        waitpid(pid, NULL, 0);
        session_pid_set((pid_t)-1);
        snprintf(error_buf, error_buf_sz, "内存不足");
        goto cleanup;
    }

    /* ================================================================
     * 网络设备模式：交互式提示符检测，逐条发命令
     * ================================================================ */
    if (net_device_mode) {
        /* Step 1: 读取初始提示符。
         * 等待逻辑与每条命令完全相同：有数据到来就重置空闲计时器，
         * 空闲超过 idle_timeout_sec 秒才放弃。
         * 适用于 PTY 模式下脚本启动后需要较长时间才进入自定义视图的场景。 */
        size_t init_len = 0;
        {
            /* 单调时钟：墙上 / 空闲上限，防止对时或 trickle 输出绕过 */
            double t_init_start = monotonic_sec();
            double t_data       = t_init_start;
            for (;;) {
                struct timeval tv = { 0, 200000 };
                fd_set rset; FD_ZERO(&rset); FD_SET(out_pipe[0], &rset);
                if (select(out_pipe[0]+1, &rset, NULL, NULL, &tv) > 0) {
                    ssize_t nr = read(out_pipe[0], accum + init_len,
                                      SSH_OUTPUT_MAX - 1 - init_len);
                    if (nr > 0) { init_len += (size_t)nr; t_data = monotonic_sec(); }
                    else break;   /* EOF：SSH 已断开 */
                    if (monotonic_sec() - t_init_start >= (double)idle_timeout_sec)
                        break;
                } else {
                    /* 200ms 无新数据：检查是否已出现提示符 */
                    if (init_len > 0 && is_prompt(accum, init_len)) break;
                    {
                        double now = monotonic_sec();
                        if (now - t_init_start >= (double)idle_timeout_sec)
                            break;
                        /* 空闲超时（与每条命令超时一致） */
                        if (now - t_data >= (double)idle_timeout_sec) break;
                    }
                }
            }
        }
        if (!is_prompt(accum, init_len)) {
            /* 与 Linux/bash 模式一致：必须结束 SSH 子进程，否则 waitpid 可能永久阻塞，
             * SSE 无法收尾，前端表现为「超时/失败不生效」。 */
            kill(pid, SIGKILL);
            /* 未检测到提示符：连接失败或设备无响应 */
            if (init_len > 0) {
                size_t el = init_len < error_buf_sz-1 ? init_len : error_buf_sz-1;
                memcpy(error_buf, accum, el); error_buf[el] = '\0';
                size_t el2 = strlen(error_buf);
                while (el2 > 0 && (error_buf[el2-1]=='\n'||error_buf[el2-1]=='\r'))
                    error_buf[--el2] = '\0';
            } else {
                snprintf(error_buf, error_buf_sz, "连接后未检测到设备提示符");
            }
            goto net_done;
        }

        LOG_INFO("ssh_session_exec_stream(net): initial prompt detected, sending %d cmds",
                 cmd_count);

        /* Step 2: 逐条发送命令，等待提示符返回 */
        for (int i = 0; i < cmd_count; i++) {
            const char *cmd  = skip_shell_marker_prefix(commands[i]);
            size_t      clen = strlen(cmd);

            /* 发送命令 + 换行 */
            write(in_pipe[1], cmd, clen);
            write(in_pipe[1], "\n", 1);

            /* 读取命令输出，直到提示符出现或超时。
             * wall-clock 检查放在循环顶部，每 200ms 必然执行一次，
             * 无论 select 返回 0（无数据）还是 >0（有数据）都不会被绕过。
             * 这样即使 PTY 持续产生 ANSI 码/echo 导致 select 一直有数据，
             * 到时也能可靠触发，不会像分支内检查那样被跳过。 */
            size_t cmd_len       = 0;
            double t_cmd_start   = monotonic_sec();

            for (;;) {
                /* ── 墙上时钟（绝对超时）：循环顶优先检查 ── */
                if (monotonic_sec() - t_cmd_start >= (double)idle_timeout_sec) {
                    LOG_INFO("ssh_session_exec_stream(net): wall-clock timeout (%ds) on cmd[%d]",
                             idle_timeout_sec, i);
                    if (out_partial_buf && out_partial_sz > 0 && cmd_len > 0) {
                        size_t cp = cmd_len < out_partial_sz-1 ? cmd_len : out_partial_sz-1;
                        memcpy(out_partial_buf, accum, cp);
                        out_partial_buf[cp] = '\0';
                    }
                    timed_out = 1;
                    if (out_timeout_cmd_idx) *out_timeout_cmd_idx = i;
                    kill(pid, SIGKILL);
                    break;
                }

                struct timeval tv = { 0, 200000 };
                fd_set rset; FD_ZERO(&rset); FD_SET(out_pipe[0], &rset);
                int sel = select(out_pipe[0]+1, &rset, NULL, NULL, &tv);
                if (sel > 0) {
                    ssize_t nr = read(out_pipe[0], accum + cmd_len,
                                      SSH_OUTPUT_MAX - 1 - cmd_len);
                    if (nr <= 0) {
                        timed_out = 1;
                        if (out_timeout_cmd_idx) *out_timeout_cmd_idx = i;
                        break;
                    }   /* EOF */
                    cmd_len += (size_t)nr;
                } else {
                    /* 200ms 无新数据：检测提示符，超时由循环顶处理 */
                    if (cmd_len > 0 && is_prompt(accum, cmd_len)) break;
                }
            }

            accum[cmd_len] = '\0';
            char *clean = strip_cmd_output(accum, cmd_len);
            if (cb) cb(i, cmd, clean ? clean : "", 0, ud);
            free(clean);

            if (timed_out) break;
        }

net_done:
        if (in_pipe[1] >= 0) { close(in_pipe[1]); in_pipe[1] = -1; }
        free(accum);
        accum = NULL;
        goto stream_drain;
    }

    /* ================================================================
     * Linux/bash 模式：sentinel 边界解析
     * ================================================================ */
    {
    size_t accum_len  = 0;
    size_t scan_from  = 0;
    int    cmd_idx    = 0;

    for (;;) {
        if (cmd_idx >= cmd_count) break;

        fd_set rfds;
        struct timeval tv = { idle_timeout_sec, 0 };
        FD_ZERO(&rfds);
        FD_SET(out_pipe[0], &rfds);

        int sel = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel == 0) {
            LOG_INFO("ssh_session_exec_stream: idle timeout (%ds), killing pid=%d",
                     idle_timeout_sec, (int)pid);
            /* 保存被中断命令已采集到的部分输出 */
            if (out_partial_buf && out_partial_sz > 0 && accum_len > 0) {
                size_t cpy = accum_len < out_partial_sz - 1 ? accum_len : out_partial_sz - 1;
                memcpy(out_partial_buf, accum, cpy);
                out_partial_buf[cpy] = '\0';
                /* 去末尾空白 */
                while (cpy > 0 &&
                       (out_partial_buf[cpy-1] == '\n' || out_partial_buf[cpy-1] == '\r' ||
                        out_partial_buf[cpy-1] == ' '))
                    out_partial_buf[--cpy] = '\0';
            }
            kill(pid, SIGKILL);
            timed_out = 1;
            if (out_timeout_cmd_idx) *out_timeout_cmd_idx = cmd_idx;
            break;
        }
        if (sel < 0) break;

        char tmp[8192];
        ssize_t nr = read(out_pipe[0], tmp, sizeof(tmp));
        if (nr <= 0) break;  /* EOF */

        size_t copy = (size_t)nr;
        if (accum_len + copy >= SSH_OUTPUT_MAX - 1)
            copy = SSH_OUTPUT_MAX - 1 - accum_len;
        if (copy == 0) break;  /* buffer full */
        memcpy(accum + accum_len, tmp, copy);
        accum_len += copy;

        /* 扫描缓冲区中的边界标记，可能一次读到多条命令的结果 */
        while (cmd_idx < cmd_count) {
            const char *p   = accum + scan_from;
            const char *end = accum + accum_len;
            const char *marker = NULL;

            while (p < end) {
                /* 需要至少 blen+':'+digit+'\n' 字符才能判断 */
                if ((size_t)(end - p) < blen + 3) break;
                if (strncmp(p, boundary, blen) == 0 && p[blen] == ':') {
                    const char *r = p + blen + 1;
                    while (r < end && (unsigned char)*r >= '0' && (unsigned char)*r <= '9') r++;
                    if (r > p + blen + 1 && r < end && (*r == '\n' || *r == '\r')) {
                        marker = p; break;
                    }
                }
                p++;
            }

            if (!marker) {
                /* 留出 blen+2 字节防止边界被截断 */
                if (accum_len > blen + 2)
                    scan_from = accum_len - blen - 2;
                break;
            }

            /* 提取本条命令输出（去掉末尾换行） */
            size_t out_end = (size_t)(marker - accum);
            while (out_end > 0 &&
                   (accum[out_end-1] == '\n' || accum[out_end-1] == '\r'))
                out_end--;

            /* 解析退出码 */
            const char *after_colon = marker + blen + 1;
            int exit_code = atoi(after_colon);

            /* 定位边界行末尾 */
            const char *eol = after_colon;
            while (eol < end && *eol != '\n') eol++;
            if (eol < end && *eol == '\n') eol++;
            size_t consumed = (size_t)(eol - accum);

            /* 临时截断，调用回调 */
            char saved = accum[out_end];
            accum[out_end] = '\0';
            if (cb) cb(cmd_idx, commands[cmd_idx], accum, exit_code, ud);
            accum[out_end] = saved;

            /* 压缩缓冲区：将已处理数据移出，为下一条命令腾出空间 */
            size_t remaining = accum_len - consumed;
            if (remaining > 0)
                memmove(accum, accum + consumed, remaining);
            accum_len = remaining;
            scan_from = 0;
            cmd_idx++;
        }
    }

    /* 未找到任何边界 → SSH 连接/认证失败，读取剩余输出作为错误信息
     * 超时中断时跳过（已在上方保存部分输出，且进程已被杀死无需再读） */
    if (cmd_idx == 0 && !timed_out) {
        /* 排空管道（最多等 2 秒）以获取完整错误信息 */
        {
            char tmp2[4096];
            fd_set rfds2;
            while (accum_len < SSH_OUTPUT_MAX - 1) {
                struct timeval tv2 = { 2, 0 };   /* 每次重置，防止 select 消耗后成为忙轮询 */
                FD_ZERO(&rfds2);
                FD_SET(out_pipe[0], &rfds2);
                if (select(out_pipe[0] + 1, &rfds2, NULL, NULL, &tv2) <= 0) break;
                ssize_t nr2 = read(out_pipe[0], tmp2, sizeof(tmp2));
                if (nr2 <= 0) break;
                size_t cp2 = (size_t)nr2;
                if (accum_len + cp2 >= SSH_OUTPUT_MAX - 1)
                    cp2 = SSH_OUTPUT_MAX - 1 - accum_len;
                memcpy(accum + accum_len, tmp2, cp2);
                accum_len += cp2;
            }
        }
        if (accum_len > 0) {
            accum[accum_len] = '\0';
            size_t elen = accum_len < error_buf_sz - 1 ? accum_len : error_buf_sz - 1;
            memcpy(error_buf, accum, elen);
            error_buf[elen] = '\0';
            size_t el = strlen(error_buf);
            while (el > 0 && (error_buf[el-1]=='\n'||error_buf[el-1]=='\r'||
                               error_buf[el-1]==' '))
                error_buf[--el] = '\0';
        }
    }

    free(accum);
    accum = NULL;
    } /* end Linux/bash block */

stream_drain:
    /* 排空剩余输出，确保 SSH 子进程能正常退出
     * 注意：timeval 在 Linux 上会被 select 修改，必须每次迭代重置，
     * 否则降至 {0,0} 后 select 退化为非阻塞轮询，导致 CPU 占用飙升 */
    {
        char drain[4096];
        fd_set dfds;
        while (1) {
            struct timeval dtv = { 1, 0 };   /* 每次重置：等待最多 1 秒有新数据 */
            FD_ZERO(&dfds);
            FD_SET(out_pipe[0], &dfds);
            if (select(out_pipe[0] + 1, &dfds, NULL, NULL, &dtv) <= 0) break;
            if (read(out_pipe[0], drain, sizeof(drain)) <= 0) break;
        }
    }
    close(out_pipe[0]);
    session_pid_set((pid_t)-1);
    waitpid(pid, NULL, 0);
    if (out_timed_out) *out_timed_out = timed_out;

cleanup:
    free(script);
    if (has_pass_file)  unlink(pass_file);
    if (has_askpass)    unlink(askpass_file);
}

void ssh_batch_free(ssh_batch_t *b)
{
    if (!b) return;
    for (int i = 0; i < b->count; i++) {
        free(b->results[i].cmd);
        free(b->results[i].output);
        free(b->results[i].workdir);
    }
    free(b->results);
    free(b);
}
