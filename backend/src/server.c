#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "log.h"
#include "crypto.h"
#include "platform.h"
#include "settings.h"
#include "stats.h"
#include "auth.h"

#include <openssl/crypto.h>
#include "strbuf.h"

#define MAX_HEADER   (16 * 1024)
#define MAX_BODY     (4 * 1024 * 1024)
#define MAX_MESSAGES 200
#define MAX_MSG_LEN  (200 * 1024)
#define MAX_CLIENTS  64
#define IO_TIMEOUT_MS 15000

static volatile sock_t g_listen = SOCK_INVALID;
static volatile int    g_running = 0;
static volatile int    g_clients = 0;

/* ------------------------------------------------------------------ ответы ---- */

static char *json_error(const char *msg) {
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":false,\"error\":");
    sb_json_str(&b, msg);
    sb_append(&b, "}");
    return sb_take(&b);
}

/* Контекст одного запроса: пользователь и роутер с его uid. */
typedef struct {
    server_ctx *ctx;
    router      rt;          /* копия общего роутера с uid текущего пользователя */
    auth_user   user;        /* id == 0 — не вошёл */
    const http_request *rq;
    const char *query;       /* после '?' или "" */
    http_reply *out;
} req_t;

static int reply(req_t *r, int code, char *body) {
    r->out->body = body;
    return code;
}
static int fail(req_t *r, int code, const char *msg) { return reply(r, code, json_error(msg)); }

/* Значение параметра запроса ?name=value (только цифры и знак). */
static int query_int(const char *query, const char *name, int def) {
    size_t n = strlen(name);
    for (const char *p = query; p && *p;) {
        if (!strncmp(p, name, n) && p[n] == '=') return atoi(p + n + 1);
        p = strchr(p, '&');
        if (p) p++;
    }
    return def;
}

static int handle_health(req_t *r) {
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"service\":\"infinityAI-router\",\"version\":\"0.2.0\",\"models\":%zu}", r->ctx->cfg->nmodels);
    return reply(r, 200, sb_take(&b));
}

/* ------------------------------------------------------------ вход и регистрация ---- */

static void session_cookie(req_t *r, const char *token) {
    const app_config *cfg = r->ctx->cfg;
    if (token)
        snprintf(r->out->set_cookie, sizeof r->out->set_cookie, "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%ld%s", AUTH_COOKIE,
                 token, AUTH_SESSION_TTL, cfg->cookie_secure ? "; Secure" : "");
    else
        snprintf(r->out->set_cookie, sizeof r->out->set_cookie, "%s=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s", AUTH_COOKIE,
                 cfg->cookie_secure ? "; Secure" : "");
}

static char *user_json(const auth_user *u, int registration) {
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"registration\":%s,\"user\":{\"id\":%lld,\"login\":", registration ? "true" : "false", (long long)u->id);
    sb_json_str(&b, u->login);
    sb_printf(&b, ",\"tz\":%d}}", u->tz_min);
    return sb_take(&b);
}

static int handle_auth(req_t *r, const char *path) {
    server_ctx *ctx = r->ctx;
    const char *method = r->rq->method;
    if (!strcmp(path, "/api/auth/me")) {
        if (!r->user.id) {
            strbuf b;
            sb_init(&b);
            sb_printf(&b, "{\"ok\":false,\"auth\":false,\"registration\":%s,\"error\":\"нужно войти\"}",
                      ctx->cfg->allow_registration ? "true" : "false");
            return reply(r, 401, sb_take(&b));
        }
        int tz = query_int(r->query, "tz", r->user.tz_min);
        if (tz != r->user.tz_min) { auth_set_tz(ctx->quota, r->user.id, tz); r->user.tz_min = tz < -720 ? -720 : tz > 840 ? 840 : tz; }
        stats_visit(ctx->quota, r->user.id); /* открыл сайт — день посещения */
        return reply(r, 200, user_json(&r->user, ctx->cfg->allow_registration));
    }
    if (strcmp(method, "POST")) return fail(r, 405, "используйте POST");
    if (!strcmp(path, "/api/auth/logout")) {
        char token[AUTH_TOKEN_LEN + 1];
        if (auth_cookie_token(r->rq->cookie, token)) auth_session_delete(ctx->quota, token);
        session_cookie(r, NULL);
        return reply(r, 200, xstrdup("{\"ok\":true}"));
    }
    int reg = !strcmp(path, "/api/auth/register");
    if (!reg && strcmp(path, "/api/auth/login")) return fail(r, 404, "не найдено");
    if (reg && !ctx->cfg->allow_registration) return fail(r, 403, "регистрация на этом сервере закрыта");

    json *j = json_parse(r->rq->body ? r->rq->body : "", NULL, 0);
    const char *login = json_str(json_get(j, "login"), NULL);
    const char *password = json_str(json_get(j, "password"), NULL);
    int tz = (int)json_num(json_get(j, "tz"), 0);
    if (!login || !password) { json_free(j); return fail(r, 400, "нужно {\"login\": \"...\", \"password\": \"...\"}"); }
    auth_user u;
    memset(&u, 0, sizeof u);
    char err[200];
    long retry = 0;
    int rc = reg ? auth_register(ctx->quota, login, password, tz, &u, err, sizeof err)
                 : auth_login(ctx->quota, login, password, tz, &u, &retry, err, sizeof err);
    json_free(j);
    if (rc != 0) {
        if (rc == -2) {
            strbuf b;
            sb_init(&b);
            sb_append(&b, "{\"ok\":false,\"error\":");
            sb_json_str(&b, err);
            sb_printf(&b, ",\"retry_after\":%ld}", retry);
            return reply(r, 429, sb_take(&b));
        }
        return fail(r, reg ? 400 : 401, err);
    }
    char token[AUTH_TOKEN_LEN + 1];
    if (auth_session_create(ctx->quota, u.id, token) != 0) return fail(r, 500, "не удалось создать сессию");
    session_cookie(r, token);
    OPENSSL_cleanse(token, sizeof token);
    stats_visit(ctx->quota, u.id);
    if (!reg) LOGI("auth", "вход: «%s»", u.login);
    return reply(r, 200, user_json(&u, ctx->cfg->allow_registration));
}

