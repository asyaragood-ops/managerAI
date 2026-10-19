/* Минимальный HTTP/1.1-сервер для API фронтенда. */
#ifndef IAI_SERVER_H
#define IAI_SERVER_H

#include "config.h"
#include "quota.h"
#include "router.h"

typedef struct {
    const app_config *cfg;
    router           *rt;
    quota_db         *quota;
} server_ctx;

/* Запустить сервер (блокирует до server_stop / Ctrl+C). 0 — штатная остановка. */
int  server_run(server_ctx *ctx);
void server_stop(void);

typedef struct {
    const char *method;
    const char *path;    /* с ?query */
    const char *body;
    const char *cookie;  /* значение заголовка Cookie (может быть NULL) */
    int         is_local;
} http_request;

typedef struct {
    char *body;            /* malloc: JSON-ответ */
    char  set_cookie[320]; /* значение заголовка Set-Cookie или "" */
} http_reply;

/* Обработка одного API-запроса — вынесена для тестов без сети. Все разделы, кроме /api/health и /api/auth/…,
 * требуют входа (cookie сессии). Возвращает HTTP-код. */
int  server_handle(server_ctx *ctx, const http_request *rq, http_reply *out);

#endif
