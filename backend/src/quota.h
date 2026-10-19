/* Хранилище SQLite: учёт лимитов (счётчики запросов/токенов по окнам, паузы после 429, журнал попыток),
 * API-ключи и настройки. Всё разделено по пользователям (uid): у каждого свои ключи, лимиты и паузы —
 * лимиты провайдера считаются на ключ, а ключи у пользователей разные.
 * uid = 0 — «без авторизации» (тесты, старые данные до появления пользователей). */
#ifndef IAI_QUOTA_H
#define IAI_QUOTA_H

#include <stdint.h>

#include "config.h"

typedef struct quota_db quota_db;

typedef enum {
    QUOTA_OK = 0,
    QUOTA_LOCAL_LIMIT, /* исчерпан локально отслеживаемый лимит (rpm/rpd/rpmo/tpd) */
    QUOTA_COOLDOWN     /* модель на паузе после 429/ошибок */
} quota_state;

typedef struct {
    int     req_minute, req_day, req_month;
    long    tokens_day;
    int64_t cooldown_until; /* 0 — нет паузы */
    char    cooldown_reason[128];
} quota_status;

quota_db *quota_open(const char *path, char *err, size_t errlen);
void      quota_close(quota_db *q);

/* Можно ли сейчас вызвать модель. threshold_pct (50–100) — переключаться заранее, когда израсходовано
 * столько процентов локального лимита. reason — человекочитаемая причина отказа. */
quota_state quota_check(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, int threshold_pct, char *reason, size_t rlen);

/* Загруженность модели по локальным лимитам, 0–100. -1 — лимиты не заданы. Нужна стратегии «Свободнее». */
int quota_load_pct(quota_db *q, int64_t uid, const model_cfg *m, int64_t now);

/* Учесть успешный запрос. */
void quota_record(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, long tokens);

/* Поставить модель на паузу до until (unix-время). */
void quota_set_cooldown(quota_db *q, int64_t uid, const char *model_id, int64_t until, const char *reason);

/* Снять паузу и обнулить счётчики модели. */
void quota_reset(quota_db *q, int64_t uid, const char *model_id);

/* Текущее состояние для /api/models и статистики. */
void quota_get_status(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, quota_status *st);

/* Запись в журнал попыток. */
void quota_log_attempt(quota_db *q, int64_t uid, int64_t now, const char *request_id, const char *model_id, const char *kind,
                       long http_status, const char *message);

/* Снять только паузу (счётчики не трогаем) — например, после смены ключа. */
void quota_clear_cooldown(quota_db *q, int64_t uid, const char *model_id);

/* ---- API-ключи пользователя (из «Настроек» сайта) ---- */
/* Сохранить ключ провайдера; key == NULL или "" — удалить. Шифруется, если пользователь включил шифрование. 0 — успех. */
int  store_key_set(quota_db *q, int64_t uid, const char *provider, const char *key);
/* Прочитать ключ. 1 — найден (записан в buf), 0 — нет. */
int  store_key_get(quota_db *q, int64_t uid, const char *provider, char *buf, size_t len);
/* Имена провайдеров с сохранёнными ключами. Возвращает количество (не больше max). */
int  store_key_list(quota_db *q, int64_t uid, char names[][64], int max);
/* Сырое значение ключа как оно лежит в БД (для тестов и диагностики). */
int  store_key_get_raw(quota_db *q, int64_t uid, const char *provider, char *buf, size_t len);

/* Шифрование ключей пользователя. migrate=1 — перезаписать уже сохранённые ключи в новом виде.
 * Возвращает число обработанных ключей или -1 (шифрование недоступно). */
int  quota_set_encryption(quota_db *q, int64_t uid, int on, int migrate);
int  quota_encryption_on(quota_db *q, int64_t uid);

/* Хранилище «имя → значение» (настройки роутера). kv_get возвращает malloc-строку или NULL. */
char *quota_kv_get(quota_db *q, const char *name);
void  quota_kv_set(quota_db *q, const char *name, const char *value);

/* Начало следующих суток UTC после now — когда сбрасываются суточные лимиты. */
int64_t quota_next_day(int64_t now);

/* ---- низкоуровневый доступ для модулей auth.c и stats.c (та же БД и та же блокировка) ---- */
struct sqlite3;
struct sqlite3 *quota_sqlite(quota_db *q);
void quota_lock(quota_db *q);
void quota_unlock(quota_db *q);
/* id первого зарегистрированного пользователя (владелец сервера) или 0. */
int64_t quota_owner_uid(quota_db *q);
void    quota_set_owner_uid(quota_db *q, int64_t uid);

#endif
