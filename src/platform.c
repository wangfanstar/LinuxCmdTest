#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

int platform_cpu_cores(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return (c > 0) ? (int)c : 2;
#else
    return 2;
#endif
}

void platform_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(1);
    }
#endif
}

void platform_net_end(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

void platform_format_sock_err(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return;
    }
#ifdef _WIN32
    snprintf(buf, buflen, "winsock error %d", (int)WSAGetLastError());
#else
    snprintf(buf, buflen, "%s", strerror(errno));
#endif
}

ssize_t http_sock_send_all(http_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(s, p + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR) {
            return -1;
        }
        if (n == 0) {
            return (ssize_t)sent;
        }
#else
        ssize_t n = write((int)s, p + sent, len - sent);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return (ssize_t)sent;
        }
#endif
        sent += (size_t)n;
    }
    return (ssize_t)len;
}

ssize_t http_sock_recv_buf(http_sock_t s, void *buf, size_t len)
{
#ifdef _WIN32
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    int n = recv(s, (char *)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        return -1;
    }
    return n;
#else
    return read((int)s, buf, len);
#endif
}

int http_sock_shutdown_rw(http_sock_t s)
{
#ifdef _WIN32
    if (shutdown(s, SD_BOTH) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
#else
    if (shutdown((int)s, SHUT_RDWR) < 0) {
        return -1;
    }
    return 0;
#endif
}

int http_sock_close(http_sock_t s)
{
#ifdef _WIN32
    if (closesocket(s) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
#else
    return close((int)s);
#endif
}

void platform_gmtime_utc(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    gmtime_s(out, t);
#else
    gmtime_r(t, out);
#endif
}

void platform_localtime_wall(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    localtime_s(out, t);
#else
    localtime_r(t, out);
#endif
}

const char *platform_strcasestr(const char *haystack, const char *needle)
{
    if (!needle[0]) {
        return haystack;
    }
    for (; *haystack; haystack++) {
        const char *p = haystack;
        const char *q = needle;
        while (*p && *q && tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
            p++;
            q++;
        }
        if (!*q) {
            return haystack;
        }
    }
    return NULL;
}

int platform_mkdir(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

int platform_process_kill(int pid)
{
    if (pid <= 0) {
        return -1;
    }
#ifdef _WIN32
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (!h) {
            return -1;
        }
        BOOL ok = TerminateProcess(h, 1);
        CloseHandle(h);
        return ok ? 0 : -1;
    }
#else
    if (kill((pid_t)pid, SIGKILL) == 0) {
        return 0;
    }
    return -1;
#endif
}
