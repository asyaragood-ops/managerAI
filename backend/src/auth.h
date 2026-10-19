/* Пользователи и сессии: регистрация, вход, выход.
 *
 *  - пароль хранится как PBKDF2-HMAC-SHA256 (600 000 итераций, случайная соль), сравнение за постоянное время;
 *  - сессия — случайный 256-битный токен в cookie HttpOnly; в БД лежит только его SHA-256;
 *  - после 5 неверных паролей подряд вход для логина блокируется (1 мин, затем дольше, максимум 15 мин);
 *  - первый зарегистрированный пользователь получает ключи и настройки, сохранённые до появления аккаунтов. */
#ifndef IAI_AUTH_H
#define IAI_AUTH_H

#include <stdint.h>

#include "quota.h"

#define AUTH_COOKIE      "iai_session"
#define AUTH_SESSION_TTL (30L * 86400) /* 30 дней, продлевается при использовании */
#define AUTH_TOKEN_LEN   64            /* hex */

typedef struct {
    int64_t id;
    char    login[64];
    int     tz_min; /* смещение часового пояса пользователя от UTC, минуты (Москва: 180) */
} auth_user;

/* Для тестов: меньше итераций PBKDF2 (по умолчанию 600 000). */
void auth_set_iterations(int n);

/* 0 — успех; иначе текст ошибки (логин занят, слабый пароль…). */
int auth_register(quota_db *q, const char *login, const char *password, int tz_min, auth_user *out, char *err, size_t errlen);

/* 0 — успех; -1 — неверный логин/пароль; -2 — временная блокировка (retry_after — секунд до разблокировки). */
int auth_login(quota_db *q, const char *login, const char *password, int tz_min, auth_user *out, long *retry_after, char *err,
               size_t errlen);

/* Новая сессия: token — 64 hex-символа + '\0'. 0 — успех. */
int auth_session_create(quota_db *q, int64_t uid, char token[AUTH_TOKEN_LEN + 1]);
/* Пользователь по токену сессии. 1 — сессия действительна. */
int auth_session_user(quota_db *q, const char *token, auth_user *out);
void auth_session_delete(quota_db *q, const char *token);

/* Обновить часовой пояс пользователя (приходит с фронтенда). */
void auth_set_tz(quota_db *q, int64_t uid, int tz_min);

/* Достать токен из заголовка Cookie. 1 — найден и корректен. */
int auth_cookie_token(const char *cookie_header, char token[AUTH_TOKEN_LEN + 1]);

/* Проверки формата (экспортированы для тестов). 0 — ок. */
int auth_check_login(const char *login, char *norm, size_t nlen, char *err, size_t errlen);
int auth_check_password(const char *password, const char *login, char *err, size_t errlen);

#endif