/* ------------------------------------------------------------ модели и чат ---- */

static int handle_models(req_t *r) {
    server_ctx *ctx = r->ctx;
    int64_t now = plat_now();
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,\"models\":[");
    router_settings *rs = malloc(sizeof *rs);
    if (!rs) { sb_free(&b); return fail(r, 500, "нет памяти"); }
    settings_get(ctx->quota, r->user.id, rs);
    int first = 1;
    for (size_t i = 0; i < ctx->cfg->nmodels; i++) {
        const model_cfg *m = &ctx->cfg->models[i];
        char kb[600];
        const char *src = NULL;
        int has_key = router_api_key(&r->rt, m, kb, sizeof kb, &src) != NULL;
        if (m->auto_added && !has_key) continue; /* модели каталога показываем, только когда для них есть ключ */
        quota_status st;
        quota_get_status(ctx->quota, r->user.id, m, now, &st);
        char reason[200];
        int disabled = settings_is_disabled(rs, m->id);
        int available = m->enabled && has_key && quota_check(ctx->quota, r->user.id, m, now, 100, reason, sizeof reason) == QUOTA_OK;
        if (!first) sb_appendc(&b, ',');
        first = 0;
        sb_append(&b, "{\"id\":");
        sb_json_str(&b, m->id);
        sb_append(&b, ",\"label\":");
        sb_json_str(&b, m->label);
        sb_append(&b, ",\"provider\":");
        sb_json_str(&b, m->provider);
        sb_append(&b, ",\"model\":");
        sb_json_str(&b, m->model);
        sb_append(&b, ",\"tier\":");
        sb_json_str(&b, m->tier);
        sb_append(&b, ",\"key_name\":");
        sb_json_str(&b, m->key_name);
        sb_append(&b, ",\"key_source\":");
        sb_json_str(&b, src);
        sb_append(&b, ",\"kind\":");
        sb_json_str(&b, kind_code(model_kind(m)));
        sb_printf(&b, ",\"order\":%zu,\"auto\":%s,\"priority\":%d,\"enabled\":%s,\"configured\":%s,\"available\":%s,\"skills\":[", i,
                  m->auto_added ? "true" : "false", m->priority, m->enabled && !disabled ? "true" : "false",
                  has_key ? "true" : "false", available && !disabled ? "true" : "false");
        for (int s = 0; s < m->nskills; s++) {
            if (s) sb_appendc(&b, ',');
            sb_json_str(&b, m->skills[s]);
        }
        sb_printf(&b, "],\"limits\":{\"rpm\":%d,\"rpd\":%d,\"rpmo\":%d,\"tpd\":%ld}", m->limits.rpm, m->limits.rpd,
                  m->limits.rpmo, m->limits.tpd);
        sb_printf(&b, ",\"usage\":{\"minute\":%d,\"day\":%d,\"month\":%d,\"tokens_day\":%ld}", st.req_minute, st.req_day,
                  st.req_month, st.tokens_day);
        sb_printf(&b, ",\"cooldown_until\":%lld,\"cooldown_reason\":", (long long)st.cooldown_until);
        sb_json_str(&b, st.cooldown_reason[0] ? st.cooldown_reason : NULL);
        sb_append(&b, "}");
    }
    free(rs);
    sb_printf(&b, "],\"day_resets_at\":%lld}", (long long)quota_next_day(now));
    return reply(r, 200, sb_take(&b));
}

