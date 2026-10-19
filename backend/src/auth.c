#include "auth.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#include "json.h"
#include "log.h"
#include "platform.h"
#include "strbuf.h"

#define SALT_LEN 16
#define HASH_LEN 32
#define MAX_FAILS 5

static int g_iterations = 600000;

void auth_set_iterations(int n) { g_iterations = n > 0 ? n : 600000; }

static void to_hex(const unsigned char *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = h[in[i] >> 4];
        out[2 * i + 1] = h[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int from_hex(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i] = (unsigned char)v;
    }
    return 0;
}

static int pbkdf2(const char *password, const unsigned char *salt, int iter, unsigned char *out) {
    return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, SALT_LEN, iter, EVP_sha256(), HASH_LEN, out) == 1 ? 0 : -1;
}

/* "pbkdf2_sha256$<итерации>$<соль hex>$<хеш hex>" */
static int hash_password(const char *password, char *out, size_t outlen) {
    unsigned char salt[SALT_LEN], hash[HASH_LEN];
    if (RAND_bytes(salt, SALT_LEN) != 1 || pbkdf2(password, salt, g_iterations, hash) != 0) return -1;
    char sh[SALT_LEN * 2 + 1], hh[HASH_LEN * 2 + 1];
    to_hex(salt, SALT_LEN, sh);
    to_hex(hash, HASH_LEN, hh);
    snprintf(out, outlen, "pbkdf2_sha256$%d$%s$%s", g_iterations, sh, hh);
    OPENSSL_cleanse(hash, sizeof hash);
    return 0;
}

static int verify_password(const char *password, const char *stored) {
    int iter = 0;
    char sh[SALT_LEN * 2 + 1], hh[HASH_LEN * 2 + 1];
    if (sscanf(stored, "pbkdf2_sha256$%d$%32[0-9a-f]$%64[0-9a-f]", &iter, sh, hh) != 3 || iter < 1000 || iter > 10000000) return 0;
    unsigned char salt[SALT_LEN], want[HASH_LEN], got[HASH_LEN];
    if (from_hex(sh, salt, SALT_LEN) || from_hex(hh, want, HASH_LEN) || pbkdf2(password, salt, iter, got)) return 0;
    int ok = CRYPTO_memcmp(want, got, HASH_LEN) == 0;
    OPENSSL_cleanse(got, sizeof got);
    return ok;
}

static void sha256_hex(const char *s, char out[65]) {
    unsigned char d[32];
    unsigned int n = 0;
    EVP_Digest(s, strlen(s), d, &n, EVP_sha256(), NULL);
    to_hex(d, 32, out);
}

static int clamp_tz(int tz) { return tz < -720 ? -720 : tz > 840 ? 840 : tz; }

/* ------------------------------------------------------------ проверки ---- */

int auth_check_login(const char *login, char *norm, size_t nlen, char *err, size_t errlen) {
    size_t n = login ? strlen(login) : 0;
    if (n < 3 || n > 48) { snprintf(err, errlen, "логин — от 3 до 48 символов"); return -1; }
    if (n + 1 > nlen) { snprintf(err, errlen, "логин слишком длинный"); return -1; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)login[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '@' || c == '+')) {
            snprintf(err, errlen, "в логине можно использовать латинские буквы, цифры и . _ - @ +");
            return -1;
        }
        norm[i] = (char)tolower(c);
    }
    norm[n] = '\0';
    return 0;
}

int auth_check_password(const char *password, const char *login, char *err, size_t errlen) {
    size_t n = password ? strlen(password) : 0;
    if (n < 8) { snprintf(err, errlen, "пароль — не короче 8 символов"); return -1; }
    if (n > 256) { snprintf(err, errlen, "пароль — не длиннее 256 символов"); return -1; }
    if (login && !strcasecmp(password, login)) { snprintf(err, errlen, "пароль не должен совпадать с логином"); return -1; }
    return 0;
}

/* ------------------------------------------------------------ перенос старых данных ---- */

/* Данные, сохранённые до появления аккаунтов (ключи, настройки), достаются первому пользователю.
 * Вызывается под блокировкой БД, внутри транзакции регистрации. */
