#include "stats.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "log.h"
#include "platform.h"

void stats_day(int64_t now, int tz_min, char out[16]) {
    struct tm tm;
    plat_gmtime((time_t)(now + (int64_t)tz_min * 60), &tm);
    snprintf(out, 16, "%04u-%02u-%02u", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u);
}

int stats_user_tz(quota_db *q, int64_t uid) {
    if (uid <= 0) return 0;
    int tz = 0;
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(quota_sqlite(q), "SELECT tz_min FROM users WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        if (sqlite3_step(st) == SQLITE_ROW) tz = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    return tz;
}

static void upsert(quota_db *q, int64_t uid, const char *day, int visited, int requests, long tin, long tout, int media, int switches) {
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(quota_sqlite(q),
                           "INSERT INTO stats_daily(user_id, day, visited, requests, tokens_in, tokens_out, media, switches) "
                           "VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(user_id, day) DO UPDATE SET "
                           "visited=MAX(visited, excluded.visited), requests=requests+excluded.requests, "
                           "tokens_in=tokens_in+excluded.tokens_in, tokens_out=tokens_out+excluded.tokens_out, "
                           "media=media+excluded.media, switches=switches+excluded.switches",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, day, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, visited);
        sqlite3_bind_int(st, 4, requests);
        sqlite3_bind_int64(st, 5, tin);
        sqlite3_bind_int64(st, 6, tout);
        sqlite3_bind_int(st, 7, media);
        sqlite3_bind_int(st, 8, switches);
        if (sqlite3_step(st) != SQLITE_DONE) LOGE("stats", "SQLite: %s", sqlite3_errmsg(quota_sqlite(q)));
        sqlite3_finalize(st);
    }
    quota_unlock(q);
}

void stats_visit(quota_db *q, int64_t uid) {
    char day[16];
    stats_day(plat_now(), stats_user_tz(q, uid), day);
    upsert(q, uid, day, 1, 0, 0, 0, 0, 0);
}

void stats_record(quota_db *q, int64_t uid, int requests, long tin, long tout, int media, int switches) {
    char day[16];
    stats_day(plat_now(), stats_user_tz(q, uid), day);
    /* пользоваться чатом можно только с открытым сайтом — это тоже посещение */
    upsert(q, uid, day, 1, requests, tin < 0 ? 0 : tin, tout < 0 ? 0 : tout, media, switches);
}

long long stats_tokens_today(quota_db *q, int64_t uid) {
    char day[16];
    stats_day(plat_now(), stats_user_tz(q, uid), day);
    long long t = 0;
    quota_lock(q);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(quota_sqlite(q), "SELECT tokens_in + tokens_out FROM stats_daily WHERE user_id=? AND day=?", -1, &st,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, day, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) t = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    return t;
}

void stats_days_json(quota_db *q, int64_t uid, int ndays, strbuf *b) {
    int tz = stats_user_tz(q, uid);
    int64_t now = plat_now();
    char today[16], from[16];
    stats_day(now, tz, today);
    stats_day(now - (int64_t)(ndays - 1) * 86400, tz, from);
    sb_append(b, "\"today\":");
    sb_json_str(b, today);
    sb_printf(b, ",\"tz\":%d,\"days\":[", tz);
    quota_lock(q);
    sqlite3_stmt *st;
    int first = 1;
    if (sqlite3_prepare_v2(quota_sqlite(q),
                           "SELECT day, visited, requests, tokens_in, tokens_out, media, switches FROM stats_daily "
                           "WHERE user_id=? AND day>=? AND day<=? ORDER BY day",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, uid);
        sqlite3_bind_text(st, 2, from, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, today, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) sb_appendc(b, ',');
            first = 0;
            sb_append(b, "{\"day\":");
            sb_json_str(b, (const char *)sqlite3_column_text(st, 0));
            sb_printf(b, ",\"visited\":%s,\"requests\":%d,\"tokens_in\":%lld,\"tokens_out\":%lld,\"media\":%d,\"switches\":%d}",
                      sqlite3_column_int(st, 1) ? "true" : "false", sqlite3_column_int(st, 2),
                      (long long)sqlite3_column_int64(st, 3), (long long)sqlite3_column_int64(st, 4), sqlite3_column_int(st, 5),
                      sqlite3_column_int(st, 6));
        }
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    sb_append(b, "]");
}