static int handle_chat(req_t *r) {
    char err[160];
    json *req = json_parse(r->rq->body ? r->rq->body : "", err, sizeof err);
    if (!req || req->type != JSON_OBJECT) {
        json_free(req);
        char msg[300];
        snprintf(msg, sizeof msg, "тело запроса должно быть JSON-объектом: %s", err);
        return fail(r, 400, msg);
    }
    const json *arr = json_get(req, "messages");
    size_t n = json_len(arr);
    if (!arr || arr->type != JSON_ARRAY || n == 0 || n > MAX_MESSAGES) {
        json_free(req);
        return fail(r, 400, "поле messages: нужен массив из 1–200 сообщений");
    }
    chat_msg *msgs = calloc(n, sizeof *msgs);
    if (!msgs) { json_free(req); return fail(r, 500, "нет памяти"); }
    for (size_t i = 0; i < n; i++) {
        const json *m = json_at(arr, i);
        const char *role = json_str(json_get(m, "role"), NULL);
        const char *content = json_str(json_get(m, "content"), NULL);
        if (!role || !content || (strcmp(role, "user") && strcmp(role, "assistant"))) {
            free(msgs);
            json_free(req);
            return fail(r, 400, "каждое сообщение: {\"role\": \"user\"|\"assistant\", \"content\": строка}");
        }
        if (strlen(content) > MAX_MSG_LEN) {
            free(msgs);
            json_free(req);
            return fail(r, 413, "сообщение слишком длинное (максимум 200 КБ)");
        }
        msgs[i].role = role;
        msgs[i].content = content;
    }
    if (strcmp(msgs[n - 1].role, "user")) {
        free(msgs);
        json_free(req);
        return fail(r, 400, "последнее сообщение должно быть от пользователя");
    }
    const char *prefer = json_str(json_get(req, "model"), "auto");
    const char *skill = json_str(json_get(req, "skill"), NULL);

    router_reply *rep = malloc(sizeof *rep);
    if (!rep) { free(msgs); json_free(req); return fail(r, 500, "нет памяти"); }
    router_chat(&r->rt, msgs, n, prefer, skill, rep);
    char *body = router_reply_json(rep);
    int code = rep->ok ? 200 : 503;
    router_reply_free(rep);
    free(rep);
    free(msgs);
    json_free(req);
    if (!body) return fail(r, 500, "нет памяти");
    return reply(r, code, body);
}

static int handle_reset(req_t *r, const char *path) {
    /* /api/models/{id}/reset */
    const char *id = path + strlen("/api/models/");
    const char *end = strstr(id, "/reset");
    char buf[64];
    size_t len = end ? (size_t)(end - id) : 0;
    if (!end || !len || len >= sizeof buf) return fail(r, 404, "не найдено");
    memcpy(buf, id, len);
    buf[len] = '\0';
    for (size_t i = 0; i < r->ctx->cfg->nmodels; i++)
        if (!strcmp(r->ctx->cfg->models[i].id, buf)) {
            quota_reset(r->ctx->quota, r->user.id, buf);
            LOGI("server", "пользователь %lld: счётчики и пауза модели %s сброшены", (long long)r->user.id, buf);
            return reply(r, 200, xstrdup("{\"ok\":true}"));
        }
    return fail(r, 404, "модель не найдена в каскаде");
}

/* ---------------------------------------------------------------- ключи ---- */

/* Ключ попадает в HTTP-заголовок — допускаем только видимые ASCII-символы (защита от подмены заголовков). */
static int key_is_valid(const char *k) {
    size_t n = strlen(k);
    if (n == 0 || n > 512) return 0;
    for (const unsigned char *p = (const unsigned char *)k; *p; p++)
        if (*p < 0x21 || *p > 0x7E) return 0;
    return 1;
}

static int provider_is_valid(const char *p) {
    size_t n = strlen(p);
    if (n == 0 || n >= 64) return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if (*c < 0x20) return 0;
    return 1;
}

/* Подсказка вида «…a1B2» — сам ключ наружу не отдаём никогда. */
static void key_hint(const char *k, char *out, size_t len) {
    size_t n = strlen(k);
    snprintf(out, len, "…%s", n > 4 ? k + n - 4 : "");
}

