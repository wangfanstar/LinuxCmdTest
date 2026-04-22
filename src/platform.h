/* 跨平台：Windows（Winsock2）与 POSIX 的 HTTP 套接字抽象。 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>

#ifdef _MSC_VER
#include <BaseTsd.h> /* ssize_t */
#else
#include <sys/types.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h> /* for read/write on file descriptors, not HTTP sockets */
typedef SOCKET http_sock_t;
#define HTTP_SOCK_INVALID INVALID_SOCKET
#else
#include <sys/socket.h>
#include <unistd.h>
typedef int http_sock_t;
#define HTTP_SOCK_INVALID (-1)
#endif

#include <time.h>
#include <stdint.h>

void platform_net_init(void);
void platform_net_end(void);
int  platform_cpu_cores(void);

static inline int http_sock_is_valid(http_sock_t s) {
#ifdef _WIN32
    return s != INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

/* 向 HTTP 套接字发送/接收；文件 fd 上仍用 read/write/close */
ssize_t http_sock_send_all(http_sock_t s, const void *buf, size_t len);
ssize_t http_sock_recv_buf(http_sock_t s, void *buf, size_t len);
int http_sock_shutdown_rw(http_sock_t s);
int http_sock_close(http_sock_t s);

/* 将最近一次的套接字/网络相关错误格式化为可打印信息（如日志） */
void platform_format_sock_err(char *buf, size_t buflen);

/* 关闭监听套接字 */
#define http_sock_listen_close http_sock_close

/* gmtime_r / localtime_r 的可移植包装 */
void platform_gmtime_utc(const time_t *t, struct tm *out);
void platform_localtime_wall(const time_t *t, struct tm *out);

/* 终止其它进程（用于 /api/kill）；Windows 下用 TerminateProcess */
int platform_process_kill(int pid);

/* 创建单级目录（非递归）；与 POSIX mkdir(path, 0755) 行为对应，失败时设 errno */
int platform_mkdir(const char *path);

/* strcasestr（MSVC 无 GNU 扩展） */
const char *platform_strcasestr(const char *haystack, const char *needle);

#ifdef _MSC_VER
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#endif /* PLATFORM_H */
