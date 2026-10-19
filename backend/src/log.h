/* Простой потокобезопасный лог в stderr. */
#ifndef IAI_LOG_H
#define IAI_LOG_H

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level;

void log_set_level(log_level lvl);
void log_msg(log_level lvl, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LOGD(tag, ...) log_msg(LOG_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LOG_INFO, tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LOG_WARN, tag, __VA_ARGS__)
#define LOGE(tag, ...) log_msg(LOG_ERROR, tag, __VA_ARGS__)

#endif