/* После смены ключа снимаем паузы моделей этого провайдера (например, после ошибки «неверный ключ»). */
static void on_key_changed(req_t *r, const char *provider) {
    for (size_t i = 0; i < r->ctx->cfg->nmodels; i++)
        if (!strcmp(r->ctx->cfg->models[i].key_name, provider)) quota_clear_cooldown(r->ctx->quota, r->user.id, r->ctx->cfg->models[i].id);
}

static int handle_keys_get(req_t *r) {
    server_ctx *ctx = r->ctx;
    /* все провайдеры из каскада + сохранённые пользователем, которых в каскаде нет */
    char (*names)[64] = malloc(384 * sizeof *names);
    char (*stored)[64] = malloc(128 * sizeof *stored);
    if (!names || !stored) { free(names); free(stored); return fail(r, 500, "нет памяти"); }
    int n = 0;
    for (size_t i = 0; i < ctx->cfg->nmodels && n < 256; i++) {
        const model_cfg *m = &ctx->cfg->models[i];
        if (!strcmp(m->provider, "mock") || m->keyless) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (!strcmp(names[k], m->key_name)) { dup = 1; break; }
        if (!dup) xstrlcpy(names[n++], m->key_name, 64);
    }
    int ns = store_key_list(ctx->quota, r->user.id, stored, 128);
    for (int s = 0; s < ns && n < 384; s++) {
        int dup = 0;
        for (int k = 0; k < n; k++) if (!strcmp(names[k], stored[s])) { dup = 1; break; }
        if (!dup) xstrlcpy(names[n++], stored[s], 64);
    }

    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,\"keys\":[");
    for (int k = 0; k < n; k++) {
        char kb[600], hint[16] = "";
        const char *source = NULL;
        /* источник ключа — как его увидит роутер этого пользователя */
        for (size_t i = 0; i < ctx->cfg->nmodels && !source; i++) {
            const model_cfg *m = &ctx->cfg->models[i];
            if (!strcmp(m->key_name, names[k]) && router_api_key(&r->rt, m, kb, sizeof kb, &source) == NULL) source = NULL;
        }
        if (!source && store_key_get(ctx->quota, r->user.id, names[k], kb, sizeof kb)) source = "settings";
        if (source) key_hint(kb, hint, sizeof hint);
        if (k) sb_appendc(&b, ',');
        sb_append(&b, "{\"provider\":");
        sb_json_str(&b, names[k]);
        sb_printf(&b, ",\"set\":%s,\"source\":", source ? "true" : "false");
        sb_json_str(&b, source);
        sb_append(&b, ",\"hint\":");
        sb_json_str(&b, source ? hint : NULL);
        sb_append(&b, ",\"models\":[");
        int first = 1;
        for (size_t i = 0; i < ctx->cfg->nmodels; i++)
            if (!strcmp(ctx->cfg->models[i].key_name, names[k]) && !ctx->cfg->models[i].auto_added) {
                if (!first) sb_appendc(&b, ',');
                first = 0;
                sb_json_str(&b, ctx->cfg->models[i].label);
            }
        sb_append(&b, "]}");
        OPENSSL_cleanse(kb, sizeof kb);
    }
    sb_append(&b, "]}");
    free(names);
    free(stored);
    return reply(r, 200, sb_take(&b));
}

static int handle_key_set(req_t *r) {
    json *j = json_parse(r->rq->body ? r->rq->body : "", NULL, 0);
    const char *provider = json_str(json_get(j, "provider"), NULL);
    const json *kj = json_get(j, "key");
    const char *key = json_str(kj, NULL);
    if (!provider || !provider_is_valid(provider) || (kj && kj->type != JSON_STRING && kj->type != JSON_NULL)) {
        json_free(j);
        return fail(r, 400, "нужно {\"provider\": \"Groq\", \"key\": \"...\"}; пустой key удаляет ключ");
    }
    if (key && *key && !key_is_valid(key)) {
        json_free(j);
        return fail(r, 400, "ключ содержит недопустимые символы (пробелы, переводы строк) или длиннее 512 символов");
    }
    int rc = store_key_set(r->ctx->quota, r->user.id, provider, key);
    if (rc != 0) { json_free(j); return fail(r, 500, "не удалось сохранить ключ"); }
    on_key_changed(r, provider);
    char hint[16] = "";
    if (key && *key) key_hint(key, hint, sizeof hint);
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,\"provider\":");
    sb_json_str(&b, provider);
    sb_printf(&b, ",\"set\":%s,\"hint\":", key && *key ? "true" : "false");
    sb_json_str(&b, key && *key ? hint : NULL);
    sb_append(&b, "}");
    LOGI("keys", "пользователь %lld: ключ «%s» %s", (long long)r->user.id, provider, key && *key ? "сохранён" : "удалён");
    json_free(j);
    return reply(r, 200, sb_take(&b));
}

