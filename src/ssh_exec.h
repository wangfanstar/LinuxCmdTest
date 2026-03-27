#ifndef SSH_EXEC_H
#define SSH_EXEC_H

#define SSH_MAX_CMDS     64
#define SSH_OUTPUT_MAX   (512 * 1024)   /* 单条命令输出上限 512KB */

typedef struct {
    char *cmd;        /* 命令字符串（heap） */
    char *output;     /* 输出内容（heap，stdout+stderr） */
    char *workdir;    /* 命令执行后的当前目录（heap，可为空串） */
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

/* 强制终止当前正在运行的 SSH 会话（线程安全） */
void ssh_cancel_current(void);

/*
 * 流式执行：每条命令完成后立即回调，无需等待全部命令结束。
 * cb(idx, cmd, output, exit_code, ud) 在持有 output 期间同步调用，
 * 调用返回后 output 内存即释放，cb 内部如需保留须自行复制。
 * error_buf 在连接失败时填写错误信息；正常结束时为空字符串。
 */
typedef void (*ssh_stream_cb_t)(int idx, const char *cmd,
                                 const char *output, int exit_code,
                                 void *ud);

void ssh_session_exec_stream(const char *host, int port,
                              const char *user, const char *pass,
                              char **commands, int cmd_count,
                              ssh_stream_cb_t cb, void *ud,
                              char *error_buf, size_t error_buf_sz);

#endif /* SSH_EXEC_H */
