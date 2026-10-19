#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "platform.h"

static log_level g_level = LOG_INFO;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void log_set_level(log_level lvl) { g_level = lvl; }

void log_msg(log_level lvl, const char *tag, const char *fmt, ...) {
    if (lvl < g_level) return;
    static const char *names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    int64_t ms = plat_now_ms();
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    plat_gmtime(t, &tm);
    pthread_mutex_lock(&g_mu);
    fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03dZ %s [%s] ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000), names[lvl], tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_mu);
}