static int handle_keys_bulk(req_t *r) {
    json *j = json_parse(r->rq->body ? r->rq->body : "", NULL, 0);
    const json *keys = json_get(j, "keys");
    if (!keys || keys->type != JSON_OBJECT) {
        json_free(j);
        return fail(r, 400, "нужно {\"keys\": {\"Groq\": \"...\", ...}}");
    }
    int saved = 0, skipped = 0;
    for (const json *c = keys->child; c; c = c->next) {
        const char *k = json_str(c, NULL);
        if (!c->key || !provider_is_valid(c->key) || !k || !key_is_valid(k)) { skipped++; continue; }
        char cur[600];
        if (store_key_get(r->ctx->quota, r->user.id, c->key, cur, sizeof cur) && !strcmp(cur, k)) continue; /* без изменений */
        if (store_key_set(r->ctx->quota, r->user.id, c->key, k) == 0) { saved++; on_key_changed(r, c->key); }
    }
    json_free(j);
    if (saved) LOGI("keys", "пользователь %lld: перенесено ключей из браузера: %d", (long long)r->user.id, saved);
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"saved\":%d,\"skipped\":%d}", saved, skipped);
    return reply(r, 200, sb_take(&b));
}

static int handle_settings(req_t *r) {
    const char *method = r->rq->method;
    if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
        char err[200];
        if (settings_apply_json(r->ctx->quota, r->user.id, r->rq->body, err, sizeof err) != 0) return fail(r, 400, err);
    } else if (strcmp(method, "GET")) {
        return fail(r, 405, "используйте GET или POST");
    }
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"encryption_available\":%s,\"settings\":", crypto_available() ? "true" : "false");
    settings_to_json(r->ctx->quota, r->user.id, &b);
    sb_append(&b, "}");
    return reply(r, 200, sb_take(&b));
}

/* ---------------------------------------------------------------- статистика ---- */

/* Дни посещений и расход по дням + остатки на сегодня по лимитам провайдеров и личному дневному лимиту. */
static int handle_stats(req_t *r) {
    server_ctx *ctx = r->ctx;
    int days = query_int(r->query, "days", 371);
    if (days < 1) days = 1;
    if (days > 800) days = 800;
    router_settings *rs = malloc(sizeof *rs);
    if (!rs) return fail(r, 500, "нет памяти");
    settings_get(ctx->quota, r->user.id, rs);
    int64_t now = plat_now();
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"ok\":true,");
    stats_days_json(ctx->quota, r->user.id, days, &b);
    sb_printf(&b, ",\"daily_limit\":%lld,\"used_today\":%lld,\"limits_reset_at\":%lld,\"models\":[", rs->daily_tokens,
              stats_tokens_today(ctx->quota, r->user.id), (long long)quota_next_day(now));
    long long tok_left = 0, req_left = 0;
    int tok_models = 0, req_models = 0, first = 1;
    for (size_t i = 0; i < ctx->cfg->nmodels; i++) {
        const model_cfg *m = &ctx->cfg->models[i];
        const limits_cfg *L = &m->limits;
        if (!m->enabled || settings_is_disabled(rs, m->id) || (!L->rpd && !L->tpd && !L->rpmo)) continue;
        char kb[600];
        if (!router_api_key(&r->rt, m, kb, sizeof kb, NULL)) continue;
        OPENSSL_cleanse(kb, sizeof kb);
        quota_status st;
        quota_get_status(ctx->quota, r->user.id, m, now, &st);
        if (L->tpd) { tok_left += L->tpd > st.tokens_day ? L->tpd - st.tokens_day : 0; tok_models++; }
        if (L->rpd) { req_left += L->rpd > st.req_day ? L->rpd - st.req_day : 0; req_models++; }
        if (!first) sb_appendc(&b, ',');
        first = 0;
        sb_append(&b, "{\"id\":");
        sb_json_str(&b, m->id);
        sb_append(&b, ",\"label\":");
        sb_json_str(&b, m->label);
        sb_append(&b, ",\"provider\":");
        sb_json_str(&b, m->key_name);
        sb_append(&b, ",\"kind\":");
        sb_json_str(&b, kind_code(model_kind(m)));
        sb_printf(&b, ",\"rpd\":%d,\"req_day\":%d,\"tpd\":%ld,\"tokens_day\":%ld,\"rpmo\":%d,\"req_month\":%d,\"cooldown_until\":%lld}",
                  L->rpd, st.req_day, L->tpd, st.tokens_day, L->rpmo, st.req_month, (long long)st.cooldown_until);
    }
    sb_append(&b, "],\"left\":{\"tokens\":");
    if (tok_models) sb_printf(&b, "%lld", tok_left);
    else sb_append(&b, "null");
    sb_append(&b, ",\"requests\":");
    if (req_models) sb_printf(&b, "%lld", req_left);
    else sb_append(&b, "null");
    sb_printf(&b, ",\"token_models\":%d,\"request_models\":%d}}", tok_models, req_models);
    free(rs);
    return reply(r, 200, sb_take(&b));
}

