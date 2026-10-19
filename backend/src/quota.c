#include "quota.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <sqlite3.h>

#include "crypto.h"
#include "log.h"
#include "platform.h"
#include "strbuf.h"

struct quota_db {
    sqlite3        *db;
    pthread_mutex_t mu;    /* одно соединение на процесс, доступ сериализуем */
    int64_t         owner; /* первый зарегистрированный пользователь */
};

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS users("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, login TEXT NOT NULL UNIQUE, pass TEXT NOT NULL,"
    "  created_ts INTEGER NOT NULL, tz_min INTEGER NOT NULL DEFAULT 0, last_seen INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS sessions("
    "  token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL, created_ts INTEGER NOT NULL,"
    "  expires_ts INTEGER NOT NULL, last_seen INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);"
    "CREATE TABLE IF NOT EXISTS login_fail(login TEXT PRIMARY KEY, fails INTEGER NOT NULL, until_ts INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS user_keys("
    "  user_id INTEGER NOT NULL, provider TEXT NOT NULL, api_key TEXT NOT NULL, updated_ts INTEGER NOT NULL,"
    "  PRIMARY KEY(user_id, provider));"
    "CREATE TABLE IF NOT EXISTS usage_u("
    "  user_id INTEGER NOT NULL, model_id TEXT NOT NULL, period TEXT NOT NULL, bucket TEXT NOT NULL,"
    "  requests INTEGER NOT NULL DEFAULT 0, tokens INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY(user_id, model_id, period, bucket));"
    "CREATE TABLE IF NOT EXISTS cooldown_u("
    "  user_id INTEGER NOT NULL, model_id TEXT NOT NULL, until_ts INTEGER NOT NULL, reason TEXT,"
    "  PRIMARY KEY(user_id, model_id));"
    "CREATE TABLE IF NOT EXISTS attempts_u("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, ts INTEGER NOT NULL, request_id TEXT,"
    "  model_id TEXT, kind TEXT, http_status INTEGER, message TEXT);"
    "CREATE INDEX IF NOT EXISTS attempts_u_ts ON attempts_u(ts);"
    "CREATE TABLE IF NOT EXISTS stats_daily("
    "  user_id INTEGER NOT NULL, day TEXT NOT NULL, visited INTEGER NOT NULL DEFAULT 0,"
    "  requests INTEGER NOT NULL DEFAULT 0, tokens_in INTEGER NOT NULL DEFAULT 0, tokens_out INTEGER NOT NULL DEFAULT 0,"
    "  media INTEGER NOT NULL DEFAULT 0, switches INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(user_id, day));"
    "CREATE TABLE IF NOT EXISTS kv(name TEXT PRIMARY KEY, value TEXT NOT NULL);"
    /* счётчики, паузы и журнал прошлой версии были общими на всех — они временные, начинаем заново;
     * старая таблица api_keys остаётся до регистрации первого пользователя (ключи перейдут к нему) */
    "DROP TABLE IF EXISTS usage;"
    "DROP TABLE IF EXISTS cooldown;"
    "DROP TABLE IF EXISTS attempts;";

/* Ключи окон в UTC: минута, сутки, месяц. */
static void buckets(int64_t now, char *minute, char *day, char *month) {
    struct tm tm;
    plat_gmtime((time_t)now, &tm);
    snprintf(minute, 32, "%04d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    snprintf(day, 32, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(month, 32, "%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
}

int64_t quota_next_day(int64_t now) { return (now / 86400 + 1) * 86400; }

struct sqlite3 *quota_sqlite(quota_db *q) { return q->db; }
void quota_lock(quota_db *q) { pthread_mutex_lock(&q->mu); }
void quota_unlock(quota_db *q) { pthread_mutex_unlock(&q->mu); }
int64_t quota_owner_uid(quota_db *q) { return q->owner; }
void quota_set_owner_uid(quota_db *q, int64_t uid) { q->owner = uid; }

quota_db *quota_open(const char *path, char *err, size_t errlen) {
    /* создаём каталог для файла БД */
    char dir[256];
    xstrlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash && slash != dir) { *slash = '\0'; plat_mkdirs(dir); }

    quota_db *q = calloc(1, sizeof *q);
    if (!q) { snprintf(err, errlen, "нет памяти"); return NULL; }
    if (sqlite3_open(path, &q->db) != SQLITE_OK) {
        snprintf(err, errlen, "SQLite: не удалось открыть %s: %s", path, sqlite3_errmsg(q->db));
        sqlite3_close(q->db);
        free(q);
        return NULL;
    }
    sqlite3_busy_timeout(q->db, 3000);
    char *e = NULL;
    if (sqlite3_exec(q->db, SCHEMA, NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "SQLite: %s", e ? e : "ошибка схемы");
        sqlite3_free(e);
        sqlite3_close(q->db);
        free(q);
        return NULL;
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT MIN(id) FROM users", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) q->owner = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    pthread_mutex_init(&q->mu, NULL);
    return q;
}

void quota_close(quota_db *q) {
    if (!q) return;
    sqlite3_close(q->db);
    pthread_mutex_destroy(&q->mu);
    free(q);
}

static void get_usage(quota_db *q, int64_t uid, const char *id, const char *period, const char *bucket, int *req, long *tok) {
    *req = 0;
    if (tok) *tok = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT requests, tokens FROM usage_u WHERE user_id=? AND model_id=? AND period=? AND bucket=?",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, period, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, bucket, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *req = sqlite3_column_int(st, 0);
        if (tok) *tok = (long)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
}

static int64_t get_cooldown(quota_db *q, int64_t uid, const char *id, char *reason, size_t rlen) {
    int64_t until = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT until_ts, reason FROM cooldown_u WHERE user_id=? AND model_id=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        until = sqlite3_column_int64(st, 0);
        if (reason) xstrlcpy(reason, (const char *)sqlite3_column_text(st, 1), rlen);
    }
    sqlite3_finalize(st);
    return until;
}

/* Порог с учётом процента: 20 запросов при 90% → 18. Не меньше 1. */
static long eff_limit(long limit, int pct) {
    if (pct >= 100 || pct <= 0) return limit;
    long v = limit * pct / 100;
    return v < 1 ? 1 : v;
}

quota_state quota_check(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, int pct, char *reason, size_t rlen) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    quota_state res = QUOTA_OK;
    pthread_mutex_lock(&q->mu);
    char cr[128] = "";
    int64_t until = get_cooldown(q, uid, m->id, cr, sizeof cr);
    if (until > now) {
        snprintf(reason, rlen, "пауза ещё %lld с: %s", (long long)(until - now), cr);
        res = QUOTA_COOLDOWN;
    } else {
        int r;
        long t;
        const limits_cfg *L = &m->limits;
        /* минутный лимит — всегда целиком (порог к нему не применяем: он сбрасывается через минуту) */
        if (L->rpm > 0) {
            get_usage(q, uid, m->id, "m", mi, &r, NULL);
            if (r >= L->rpm) { snprintf(reason, rlen, "локальный лимит %d запросов/мин", L->rpm); res = QUOTA_LOCAL_LIMIT; }
        }
        if (res == QUOTA_OK && (L->rpd > 0 || L->tpd > 0)) {
            get_usage(q, uid, m->id, "d", d, &r, &t);
            if (L->rpd > 0 && r >= eff_limit(L->rpd, pct)) {
                snprintf(reason, rlen, "израсходовано %d из %d запросов/сутки (порог %d%%)", r, L->rpd, pct);
                res = QUOTA_LOCAL_LIMIT;
            } else if (L->tpd > 0 && t >= eff_limit(L->tpd, pct)) {
                snprintf(reason, rlen, "израсходовано %ld из %ld токенов/сутки (порог %d%%)", t, L->tpd, pct);
                res = QUOTA_LOCAL_LIMIT;
            }
        }
        if (res == QUOTA_OK && L->rpmo > 0) {
            get_usage(q, uid, m->id, "mo", mo, &r, NULL);
            if (r >= eff_limit(L->rpmo, pct)) {
                snprintf(reason, rlen, "израсходовано %d из %d запросов/месяц (порог %d%%)", r, L->rpmo, pct);
                res = QUOTA_LOCAL_LIMIT;
            }
        }
    }
    pthread_mutex_unlock(&q->mu);
    return res;
}

