/* Статистика пользователя по дням: посещения сайта, запросы, токены, сгенерированные файлы.
 * День считается в часовом поясе пользователя (users.tz_min), а не в UTC — чтобы «сегодня» совпадало с его календарём. */
#ifndef IAI_STATS_H
#define IAI_STATS_H

#include <stdint.h>

#include "quota.h"
#include "strbuf.h"

/* "YYYY-MM-DD" для момента now при смещении tz_min минут от UTC. */
void stats_day(int64_t now, int tz_min, char out[16]);
/* Смещение часового пояса пользователя (0 — неизвестно / uid 0). */
int  stats_user_tz(quota_db *q, int64_t uid);
/* Отметить, что пользователь сегодня заходил на сайт. */
void stats_visit(quota_db *q, int64_t uid);
/* Добавить к сегодняшним счётчикам. */
void stats_record(quota_db *q, int64_t uid, int requests, long tokens_in, long tokens_out, int media, int switches);
/* Токенов (вход + выход) за сегодня. */
long long stats_tokens_today(quota_db *q, int64_t uid);
/* "today":"…","tz":…,"days":[{"day":"2026-10-19","visited":true,…}] — дни за последние ndays, где была активность. */
void stats_days_json(quota_db *q, int64_t uid, int ndays, strbuf *b);

#endif
