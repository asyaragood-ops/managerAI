/* Умный роутер: каскадный вызов моделей с переключением при исчерпании лимитов. */
#ifndef IAI_ROUTER_H
#define IAI_ROUTER_H

#include "config.h"
#include "providers/provider.h"
#include "quota.h"

#define ROUTER_MAX_ATTEMPTS 64

/* Одна попытка в каскаде — уходит на фронтенд, чтобы показать, почему было переключение. */
typedef struct {
    const model_cfg *model;
    result_kind      kind;
    long             http_status;
    long             latency_ms;
    char             message[512];
} attempt_info;

typedef struct {
    int              ok;
    char             request_id[24];
    char            *text;          /* malloc */
    media_kind       kind;          /* что просили: текст, картинку, озвучку, видео */
    unsigned char   *media;         /* malloc: сгенерированный файл */
    size_t           media_len;
    char             media_mime[64];
    char             media_file[64]; /* имя файла в media_dir: <request_id>.png */
    char            *prompt;        /* malloc: промпт, ушедший медиа-модели */
    const model_cfg *prompt_model;  /* текстовая модель, которая его подготовила (или NULL) */
    const model_cfg *model;         /* кто ответил */
    int              switched;      /* ответил не первый ИИ из каскада */
    long             tokens_in, tokens_out;
    long             latency_ms;
    attempt_info     attempts[ROUTER_MAX_ATTEMPTS];
    int              nattempts;
    char             error[256];
} router_reply;

typedef struct {
    const app_config *cfg;
    quota_db         *quota;
    int64_t           uid;   /* чей запрос: ключи, лимиты, настройки и статистика у каждого пользователя свои */
} router;

/* Обработать запрос. prefer — id модели, которую пользователь закрепил в чате ("auto"/NULL — каскад по порядку).
 * skill — специализация чата (модели с этим навыком идут в каскаде раньше). */
void router_chat(router *r, const chat_msg *msgs, size_t n, const char *prefer, const char *skill, router_reply *out);
void router_reply_free(router_reply *out);

/* Ключ для модели: сначала из «Настроек» пользователя (SQLite), затем из окружения/.env — но только
 * владельцу сервера (первому зарегистрированному) или всем, если в config.json "env_keys": "all".
 * Возвращает указатель на buf/строку окружения или NULL. source — "settings" | "env" | "mock" | "none". */
const char *router_api_key(router *r, const model_cfg *m, char *buf, size_t len, const char **source);

/* Что хочет пользователь по тексту сообщения и специализации чата (экспортировано для тестов). */
media_kind router_detect_kind(const char *text, const char *skill);

/* Папка с файлами пользователя: <media_dir>/u<uid>. */
void router_media_dir(const app_config *cfg, int64_t uid, char *out, size_t n);

/* JSON-ответ для фронтенда. */
char *router_reply_json(const router_reply *out);

/* Сколько секунд держать модель на паузе после ошибки (0 — без паузы). Экспортировано для тестов. */
long router_cooldown_seconds(const call_result *res, int64_t now);

#endif
