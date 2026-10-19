/* HTTPS-клиент на libcurl для запросов к провайдерам ИИ. */
#ifndef IAI_HTTP_CLIENT_H
#define IAI_HTTP_CLIENT_H

#include "strbuf.h"

typedef struct {
    long   status;          /* HTTP-код; 0 — запрос не дошёл (сеть/таймаут) */
    strbuf body;
    long   retry_after_s;   /* из заголовка Retry-After (секунды), 0 — нет */
    char   error[256];      /* описание сетевой ошибки */
    char   content_type[128]; /* Content-Type ответа (после редиректов) */
} http_response;

int  http_global_init(void);
void http_global_cleanup(void);

/* POST с JSON-телом. headers — массив строк "Name: value". Возвращает 0, если получен любой HTTP-ответ. */
int  http_post_json(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                    http_response *out);
/* GET (с переходом по редиректам). max_bytes — предел размера ответа (картинки, видео). */
int  http_get(const char *url, const char *const *headers, int nheaders, long timeout_ms, size_t max_bytes,
              http_response *out);
/* POST с JSON-телом и увеличенным пределом ответа (ответы с картинками в base64). */
int  http_post_json_big(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                        size_t max_bytes, http_response *out);
void http_response_free(http_response *r);

#endif
