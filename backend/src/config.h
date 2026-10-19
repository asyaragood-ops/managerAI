/* Конфигурация: сервер + каскад моделей (config.json) и секреты из окружения/.env. */
#ifndef IAI_CONFIG_H
#define IAI_CONFIG_H

#include <stddef.h>

#define CFG_MAX_SKILLS 12
#define CFG_MAX_MODELS 512

/* Локальные лимиты модели (0 — не ограничено). Нужны, чтобы не дожидаться 429 от провайдера. */
typedef struct {
    int  rpm;   /* запросов в минуту */
    int  rpd;   /* запросов в сутки (UTC) */
    int  rpmo;  /* запросов в месяц */
    long tpd;   /* токенов в сутки */
} limits_cfg;

typedef struct {
    char id[64];           /* уникальный id в каскаде (совпадает с id каталога фронтенда, если есть) */
    char label[64];        /* короткое имя для бэйджа: «Gemini», «GPT-4o» */
    char provider[32];     /* gemini | openai | anthropic | cloudflare | pollinations | mock */
    char kind[8];          /* что генерирует: chat | image | audio | video */
    int  keyless;          /* ключ не нужен (Pollinations) */
    char model[128];       /* имя модели у провайдера */
    char base_url[256];    /* для OpenAI-совместимых: https://api.groq.com/openai/v1 и т.п. */
    char key_name[64];     /* имя провайдера, под которым ключ сохраняется в «Настройках» сайта */
    char api_key_env[64];  /* запасной вариант: переменная окружения / .env */
    char tier[16];         /* free | paid */
    char skills[CFG_MAX_SKILLS][16];
    int  nskills;
    int  max_tokens;
    char max_tokens_field[32]; /* openai: max_tokens или max_completion_tokens */
    int  enabled;
    int  priority;   /* «качество» для стратегии «Качественнее»: 1 — сильнейшие, 3 — лёгкие */
    int  auto_added; /* модель из каталога: участвует в каскаде, только если для провайдера есть ключ */
    limits_cfg limits;
    /* Только для provider=mock: ok | rate_limit | quota | server_error | auth_error */
    char mock_behavior[32];
} model_cfg;

typedef struct {
    char   host[64];
    int    port;
    char   db_path[256];
    long   timeout_ms;      /* таймаут одного запроса к провайдеру */
    int    max_history;     /* сколько последних сообщений передавать модели */
    char   cors_origin[128];
    char   system_prompt[2048];
    char   media_dir[256];
    char   env_keys[8];        /* кому доступны ключи из .env: owner (первый пользователь) | all | none */
    int    allow_registration; /* можно ли регистрироваться новым пользователям */
    int    cookie_secure;      /* cookie сессии только по HTTPS (включите за reverse-proxy с TLS) */    /* куда сохранять картинки, аудио и видео (по умолчанию рядом с БД: data/media) */
    char   catalog_path[256]; /* каталог моделей для автоматического каскада (catalog.json) */
    size_t nexplicit;         /* сколько моделей задано в config.json явно (они идут первыми) */
    model_cfg *models;      /* порядок = порядок каскада */
    size_t nmodels;
} app_config;

/* Загрузить .env (KEY=VALUE), не перезаписывая уже заданные переменные. Отсутствие файла — не ошибка. */
void config_load_env(const char *path);

/* Загрузить config.json. 0 — успех; иначе текст ошибки в err. */
int  config_load(const char *path, app_config *cfg, char *err, size_t errlen);
void config_free(app_config *cfg);

/* Добавить в каскад модели из каталога (catalog.json). Возвращает число добавленных, -1 — ошибка JSON. */
int  config_load_catalog(app_config *cfg, const char *path, char *err, size_t errlen);

/* Ключ модели из окружения/.env (NULL, если не задан). Для mock всегда "mock".
 * Ключи из «Настроек» сайта хранятся в SQLite — см. router_api_key(). */
const char *config_api_key(const model_cfg *m);

int  model_has_skill(const model_cfg *m, const char *skill);

/* Что генерирует модель. */
typedef enum { KIND_CHAT = 0, KIND_IMAGE, KIND_AUDIO, KIND_VIDEO } media_kind;
media_kind  model_kind(const model_cfg *m);
const char *kind_code(media_kind k);
/* "image" → KIND_IMAGE; неизвестное → -1 */
int         kind_parse(const char *s);

#endif
