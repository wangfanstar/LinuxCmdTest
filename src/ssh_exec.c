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

/* host/user 仅允许安全字符（防止 shell 注入） */
static int validate_safe(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == '@' ||
            c == '[' || c == ']' || c == ':')   /* IPv6 支持 */
            continue;
        return 0;
    }
    return 1;
}

/* 将命令中的单引号转义为 '\'' 以便嵌入单引号包裹的 shell 参数 */
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
/*  临时密码文件                                                        */
/* ------------------------------------------------------------------ */

static int create_pass_file(const char *pass, char *path_out, size_t path_len)
{
    snprintf(path_out, path_len, "/tmp/.swps_XXXXXX");
    int fd = mkstemp(path_out);
    if (fd < 0) return -1;
    fchmod(fd, 0600);              /* 仅 owner 可读 */
    write(fd, pass, strlen(pass));
    close(fd);
    return 0;
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

    /* 校验 host / user */
    if (!validate_safe(host)) {
        snprintf(b->error, sizeof(b->error),
                 "非法主机名: %s（仅允许字母、数字及 . - _ : []）", host);
        return b;
    }
    if (!validate_safe(user)) {
        snprintf(b->error, sizeof(b->error),
                 "非法用户名: %s", user);
        return b;
    }
    if (port <= 0 || port > 65535) {
        snprintf(b->error, sizeof(b->error), "非法端口: %d", port);
        return b;
    }

    /* 检查 sshpass 是否可用 */
    if (system("which sshpass > /dev/null 2>&1") != 0) {
        snprintf(b->error, sizeof(b->error),
                 "未找到 sshpass，请安装: sudo yum install sshpass  或  sudo apt install sshpass");
        return b;
    }

    /* 创建临时密码文件 */
    char pass_file[64];
    if (create_pass_file(pass, pass_file, sizeof(pass_file)) < 0) {
        snprintf(b->error, sizeof(b->error),
                 "创建临时密码文件失败: %s", strerror(errno));
        return b;
    }

    /* 分配结果数组 */
    b->results = calloc(cmd_count, sizeof(ssh_cmd_result_t));
    if (!b->results) {
        unlink(pass_file);
        free(b);
        return NULL;
    }
    b->count = cmd_count;

    static const char *SSH_OPTS =
        "-o StrictHostKeyChecking=accept-new "
        "-o ConnectTimeout=15 "
        "-o BatchMode=no "
        "-o PasswordAuthentication=yes "
        "-o PubkeyAuthentication=no ";

    for (int i = 0; i < cmd_count; i++) {
        b->results[i].cmd = strdup(commands[i]);

        /* 转义命令中的单引号 */
        char esc[8192];
        escape_sq(commands[i], esc, sizeof(esc));

        /* 构造完整 shell 命令 */
        char shell_cmd[10240];
        snprintf(shell_cmd, sizeof(shell_cmd),
                 "timeout 60 sshpass -f '%s' ssh %s -p %d '%s'@'%s' '%s' 2>&1",
                 pass_file, SSH_OPTS, port, user, host, esc);

        LOG_INFO("ssh_exec[%d]: %s@%s:%d  cmd=%s", i, user, host, port, commands[i]);

        FILE *fp = popen(shell_cmd, "r");
        if (!fp) {
            b->results[i].output    = strdup("popen() 失败");
            b->results[i].exit_code = -1;
            continue;
        }

        /* 读取输出 */
        char *out = malloc(SSH_OUTPUT_MAX);
        if (!out) {
            pclose(fp);
            b->results[i].output    = strdup("内存分配失败");
            b->results[i].exit_code = -1;
            continue;
        }

        size_t total = 0;
        char   tmp[8192];
        size_t nr;
        while ((nr = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
            size_t copy = nr;
            if (total + copy >= SSH_OUTPUT_MAX - 1)
                copy = SSH_OUTPUT_MAX - 1 - total;
            memcpy(out + total, tmp, copy);
            total += copy;
            if (total >= SSH_OUTPUT_MAX - 1) break;
        }
        out[total] = '\0';

        int status = pclose(fp);
        b->results[i].output    = out;
        b->results[i].exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        LOG_INFO("ssh_exec[%d] exit=%d output_len=%zu",
                 i, b->results[i].exit_code, total);
    }

    unlink(pass_file);
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
