#ifndef SSH_EXEC_H
#define SSH_EXEC_H

#define SSH_MAX_CMDS     64
#define SSH_OUTPUT_MAX   (512 * 1024)   /* 单条命令输出上限 512KB */

typedef struct {
    char *cmd;        /* 命令字符串（heap） */
    char *output;     /* 输出内容（heap，stdout+stderr） */
    int   exit_code;  /* 退出码；-1 表示执行失败 */
} ssh_cmd_result_t;

typedef struct {
    ssh_cmd_result_t *results;
    int               count;
    char              error[512];   /* 全局错误（连接失败等） */
} ssh_batch_t;

/*
 * 通过 sshpass + ssh 批量执行命令。
 * 返回堆分配结果，调用方须调用 ssh_batch_free()。
 * 返回 NULL 仅在内存分配失败时发生。
 */
/* 每条命令独立 SSH 连接（无状态，cd 不持久） */
ssh_batch_t *ssh_batch_exec(const char *host, int port,
                             const char *user, const char *pass,
                             char **commands, int cmd_count);

/* 所有命令共享同一 SSH 会话（cd/env 持久，推荐） */
ssh_batch_t *ssh_session_exec(const char *host, int port,
                               const char *user, const char *pass,
                               char **commands, int cmd_count);

void ssh_batch_free(ssh_batch_t *b);

#endif /* SSH_EXEC_H */
