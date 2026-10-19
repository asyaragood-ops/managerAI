#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <signal.h>
#  include <sys/time.h>
#  include <time.h>
#endif

int plat_net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN); /* запись в закрытый сокет не должна ронять сервер */
    return 0;
#endif
}

void plat_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void plat_sock_timeouts(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

int64_t plat_now(void) { return (int64_t)time(NULL); }

int64_t plat_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 нс с 1601 */
    return (int64_t)(t / 10000ULL - 11644473600000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

void plat_gmtime(time_t t, struct tm *out) {
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}

void plat_setenv_if_unset(const char *name, const char *value) {
    const char *cur = getenv(name);
    if (cur && *cur) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int mkdir_one(const char *p) {
#ifdef _WIN32
    int r = _mkdir(p);
#else
    int r = mkdir(p, 0755);
#endif
    return (r == 0 || errno == EEXIST) ? 0 : -1;
}

int plat_mkdirs(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
            if (!(i == 2 && buf[1] == ':')) /* пропускаем «C:» */
                if (mkdir_one(buf) != 0) return -1;
            buf[i] = c;
        }
    }
    return mkdir_one(buf);
}

void plat_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {}
#endif
}