static void adopt_legacy(sqlite3 *db, int64_t uid) {
    sqlite3_stmt *st;
    int has_legacy = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='api_keys'", -1, &st, NULL) == SQLITE_OK) {
        has_legacy = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    if (has_legacy) {
        if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO user_keys(user_id, provider, api_key, updated_ts) "
                                   "SELECT ?, provider, api_key, updated_ts FROM api_keys", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, uid);
            sqlite3_step(st);
            sqlite3_finalize(st);
            int moved = sqlite3_changes(db);
            if (moved) LOGI("auth", "ключи, сохранённые до появления аккаунтов (%d), переданы первому пользователю", moved);
        }
        sqlite3_exec(db, "DROP TABLE api_keys", NULL, NULL, NULL);
    }
    /* настройки роутера и флаг шифрования */
    char *legacy = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM kv WHERE name='router_settings'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) legacy = xstrdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    if (legacy) {
        char name[48];
        snprintf(name, sizeof name, "router_settings:%lld", (long long)uid);
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv(name, value) VALUES(?, ?)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, legacy, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        json *j = json_parse(legacy, NULL, 0);
        if (json_bool(json_get(j, "encryptKeys"), 0)) {
            snprintf(name, sizeof name, "encrypt:%lld", (long long)uid);
            if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv(name, value) VALUES(?, '1')", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
        json_free(j);
        free(legacy);
        sqlite3_exec(db, "DELETE FROM kv WHERE name='router_settings'", NULL, NULL, NULL);
    }
}

/* ------------------------------------------------------------ регистрация и вход ---- */

int auth_register(quota_db *q, const char *login, const char *password, int tz_min, auth_user *out, char *err, size_t errlen) {
    char norm[64];
    if (auth_check_login(login, norm, sizeof norm, err, errlen) || auth_check_password(password, norm, err, errlen)) return -1;
    char stored[200];
    if (hash_password(password, stored, sizeof stored) != 0) { snprintf(err, errlen, "не удалось захешировать пароль"); return -1; }

    sqlite3 *db = quota_sqlite(q);
    quota_lock(q);
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    int first = 0, rc = -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) first = sqlite3_column_int(st, 0) == 0;
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "INSERT INTO users(login, pass, created_ts, tz_min, last_seen) VALUES(?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        int64_t now = plat_now();
        sqlite3_bind_text(st, 1, norm, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, stored, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, now);
        sqlite3_bind_int(st, 4, clamp_tz(tz_min));
        sqlite3_bind_int64(st, 5, now);
        int s = sqlite3_step(st);
        sqlite3_finalize(st);
        if (s == SQLITE_DONE) {
            out->id = sqlite3_last_insert_rowid(db);
            xstrlcpy(out->login, norm, sizeof out->login);
            out->tz_min = clamp_tz(tz_min);
            if (first) adopt_legacy(db, out->id);
            rc = 0;
        } else if (s == SQLITE_CONSTRAINT) {
            snprintf(err, errlen, "логин «%s» уже занят", norm);
        } else {
            snprintf(err, errlen, "SQLite: %s", sqlite3_errmsg(db));
        }
    }
    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (rc == 0 && first) quota_set_owner_uid(q, out->id);
    quota_unlock(q);
    OPENSSL_cleanse(stored, sizeof stored);
    if (rc == 0) LOGI("auth", "зарегистрирован пользователь «%s» (id %lld)%s", norm, (long long)out->id, first ? " — владелец сервера" : "");
    return rc;
}