int quota_load_pct(quota_db *q, int64_t uid, const model_cfg *m, int64_t now) {
    const limits_cfg *L = &m->limits;
    if (!L->rpm && !L->rpd && !L->rpmo && !L->tpd) return -1;
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    int r, best = 0;
    long t;
    pthread_mutex_lock(&q->mu);
    if (L->rpm) { get_usage(q, uid, m->id, "m", mi, &r, NULL); if (r * 100 / L->rpm > best) best = r * 100 / L->rpm; }
    get_usage(q, uid, m->id, "d", d, &r, &t);
    if (L->rpd && r * 100 / L->rpd > best) best = r * 100 / L->rpd;
    if (L->tpd && (int)(t * 100 / L->tpd) > best) best = (int)(t * 100 / L->tpd);
    if (L->rpmo) { get_usage(q, uid, m->id, "mo", mo, &r, NULL); if (r * 100 / L->rpmo > best) best = r * 100 / L->rpmo; }
    pthread_mutex_unlock(&q->mu);
    return best > 100 ? 100 : best;
}

static void bump(quota_db *q, int64_t uid, const char *id, const char *period, const char *bucket, long tokens) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db,
                           "INSERT INTO usage_u(user_id, model_id, period, bucket, requests, tokens) VALUES(?,?,?,?,1,?) "
                           "ON CONFLICT(user_id, model_id, period, bucket) DO UPDATE SET requests=requests+1, tokens=tokens+excluded.tokens",
                           -1, &st, NULL) != SQLITE_OK) {
        LOGE("quota", "SQLite: %s", sqlite3_errmsg(q->db));
        return;
    }
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, period, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, bucket, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, tokens);
    if (sqlite3_step(st) != SQLITE_DONE) LOGE("quota", "SQLite: %s", sqlite3_errmsg(q->db));
    sqlite3_finalize(st);
}