/* ---------------------------------------------------------------- маршруты ---- */

int server_handle(server_ctx *ctx, const http_request *rq, http_reply *out) {
    memset(out, 0, sizeof *out);
    req_t r;
    memset(&r, 0, sizeof r);
    r.ctx = ctx;
    r.rq = rq;
    r.out = out;
    r.rt = *ctx->rt;
    char clean[256];
    xstrlcpy(clean, rq->path, sizeof clean);
    char *qm = strchr(clean, '?');
    if (qm) *qm = '\0';
    const char *q = strchr(rq->path, '?');
    r.query = q ? q + 1 : "";
    const char *method = rq->method;

    /* кто делает запрос */
    char token[AUTH_TOKEN_LEN + 1];
    if (auth_cookie_token(rq->cookie, token) && auth_session_user(ctx->quota, token, &r.user)) r.rt.uid = r.user.id;
    OPENSSL_cleanse(token, sizeof token);

    if (!strcmp(clean, "/api/health") && !strcmp(method, "GET")) return handle_health(&r);
    if (!strncmp(clean, "/api/auth/", 10)) return handle_auth(&r, clean);

    /* всё остальное — только после входа */
    if (!r.user.id) {
        strbuf b;
        sb_init(&b);
        sb_printf(&b, "{\"ok\":false,\"auth\":false,\"registration\":%s,\"error\":\"нужно войти\"}",
                  ctx->cfg->allow_registration ? "true" : "false");
        return reply(&r, 401, sb_take(&b));
    }
    if (!strcmp(clean, "/api/models") && !strcmp(method, "GET")) return handle_models(&r);
    if (!strcmp(clean, "/api/chat")) {
        if (strcmp(method, "POST")) return fail(&r, 405, "используйте POST");
        return handle_chat(&r);
    }
    if (!strncmp(clean, "/api/models/", 12) && !strcmp(method, "POST")) return handle_reset(&r, clean);
    if (!strcmp(clean, "/api/settings")) return handle_settings(&r);
    if (!strcmp(clean, "/api/stats") && !strcmp(method, "GET")) return handle_stats(&r);
    if (!strcmp(clean, "/api/keys") && !strcmp(method, "GET")) return handle_keys_get(&r);
    if (!strcmp(clean, "/api/keys") && (!strcmp(method, "POST") || !strcmp(method, "PUT"))) return handle_key_set(&r);
    if (!strcmp(clean, "/api/keys/bulk") && !strcmp(method, "POST")) return handle_keys_bulk(&r);
    return fail(&r, 404, "не найдено");
}

/* ------------------------------------------------------------ HTTP/сокеты ---- */

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
    }
    return "OK";
}