int auth_login(quota_db *q, const char *login, const char *password, int tz_min, auth_user *out, long *retry_after, char *err,
               size_t errlen) {
    *retry_after = 0;
    char norm[64];
    if (auth_check_login(login, norm, sizeof norm, err, errlen) != 0 || !password || strlen(password) > 256) {
        snprintf(err, errlen, "неверный логин или пароль");
        return -1;
    }
    sqlite3 *db = quota_sqlite(q);
    int64_t now = plat_now();
    sqlite3_stmt *st;

    /* блокировка после серии ошибок */
    quota_lock(q);
    int fails = 0;
    int64_t until = 0;
    if (sqlite3_prepare_v2(db, "SELECT fails, until_ts FROM login_fail WHERE login=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, norm, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) { fails = sqlite3_column_int(st, 0); until = sqlite3_column_int64(st, 1); }
        sqlite3_finalize(st);
    }
    char stored[200] = "";
    int64_t uid = 0;
    int tz_old = 0;
    if (until <= now && sqlite3_prepare_v2(db, "SELECT id, pass, tz_min FROM users WHERE login=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, norm, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            uid = sqlite3_column_int64(st, 0);
            xstrlcpy(stored, (const char *)sqlite3_column_text(st, 1), sizeof stored);
            tz_old = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    if (until > now) {
        *retry_after = (long)(until - now);
        snprintf(err, errlen, "слишком много неудачных попыток — подождите %ld с", *retry_after);
        return -2;
    }

    /* PBKDF2 считаем и для несуществующего логина — чтобы по времени ответа нельзя было узнать, есть ли он */
    if (!uid) /* тот же объём вычислений, что и для настоящего пользователя */
        snprintf(stored, sizeof stored, "pbkdf2_sha256$%d$%032d$%064d", g_iterations, 0, 0);
    int ok = verify_password(password, stored) && uid;
    OPENSSL_cleanse(stored, sizeof stored);

    quota_lock(q);
    if (!ok) {
        fails++;
        long lock = fails >= MAX_FAILS ? 60L << (fails - MAX_FAILS < 4 ? fails - MAX_FAILS : 4) : 0; /* 1, 2, 4, 8, 15 мин */
        if (lock > 900) lock = 900;
        if (sqlite3_prepare_v2(db, "INSERT INTO login_fail(login, fails, until_ts) VALUES(?,?,?) "
                                   "ON CONFLICT(login) DO UPDATE SET fails=excluded.fails, until_ts=excluded.until_ts",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, norm, -1, SQLITE_STATIC);
            sqlite3_bind_int(st, 2, fails);
            sqlite3_bind_int64(st, 3, lock ? now + lock : 0);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        quota_unlock(q);
        if (lock) LOGW("auth", "логин «%s»: %d неудачных попыток подряд — вход заблокирован на %ld с", norm, fails, lock);
        snprintf(err, errlen, "неверный логин или пароль");
        return -1;
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM login_fail WHERE login=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, norm, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    out->id = uid;
    xstrlcpy(out->login, norm, sizeof out->login);
    out->tz_min = tz_old;
    if (tz_min != tz_old) { auth_set_tz(q, uid, tz_min); out->tz_min = clamp_tz(tz_min); }
    return 0;
}

/* ------------------------------------------------------------ сессии ---- */

int auth_session_create(quota_db *q, int64_t uid, char token[AUTH_TOKEN_LEN + 1]) {
    unsigned char raw[AUTH_TOKEN_LEN / 2];
    if (RAND_bytes(raw, sizeof raw) != 1) return -1;
    to_hex(raw, sizeof raw, token);
    OPENSSL_cleanse(raw, sizeof raw);
    char h[65];
    sha256_hex(token, h);
    int64_t now = plat_now();
    sqlite3 *db = quota_sqlite(q);
    int rc = -1;
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO sessions(token_hash, user_id, created_ts, expires_ts, last_seen) VALUES(?,?,?,?,?)", -1,
                           &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, h, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, uid);
        sqlite3_bind_int64(st, 3, now);
        sqlite3_bind_int64(st, 4, now + AUTH_SESSION_TTL);
        sqlite3_bind_int64(st, 5, now);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(st);
    }
    /* заодно чистим истёкшие сессии */
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE expires_ts < ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    return rc;
}

int auth_session_user(quota_db *q, const char *token, auth_user *out) {
    if (!token || strlen(token) != AUTH_TOKEN_LEN) return 0;
    char h[65];
    sha256_hex(token, h);
    int64_t now = plat_now();
    sqlite3 *db = quota_sqlite(q);
    int found = 0;
    int64_t last_seen = 0;
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT u.id, u.login, u.tz_min, s.last_seen FROM sessions s JOIN users u ON u.id = s.user_id "
                               "WHERE s.token_hash=? AND s.expires_ts > ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, h, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, now);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->id = sqlite3_column_int64(st, 0);
            xstrlcpy(out->login, (const char *)sqlite3_column_text(st, 1), sizeof out->login);
            out->tz_min = sqlite3_column_int(st, 2);
            last_seen = sqlite3_column_int64(st, 3);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    /* скользящий срок: продлеваем не чаще раза в час, чтобы не писать в БД на каждый запрос */
    if (found && now - last_seen > 3600 &&
        sqlite3_prepare_v2(db, "UPDATE sessions SET last_seen=?, expires_ts=? WHERE token_hash=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, now + AUTH_SESSION_TTL);
        sqlite3_bind_text(st, 3, h, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    return found;
}

void auth_session_delete(quota_db *q, const char *token) {
    if (!token || !*token) return;
    char h[65];
    sha256_hex(token, h);
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(quota_sqlite(q), "DELETE FROM sessions WHERE token_hash=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, h, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
}

void auth_set_tz(quota_db *q, int64_t uid, int tz_min) {
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(quota_sqlite(q), "UPDATE users SET tz_min=? WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, clamp_tz(tz_min));
        sqlite3_bind_int64(st, 2, uid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
}

int auth_cookie_token(const char *cookie, char token[AUTH_TOKEN_LEN + 1]) {
    token[0] = '\0';
    if (!cookie) return 0;
    const char *p = cookie;
    size_t nl = strlen(AUTH_COOKIE);
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, AUTH_COOKIE, nl) && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t n = strcspn(v, "; ");
            if (n != AUTH_TOKEN_LEN) return 0;
            for (size_t i = 0; i < n; i++)
                if (!isxdigit((unsigned char)v[i])) return 0;
            memcpy(token, v, n);
            token[n] = '\0';
            return 1;
        }
        p += strcspn(p, ";");
    }
    return 0;
}