void quota_record(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, long tokens) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    pthread_mutex_lock(&q->mu);
    sqlite3_exec(q->db, "BEGIN", NULL, NULL, NULL);
    bump(q, uid, m->id, "m", mi, tokens);
    bump(q, uid, m->id, "d", d, tokens);
    bump(q, uid, m->id, "mo", mo, tokens);
    /* старые минутные окна больше не нужны */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "DELETE FROM usage_u WHERE user_id=? AND model_id=? AND period='m' AND bucket<>?", -1, &st,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, m->id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, mi, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_exec(q->db, "COMMIT", NULL, NULL, NULL);
    pthread_mutex_unlock(&q->mu);
}

void quota_set_cooldown(quota_db *q, int64_t uid, const char *model_id, int64_t until, const char *reason) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db,
                           "INSERT INTO cooldown_u(user_id, model_id, until_ts, reason) VALUES(?,?,?,?) "
                           "ON CONFLICT(user_id, model_id) DO UPDATE SET until_ts=MAX(until_ts, excluded.until_ts), reason=excluded.reason",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, until);
        sqlite3_bind_text(st, 4, reason ? reason : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

static void exec_uid_model(quota_db *q, const char *sql, int64_t uid, const char *model_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, model_id, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

void quota_reset(quota_db *q, int64_t uid, const char *model_id) {
    pthread_mutex_lock(&q->mu);
    exec_uid_model(q, "DELETE FROM cooldown_u WHERE user_id=? AND model_id=?", uid, model_id);
    exec_uid_model(q, "DELETE FROM usage_u WHERE user_id=? AND model_id=?", uid, model_id);
    pthread_mutex_unlock(&q->mu);
}

void quota_clear_cooldown(quota_db *q, int64_t uid, const char *model_id) {
    pthread_mutex_lock(&q->mu);
    exec_uid_model(q, "DELETE FROM cooldown_u WHERE user_id=? AND model_id=?", uid, model_id);
    pthread_mutex_unlock(&q->mu);
}

void quota_get_status(quota_db *q, int64_t uid, const model_cfg *m, int64_t now, quota_status *s) {
    char mi[32], d[32], mo[32];
    buckets(now, mi, d, mo);
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&q->mu);
    get_usage(q, uid, m->id, "m", mi, &s->req_minute, NULL);
    get_usage(q, uid, m->id, "d", d, &s->req_day, &s->tokens_day);
    get_usage(q, uid, m->id, "mo", mo, &s->req_month, NULL);
    int64_t until = get_cooldown(q, uid, m->id, s->cooldown_reason, sizeof s->cooldown_reason);
    s->cooldown_until = until > now ? until : 0;
    if (!s->cooldown_until) s->cooldown_reason[0] = '\0';
    pthread_mutex_unlock(&q->mu);
}

void quota_log_attempt(quota_db *q, int64_t uid, int64_t now, const char *request_id, const char *model_id, const char *kind,
                       long http_status, const char *message) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db,
                           "INSERT INTO attempts_u(user_id, ts, request_id, model_id, kind, http_status, message) VALUES(?,?,?,?,?,?,?)",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_int64(st, 2, now);
        sqlite3_bind_text(st, 3, request_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, kind, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 6, http_status);
        sqlite3_bind_text(st, 7, message ? message : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    /* журнал не растёт бесконечно: храним 30 дней */
    if (sqlite3_prepare_v2(q->db, "DELETE FROM attempts_u WHERE ts < ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now - 30 * 86400);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}

/* ------------------------------------------------------------ ключи пользователя ---- */

/* Запись без блокировки (вызывающий держит q->mu). value — уже в нужном виде (открытый или enc:v1:). */
static int key_write_locked(quota_db *q, int64_t uid, const char *provider, const char *value) {
    sqlite3_stmt *st;
    int rc = -1;
    const char *sql = (value && *value)
        ? "INSERT INTO user_keys(user_id, provider, api_key, updated_ts) VALUES(?,?,?,?) "
          "ON CONFLICT(user_id, provider) DO UPDATE SET api_key=excluded.api_key, updated_ts=excluded.updated_ts"
        : "DELETE FROM user_keys WHERE user_id=? AND provider=?";
    if (sqlite3_prepare_v2(q->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
        if (value && *value) {
            sqlite3_bind_text(st, 3, value, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, plat_now());
        }
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(st);
    }
    if (rc) LOGE("keys", "SQLite: %s", sqlite3_errmsg(q->db));
    return rc;
}

static void encrypt_flag_name(int64_t uid, char *out, size_t n) { snprintf(out, n, "encrypt:%lld", (long long)uid); }

int quota_encryption_on(quota_db *q, int64_t uid) {
    char name[48];
    encrypt_flag_name(uid, name, sizeof name);
    char *v = quota_kv_get(q, name);
    int on = v && !strcmp(v, "1");
    free(v);
    return on;
}

int store_key_set(quota_db *q, int64_t uid, const char *provider, const char *key) {
    char enc[4096];
    const char *value = key;
    if (key && *key && quota_encryption_on(q, uid)) {
        if (crypto_encrypt(key, enc, sizeof enc) != 0) { LOGE("keys", "не удалось зашифровать ключ «%s»", provider); return -1; }
        value = enc;
    }
    pthread_mutex_lock(&q->mu);
    int rc = key_write_locked(q, uid, provider, value);
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int store_key_get_raw(quota_db *q, int64_t uid, const char *provider, char *buf, size_t len) {
    int found = 0;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT api_key FROM user_keys WHERE user_id=? AND provider=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *k = (const char *)sqlite3_column_text(st, 0);
            if (k && *k) { xstrlcpy(buf, k, len); found = 1; }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return found;
}

int store_key_get(quota_db *q, int64_t uid, const char *provider, char *buf, size_t len) {
    char raw[4096];
    if (!store_key_get_raw(q, uid, provider, raw, sizeof raw)) return 0;
    if (crypto_is_encrypted(raw)) {
        if (crypto_decrypt(raw, buf, len) != 0) {
            LOGE("keys", "ключ «%s» не расшифровывается (сменился мастер-ключ?) — введите его заново в «Настройках»", provider);
            buf[0] = '\0';
            return 0;
        }
        return 1;
    }
    xstrlcpy(buf, raw, len);
    return 1;
}

int quota_set_encryption(quota_db *q, int64_t uid, int on, int migrate) {
    if (on && !crypto_available()) return -1;
    char name[48];
    encrypt_flag_name(uid, name, sizeof name);
    quota_kv_set(q, name, on ? "1" : "0");
    if (!migrate) return 0;
    char (*names)[64] = malloc(256 * sizeof *names);
    if (!names) return -1;
    int n = store_key_list(q, uid, names, 256), done = 0;
    for (int i = 0; i < n; i++) {
        char plain[2100];
        if (!store_key_get(q, uid, names[i], plain, sizeof plain)) continue;
        if (store_key_set(q, uid, names[i], plain) == 0) done++; /* перезапись в новом виде */
        OPENSSL_cleanse(plain, sizeof plain);
    }
    free(names);
    return done;
}

int store_key_list(quota_db *q, int64_t uid, char names[][64], int max) {
    int n = 0;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT provider FROM user_keys WHERE user_id=? ORDER BY provider", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        while (n < max && sqlite3_step(st) == SQLITE_ROW)
            xstrlcpy(names[n++], (const char *)sqlite3_column_text(st, 0), 64);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return n;
}

/* ------------------------------------------------------------ kv ---- */

char *quota_kv_get(quota_db *q, const char *name) {
    char *out = NULL;
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT value FROM kv WHERE name=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) out = xstrdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
    return out;
}

void quota_kv_set(quota_db *q, const char *name, const char *value) {
    pthread_mutex_lock(&q->mu);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "INSERT INTO kv(name, value) VALUES(?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&q->mu);
}
