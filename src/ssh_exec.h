#ifndef SSH_EXEC_H
#define SSH_EXEC_H

#include <stddef.h>  /* size_t */

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

/* 所有命令共享同一 SSH 会话（cd/env 持久，推荐）
 * idle_timeout_sec：单条命令无任何输出超过此秒数则强制终止；
 *                   传 0 使用默认值（300s）。 */
ssh_batch_t *ssh_session_exec(const char *host, int port,
                               const char *user, const char *pass,
                               char **commands, int cmd_count,
                               int idle_timeout_sec);

void ssh_batch_free(ssh_batch_t *b);

/* 强制终止当前正在运行的 SSH 会话（线程安全） */
void ssh_cancel_current(void);

/*
 * 流式执行：每条命令完成后立即回调，无需等待全部命令结束。
 * cb(idx, cmd, output, exit_code, prompt_after, ud) 在持有 output 期间同步调用，
 * 调用返回后 output 内存即释放，cb 内部如需保留须自行复制。
 * prompt_after：PTY/网络设备模式下，本条输出结束后远端显示的提示符行（已去 ANSI）；
 *   Linux 脚本模式通常为空串。idx == -1 时表示会话就绪后的首条提示符（仅 prompt_after 有效）。
 * error_buf 在连接失败时填写错误信息；正常结束时为空字符串。
 */
typedef void (*ssh_stream_cb_t)(int idx, const char *cmd,
                                 const char *output, int exit_code,
                                 const char *prompt_after,
                                 void *ud);

/* idle_timeout_sec：单条命令的绝对超时秒数（wall-clock，从发出命令时起计）；
 *   传 0 使用默认值（300s）。PTY/NET 模式下每 200ms 在循环顶检查，
 *   无论是否有数据持续流入都能可靠触发，不因 PTY echo/ANSI 码而被绕过。
 * out_timed_out：非 NULL 时，超时中断置 1，正常/连接失败置 0。
 * out_timeout_cmd_idx：非 NULL 时，超时中断填入被中断命令的 0-based 下标；
 *   未超时或非命令级中断时置为 -1。
 * out_partial_buf/out_partial_sz：非 NULL 时，超时中断后填入被中断命令
 *   的已采集输出（不完整），正常结束置为空字符串。
 * net_device_mode：非 0 时使用交互式提示符检测模式，逐条发命令并等待提示符；
 *   为 0 时使用 bash -s 脚本模式（适用于 Linux/Unix 服务器）。 */
void ssh_session_exec_stream(const char *host, int port,
                              const char *user, const char *pass,
                              char **commands, int cmd_count,
                              ssh_stream_cb_t cb, void *ud,
                              char *error_buf, size_t error_buf_sz,
                              int idle_timeout_sec,
                              int *out_timed_out,
                              int *out_timeout_cmd_idx,
                              char *out_partial_buf, size_t out_partial_sz,
                              int net_device_mode);

#endif /* SSH_EXEC_H */