static int send_all(sock_t s, const char *data, size_t len) {
    while (len) {
        int n = send(s, data, (int)(len > 65536 ? 65536 : len), SEND_FLAGS);
        if (n <= 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(sock_t s, const app_config *cfg, int code, const char *body, const char *set_cookie) {
    strbuf h;
    sb_init(&h);
    size_t blen = body ? strlen(body) : 0;
    sb_printf(&h, "HTTP/1.1 %d %s\r\n", code, status_text(code));
    if (set_cookie && *set_cookie) sb_printf(&h, "Set-Cookie: %s\r\n", set_cookie);
    if (blen) sb_append(&h, "Content-Type: application/json; charset=utf-8\r\n");
    sb_printf(&h, "Content-Length: %zu\r\n", blen);
    sb_printf(&h, "Access-Control-Allow-Origin: %s\r\n", cfg->cors_origin);
    sb_append(&h, "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                   "Access-Control-Allow-Headers: Content-Type\r\n"
                   "Access-Control-Max-Age: 600\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n");
    if (!h.oom) {
        send_all(s, h.data, h.len);
        if (blen) send_all(s, body, blen);
    }
    sb_free(&h);
}

/* GET /api/media/<файл>: картинки, аудио и видео, созданные роутером. Имя файла строго проверяется —
 * никаких «..» и путей, только <hex-id>.<расширение>. */
static int serve_media(sock_t s, const app_config *cfg, int64_t uid, int64_t owner, const char *path) {
    static const struct { const char *ext, *mime; } types[] = {
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "webp", "image/webp" }, { "gif", "image/gif" },
        { "mp3", "audio/mpeg" }, { "wav", "audio/wav" }, { "ogg", "audio/ogg" }, { "mp4", "video/mp4" }, { "webm", "video/webm" },
    };
    const char *name = path + strlen("/api/media/");
    size_t len = strcspn(name, "?#");
    const char *dot = memchr(name, '.', len);
    if (!dot || dot == name || dot - name > 40) return 404;
    for (const char *p = name; p < dot; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 404;
    const char *mime = NULL;
    size_t elen = len - (size_t)(dot + 1 - name);
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (elen == strlen(types[i].ext) && !strncmp(dot + 1, types[i].ext, elen)) mime = types[i].mime;
    if (!mime) return 404;

    char dir[400], file[512];
    router_media_dir(cfg, uid, dir, sizeof dir); /* только свои файлы */
    snprintf(file, sizeof file, "%s/%.*s", dir, (int)len, name);
    FILE *f = fopen(file, "rb");
    if (!f && uid == owner) { /* файлы, созданные до появления аккаунтов, лежат прямо в media_dir — они владельца */
        snprintf(file, sizeof file, "%s/%.*s", cfg->media_dir, (int)len, name);
        f = fopen(file, "rb");
    }
    if (!f) return 404;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return 404; }
    char head[512];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Cache-Control: private, max-age=31536000, immutable\r\n"
                      "Content-Disposition: inline; filename=\"infinityAI-%.*s\"\r\n"
                      "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                      mime, size, (int)len, name);
    if (send_all(s, head, (size_t)hl) == 0) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            if (send_all(s, buf, n) != 0) break;
    }
    fclose(f);
    return 200;
}

/* Значение заголовка (без учёта регистра имени). 1 — найден. */
static int header_value(const char *headers, const char *name, char *out, size_t len) {
    size_t nl = strlen(name);
    const char *p = headers;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= len) n = len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Изменяющие запросы принимаем только от своего фронтенда: защищает от сайтов, которые пытаются
 * из браузера пользователя тратить лимиты или подменять ключи на 127.0.0.1. */
static int origin_allowed(const app_config *cfg, const char *origin) {
    if (!origin || !*origin) return 1; /* не браузер (curl, тесты) */
    if (!strcmp(cfg->cors_origin, "*") || !strcmp(origin, cfg->cors_origin)) return 1;
    char self1[64], self2[64];
    snprintf(self1, sizeof self1, "http://localhost:%d", cfg->port);
    snprintf(self2, sizeof self2, "http://127.0.0.1:%d", cfg->port);
    return !strcmp(origin, self1) || !strcmp(origin, self2);
}

static long find_content_length(const char *headers) {
    const char *p = headers;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (!strncasecmp(p, "content-length:", 15)) return strtol(p + 15, NULL, 10);
    }
    return 0;
}

typedef struct {
    server_ctx *ctx;
    sock_t      sock;
    int         is_local; /* клиент с 127.0.0.0/8 */
} conn_arg;

