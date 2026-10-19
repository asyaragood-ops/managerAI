/* Настройки роутера со страницы «Настройки» сайта — у каждого пользователя свои. Хранятся в SQLite. */
#ifndef IAI_SETTINGS_H
#define IAI_SETTINGS_H

#include "config.h"
#include "quota.h"
#include "strbuf.h"

typedef enum { STRATEGY_CHEAP = 0, STRATEGY_FREE, STRATEGY_QUALITY } strategy_t;

typedef struct {
    int        auto_switch;   /* 0 — пробуем только первую подходящую модель, без резервных */
    int        threshold;     /* 50–100: переключаться заранее, когда израсходовано столько % локального лимита */
    strategy_t strategy;      /* cheap — дешевле, free — свободнее, quality — качественнее */
    int        keep_context;  /* 0 — резервной модели отдаём только последнее сообщение */
    int        notify_switch; /* только для фронтенда, сервер хранит для синхронизации */
    int        encrypt_keys;  /* шифровать API-ключи в базе */
    long long  daily_tokens;  /* личный дневной лимит токенов; 0 — без лимита */
    int        ndisabled;
    char       disabled[CFG_MAX_MODELS][64]; /* id моделей, выключенных на странице «Модели» */
} router_settings;

void settings_defaults(router_settings *s);

/* Настройки пользователя uid (из БД или значения по умолчанию). */
void settings_get(quota_db *q, int64_t uid, router_settings *out);

/* Применить JSON со страницы «Настройки» (частичный: отсутствующие поля не меняются), сохранить в БД.
 * При смене encrypt_keys ключи пользователя перешифровываются. 0 — успех, иначе текст ошибки в err. */
int  settings_apply_json(quota_db *q, int64_t uid, const char *json_text, char *err, size_t errlen);

/* JSON настроек пользователя для GET /api/settings. */
void settings_to_json(quota_db *q, int64_t uid, strbuf *b);

int  settings_is_disabled(const router_settings *s, const char *model_id);
const char *strategy_code(strategy_t s);

#endif