static void *conn_thread(void *argp) {
    conn_arg *arg = argp;
    sock_t s = arg->sock;
    server_ctx *ctx = arg->ctx;
    int arg_is_local = arg->is_local;
    free(arg);
    plat_sock_timeouts(s, IO_TIMEOUT_MS);

    strbuf in;
    sb_init(&in);
    char buf[8192];
    char *hdr_end = NULL;
    int code = 0;
    char *resp = NULL;
    char set_cookie[320] = "";

    /* заголовки */
    while (!hdr_end) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) goto done;
        sb_append_n(&in, buf, (size_t)n);
        hdr_end = strstr(in.data, "\r\n\r\n");
        if (!hdr_end && in.len > MAX_HEADER) { code = 431; resp = json_error("слишком большие заголовки"); goto reply; }
    }
    size_t hdr_len = (size_t)(hdr_end - in.data) + 4;
    long clen = find_content_length(in.data);
    if (clen < 0 || clen > MAX_BODY) { code = 413; resp = json_error("слишком большое тело запроса"); goto reply; }
    while (in.len < hdr_len + (size_t)clen) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) goto done;
        sb_append_n(&in, buf, (size_t)n);
    }
    if (in.oom) { code = 500; resp = json_error("нет памяти"); goto reply; }

    {
        char method[8] = "", path[256] = "";
        if (sscanf(in.data, "%7s %255s", method, path) != 2) { code = 400; resp = json_error("некорректный запрос"); goto reply; }
        in.data[hdr_len + (size_t)clen] = '\0';
        const char *body = in.data + hdr_len;
        if (!strcmp(method, "OPTIONS")) { code = 204; goto reply; } /* CORS preflight */
        char origin[256] = "";
        in.data[hdr_len - 2] = '\0'; /* временно ограничиваем поиск заголовками */
        header_value(in.data, "origin", origin, sizeof origin);
        in.data[hdr_len - 2] = '\r';
        if (strcmp(method, "GET") && !origin_allowed(ctx->cfg, origin)) {
            LOGW("http", "отклонён %s %s с чужого Origin: %s", method, path, origin);
            code = 403;
            resp = json_error("запрос с чужого сайта отклонён");
            goto reply;
        }
        char cookie[1024] = "";
        in.data[hdr_len - 2] = '\0';
        header_value(in.data, "cookie", cookie, sizeof cookie);
        in.data[hdr_len - 2] = '\r';
        int64_t t0 = plat_now_ms();
        if (!strcmp(method, "GET") && !strncmp(path, "/api/media/", 11)) {
            char token[AUTH_TOKEN_LEN + 1];
            auth_user u;
            memset(&u, 0, sizeof u);
            if (!auth_cookie_token(cookie, token) || !auth_session_user(ctx->quota, token, &u)) {
                code = 401;
                resp = json_error("нужно войти");
                goto reply;
            }
            if (serve_media(s, ctx->cfg, u.id, quota_owner_uid(ctx->quota), path) == 200) goto done;
            code = 404;
            resp = json_error("файл не найден");
            goto reply;
        }
        http_request rq = { method, path, body, cookie, arg_is_local };
        http_reply out;
        code = server_handle(ctx, &rq, &out);
        resp = out.body;
        xstrlcpy(set_cookie, out.set_cookie, sizeof set_cookie);
        LOGD("http", "%s %s → %d (%lld мс)", method, path, code, (long long)(plat_now_ms() - t0));
    }

reply:
    send_response(s, ctx->cfg, code, resp, set_cookie);
done:
    free(resp);
    sb_free(&in);
    sock_close(s);
    __sync_fetch_and_sub(&g_clients, 1);
    return NULL;
}

void server_stop(void) {
    g_running = 0;
    sock_t l = g_listen;
    if (l != SOCK_INVALID) {
        g_listen = SOCK_INVALID;
#ifdef _WIN32
        closesocket(l);
#else
        shutdown(l, SHUT_RDWR);
        close(l);
#endif
    }
}

#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type) { (void)type; server_stop(); return TRUE; }
#else
static void on_signal(int sig) { (void)sig; server_stop(); }
#endif

int server_run(server_ctx *ctx) {
    sock_t l = socket(AF_INET, SOCK_STREAM, 0);
    if (l == SOCK_INVALID) { LOGE("server", "socket() не удался"); return -1; }
    int yes = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)ctx->cfg->port);
    if (inet_pton(AF_INET, ctx->cfg->host, &addr.sin_addr) != 1) {
        LOGE("server", "некорректный host \"%s\" (нужен IPv4, например 127.0.0.1 или 0.0.0.0)", ctx->cfg->host);
        sock_close(l);
        return -1;
    }
    if (bind(l, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOGE("server", "не удалось занять %s:%d — порт занят?", ctx->cfg->host, ctx->cfg->port);
        sock_close(l);
        return -1;
    }
    if (listen(l, 64) != 0) { LOGE("server", "listen() не удался"); sock_close(l); return -1; }
    g_listen = l;
    g_running = 1;

#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* без SA_RESTART, чтобы accept() прервался */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    LOGI("server", "Слушаю http://%s:%d  (Ctrl+C — остановить)", ctx->cfg->host, ctx->cfg->port);
    while (g_running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        sock_t c = accept(l, (struct sockaddr *)&cli, &cl);
        if (c == SOCK_INVALID) {
            if (!g_running) break;
            continue;
        }
        if (g_clients >= MAX_CLIENTS) {
            char *busy = json_error("сервер перегружен, повторите позже");
            send_response(c, ctx->cfg, 503, busy, NULL);
            free(busy);
            sock_close(c);
            continue;
        }
        conn_arg *arg = malloc(sizeof *arg);
        if (!arg) { sock_close(c); continue; }
        arg->ctx = ctx;
        arg->sock = c;
        arg->is_local = (ntohl(cli.sin_addr.s_addr) >> 24) == 127;
        __sync_fetch_and_add(&g_clients, 1);
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, arg) != 0) {
            __sync_fetch_and_sub(&g_clients, 1);
            sock_close(c);
            free(arg);
            continue;
        }
        pthread_detach(th);
    }
    /* даём активным запросам завершиться (до 5 секунд) */
    for (int i = 0; i < 50 && g_clients > 0; i++) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
    LOGI("server", "Остановлен");
    return 0;
}
