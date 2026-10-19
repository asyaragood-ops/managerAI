/* Тесты ядра роутера: JSON, классификация ошибок, паузы, каскад, локальные лимиты, API-обработчик.
 * Сеть не нужна — используются mock-провайдеры. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "config.h"
#include "crypto.h"
#include "settings.h"
#include "auth.h"
#include "stats.h"
#include "json.h"
#include "log.h"
#include "platform.h"
#include "providers/provider.h"
#include "quota.h"
#include "router.h"
#include "server.h"
#include "strbuf.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) g_pass++;                                                         \
        else { g_fail++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void test_json(void) {
    char err[128];
    json *j = json_parse("{\"a\":[1,2,{\"b\":\"при\\u0432\\u0435\\u0442 \\ud83d\\ude00\"}],\"t\":true,\"n\":null,\"x\":-1.5e2}", err, sizeof err);
    CHECK(j != NULL);
    CHECK(json_num(json_path(j, "a.1"), 0) == 2);
    CHECK(!strcmp(json_str(json_path(j, "a.2.b"), ""), "привет 😀"));
    CHECK(json_bool(json_get(j, "t"), 0) == 1);
    CHECK(json_get(j, "n")->type == JSON_NULL);
    CHECK(json_num(json_get(j, "x"), 0) == -150);
    CHECK(json_path(j, "a.9") == NULL);
    json_free(j);
    CHECK(json_parse("{\"a\":1,}", err, sizeof err) == NULL);
    CHECK(json_parse("[1,2", err, sizeof err) == NULL);
    CHECK(json_parse("{} x", err, sizeof err) == NULL);

    strbuf sb;
    sb_init(&sb);
    sb_json_str(&sb, "кавычка \" слэш \\ перевод\nстроки\t\x01");
    json *r = json_parse(sb.data, err, sizeof err);
    CHECK(r && !strcmp(r->str, "кавычка \" слэш \\ перевод\nстроки\t\x01"));
    json_free(r);
    sb_free(&sb);
}

static void test_classify(void) {
    CHECK(classify_http_error(429, "{\"error\":{\"message\":\"Rate limit reached for requests\"}}") == RES_RATE_LIMIT);
    CHECK(classify_http_error(429, "{\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"You exceeded your current quota\"}}") == RES_QUOTA);
    CHECK(classify_http_error(429, "{\"error\":{\"code\":\"insufficient_quota\"}}") == RES_QUOTA);
    CHECK(classify_http_error(402, "") == RES_QUOTA);
    CHECK(classify_http_error(401, "") == RES_AUTH);
    CHECK(classify_http_error(400, "{\"error\":{\"message\":\"API key not valid\"}}") == RES_AUTH);
    CHECK(classify_http_error(400, "{\"error\":{\"message\":\"bad\"}}") == RES_BAD_REQUEST);
    CHECK(classify_http_error(529, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}") == RES_SERVER);
    CHECK(classify_http_error(0, "") == RES_NETWORK);

    call_result r;
    memset(&r, 0, sizeof r);
    fill_http_error(&r, 429,
                    "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota, please check your plan and billing details\","
                    "\"status\":\"RESOURCE_EXHAUSTED\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.RetryInfo\",\"retryDelay\":\"34s\"}]}}",
                    0, NULL);
    CHECK(r.kind == RES_QUOTA);
    CHECK(r.retry_after_s == 34);
    CHECK(r.limit_billing == 0); /* «billing details» у Google — не признак кончившихся денег */
    CHECK(strstr(r.error, "HTTP 429") != NULL);
}

static void test_cooldown(void) {
    int64_t now = 1792370000; /* фиксированное время */
    call_result r;
    memset(&r, 0, sizeof r);
    r.kind = RES_RATE_LIMIT;
    CHECK(router_cooldown_seconds(&r, now) == 60);
    r.retry_after_s = 12;
    CHECK(router_cooldown_seconds(&r, now) == 12);
    memset(&r, 0, sizeof r);
    r.kind = RES_QUOTA;
    r.limit_daily = 1;
    CHECK(router_cooldown_seconds(&r, now) == (long)(quota_next_day(now) - now));
    r.limit_daily = 0;
    r.limit_billing = 1;
    CHECK(router_cooldown_seconds(&r, now) == 3600);
    r.limit_billing = 0;
    CHECK(router_cooldown_seconds(&r, now) == 60);
    r.kind = RES_BAD_REQUEST;
    CHECK(router_cooldown_seconds(&r, now) == 0);
}

static model_cfg mk(const char *id, const char *label, const char *behavior) {
    model_cfg m;
    memset(&m, 0, sizeof m);
    xstrlcpy(m.id, id, sizeof m.id);
    xstrlcpy(m.label, label, sizeof m.label);
    xstrlcpy(m.provider, "mock", sizeof m.provider);
    xstrlcpy(m.tier, "free", sizeof m.tier);
    xstrlcpy(m.mock_behavior, behavior, sizeof m.mock_behavior);
    m.enabled = 1;
    m.max_tokens = 100;
    return m;
}

static app_config mkcfg(model_cfg *models, size_t n) {
    app_config c;
    memset(&c, 0, sizeof c);
    c.models = models;
    c.nmodels = n;
    c.max_history = 20;
    c.timeout_ms = 1000;
    xstrlcpy(c.cors_origin, "*", sizeof c.cors_origin);
    xstrlcpy(c.media_dir, "test_media", sizeof c.media_dir);
    return c;
}

static const chat_msg HELLO[] = { { "user", "Привет" } };

/* Тестовый пользователь (создаётся в main) — API требует входа. */
static auth_user U1;
static char COOKIE1[128];

static int api(server_ctx *ctx, const char *method, const char *path, const char *body, const char *cookie, char **resp,
               char *set_cookie) {
    http_request rq = { method, path, body, cookie, 1 };
    http_reply out;
    int code = server_handle(ctx, &rq, &out);
    *resp = out.body;
    if (set_cookie) strcpy(set_cookie, out.set_cookie);
    return code;
}
#define API(m, p, b, r) api(&ctx, m, p, b, COOKIE1, r, NULL)

static void test_cascade(quota_db *q) {
    /* ИИ №1 упирается в квоту → отвечает ИИ №2 */
    model_cfg ms[2] = { mk("gemini", "Gemini", "quota"), mk("gpt", "GPT-4o", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.ok);
    CHECK(rep.model == &ms[1]);
    CHECK(rep.switched == 1);
    CHECK(rep.nattempts == 2);
    CHECK(rep.attempts[0].kind == RES_QUOTA && rep.attempts[0].http_status == 429);
    CHECK(rep.attempts[1].kind == RES_OK);
    char *js = router_reply_json(&rep);
    json *j = json_parse(js, NULL, 0);
    CHECK(j && !strcmp(json_str(json_get(j, "badge"), ""), "Ответил: GPT-4o"));
    CHECK(!strcmp(json_str(json_path(j, "model.id"), ""), "gpt"));
    CHECK(json_bool(json_get(j, "switched"), 0) == 1);
    CHECK(!strcmp(json_str(json_path(j, "attempts.0.status"), ""), "quota"));
    CHECK(json_bool(json_path(j, "attempts.0.limit"), 0) == 1);
    json_free(j);
    free(js);
    router_reply_free(&rep);

    /* второй запрос: Gemini на паузе — даже не вызывается */
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    CHECK(rep.attempts[0].kind == RES_COOLDOWN);
    router_reply_free(&rep);

    /* после сброса Gemini снова вызывается (и снова упирается в квоту) */
    quota_reset(q, 0, "gemini");
    router_chat(&rt, HELLO, 1, "auto", NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_QUOTA);
    router_reply_free(&rep);
    quota_reset(q, 0, "gemini");
}

static void set_settings(quota_db *q, const char *js) {
    char err[200];
    if (settings_apply_json(q, 0, js, err, sizeof err) != 0) fprintf(stderr, "  settings: %s\n", err);
}
#define DEFAULTS "{\"autoSwitch\":true,\"threshold\":90,\"strategy\":\"free\",\"keepContext\":true,\"notifySwitch\":true,\"encryptKeys\":false,\"disabled\":[]}"

static void test_local_limit(quota_db *q) {
    /* «дешевле» — строго по порядку конфига: «свободнее» сам обошёл бы исчерпанную модель */
    set_settings(q, "{\"threshold\":100,\"strategy\":\"cheap\"}");
    model_cfg ms[2] = { mk("free1", "Free", "ok"), mk("paid", "Paid", "ok") };
    ms[0].limits.rpd = 2;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    for (int i = 0; i < 2; i++) {
        router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
        CHECK(rep.ok && rep.model == &ms[0] && !rep.switched);
        router_reply_free(&rep);
    }
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.switched);
    CHECK(rep.attempts[0].kind == RES_LOCAL_LIMIT);
    router_reply_free(&rep);
    set_settings(q, DEFAULTS);
}

static void test_errors_and_order(quota_db *q) {
    /* сеть/5xx/неверный ключ тоже ведут к резерву; ключ не задан — модель пропускается */
    model_cfg ms[5] = { mk("net", "Net", "network"), mk("srv", "Srv", "server_error"), mk("auth", "Auth", "auth_error"),
                        mk("nokey", "NoKey", "ok"), mk("last", "Last", "ok") };
    xstrlcpy(ms[3].provider, "openai", sizeof ms[3].provider);
    xstrlcpy(ms[3].api_key_env, "IAI_TEST_UNSET_KEY_42", sizeof ms[3].api_key_env);
    app_config cfg = mkcfg(ms, 5);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[4]);
    CHECK(rep.nattempts == 5);
    CHECK(rep.attempts[0].kind == RES_NETWORK);
    CHECK(rep.attempts[1].kind == RES_SERVER);
    CHECK(rep.attempts[2].kind == RES_AUTH);
    CHECK(rep.attempts[3].kind == RES_NOT_CONFIGURED);
    router_reply_free(&rep);
    for (int i = 0; i < 3; i++) quota_reset(q, 0, ms[i].id);

    /* закреплённая модель идёт первой; навык поднимает подходящие модели */
    model_cfg ms2[3] = { mk("a", "A", "ok"), mk("b", "B", "ok"), mk("c", "C", "ok") };
    xstrlcpy(ms2[2].skills[0], "code", sizeof ms2[2].skills[0]);
    ms2[2].nskills = 1;
    xstrlcpy(ms2[0].skills[0], "text", sizeof ms2[0].skills[0]);
    ms2[0].nskills = 1;
    app_config cfg2 = mkcfg(ms2, 3);
    router rt2 = { &cfg2, q };
    router_chat(&rt2, HELLO, 1, "b", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms2[1] && !rep.switched);
    router_reply_free(&rep);
    router_chat(&rt2, HELLO, 1, "auto", "code", &rep);
    CHECK(rep.ok && rep.model == &ms2[2]); /* «a» умеет только text, «b» универсальная, но «c» явно code */
    router_reply_free(&rep);

    /* все недоступны → ok=false и понятная ошибка */
    model_cfg bad[2] = { mk("x1", "X1", "quota"), mk("x2", "X2", "rate_limit") };
    app_config cfg3 = mkcfg(bad, 2);
    router rt3 = { &cfg3, q };
    router_chat(&rt3, HELLO, 1, NULL, NULL, &rep);
    CHECK(!rep.ok && rep.nattempts == 2 && strstr(rep.error, "каскаде"));
    router_reply_free(&rep);
}

static void test_history(quota_db *q) {
    /* склейка подряд идущих ролей и отбрасывание ведущего ответа ассистента не ломают запрос */
    model_cfg ms[1] = { mk("echo", "Echo", "ok") };
    app_config cfg = mkcfg(ms, 1);
    router rt = { &cfg, q };
    chat_msg msgs[] = { { "assistant", "приветствие" }, { "user", "раз" }, { "user", "два" }, { "assistant", "" }, { "user", "три" } };
    router_reply rep;
    router_chat(&rt, msgs, 5, NULL, NULL, &rep);
    CHECK(rep.ok && strstr(rep.text, "три"));
    router_reply_free(&rep);
}

static void test_api(quota_db *q) {
    model_cfg ms[2] = { mk("gemini", "Gemini", "rate_limit"), mk("gpt", "GPT-4o", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;
    CHECK(API("GET", "/api/health", "", &resp) == 200);
    free(resp);
    /* без входа — только /api/health и /api/auth/… */
    CHECK(api(&ctx, "GET", "/api/health", "", NULL, &resp, NULL) == 200);
    free(resp);
    CHECK(api(&ctx, "GET", "/api/models", "", NULL, &resp, NULL) == 401 && strstr(resp, "\"auth\":false"));
    free(resp);
    CHECK(api(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", "iai_session=deadbeef", &resp, NULL) == 401);
    free(resp);
    CHECK(API("POST", "/api/chat", "не json", &resp) == 400);
    free(resp);
    CHECK(API("POST", "/api/chat", "{\"messages\":[]}", &resp) == 400);
    free(resp);
    CHECK(API("POST", "/api/chat", "{\"messages\":[{\"role\":\"system\",\"content\":\"x\"}]}", &resp) == 400);
    free(resp);
    CHECK(API("GET", "/api/chat", "", &resp) == 405);
    free(resp);
    int code = API("POST", "/api/chat",
                             "{\"messages\":[{\"role\":\"user\",\"content\":\"Сделай сценарий\"}],\"model\":\"auto\",\"skill\":\"scripts\"}", &resp);
    CHECK(code == 200);
    json *j = json_parse(resp, NULL, 0);
    CHECK(j && !strcmp(json_str(json_get(j, "badge"), ""), "Ответил: GPT-4o"));
    CHECK(!strcmp(json_str(json_path(j, "attempts.0.status"), ""), "rate_limit"));
    json_free(j);
    free(resp);
    CHECK(API("GET", "/api/models", "", &resp) == 200);
    j = json_parse(resp, NULL, 0);
    CHECK(j && json_len(json_get(j, "models")) == 2);
    CHECK(json_num(json_path(j, "models.0.cooldown_until"), 0) > 0); /* Gemini на паузе после 429 */
    CHECK(json_num(json_path(j, "models.1.usage.day"), 0) >= 1);
    json_free(j);
    free(resp);
    CHECK(API("POST", "/api/models/gemini/reset", "", &resp) == 200);
    free(resp);
    CHECK(API("GET", "/nope", "", &resp) == 404);
    free(resp);
}

static void test_keys(quota_db *q) {
    /* ключ из «Настроек» сохраняется на сервере и используется роутером вместо .env */
    model_cfg ms[2] = { mk("groq", "Llama", "ok"), mk("fallback", "Fallback", "ok") };
    xstrlcpy(ms[0].provider, "openai", sizeof ms[0].provider);
    xstrlcpy(ms[0].model, "llama", sizeof ms[0].model);
    xstrlcpy(ms[0].base_url, "http://127.0.0.1:9", sizeof ms[0].base_url); /* закрытый порт → быстрая сетевая ошибка */
    xstrlcpy(ms[0].key_name, "Groq", sizeof ms[0].key_name);
    app_config cfg = mkcfg(ms, 2);
    router shared = { &cfg, q, 0 };
    server_ctx ctx = { &cfg, &shared, q };
    router rt = { &cfg, q, U1.id }; /* роутер того же пользователя, что вошёл в API */
    char *resp;
    router_reply rep;

    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_NOT_CONFIGURED && strstr(rep.attempts[0].message, "Groq"));
    router_reply_free(&rep);

    CHECK(api(&ctx, "POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"gsk_secret_ABCD\"}", NULL, &resp, NULL) == 401);
    free(resp);
    CHECK(API("POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"bad\\r\\nX-Evil: 1\"}", &resp) == 400);
    free(resp);
    CHECK(API("POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"gsk_secret_ABCD\"}", &resp) == 200);
    CHECK(strstr(resp, "\"hint\":\"…ABCD\"") && !strstr(resp, "gsk_secret"));
    free(resp);

    char kb[600];
    const char *src = NULL;
    CHECK(router_api_key(&rt, &ms[0], kb, sizeof kb, &src) && !strcmp(kb, "gsk_secret_ABCD") && !strcmp(src, "settings"));
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_NETWORK); /* ключ взят — модель реально вызывалась */
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);

    CHECK(API("GET", "/api/keys", "", &resp) == 200);
    CHECK(strstr(resp, "\"provider\":\"Groq\",\"set\":true,\"source\":\"settings\"") && !strstr(resp, "gsk_secret"));
    free(resp);

    /* смена ключа снимает паузу модели после ошибки */
    CHECK(API("POST", "/api/keys/bulk", "{\"keys\":{\"Groq\":\"gsk_new_EFGH\",\"OpenAI\":\"sk-1234\",\"Bad\":\"a b\"}}", &resp) == 200);
    CHECK(strstr(resp, "\"saved\":2") && strstr(resp, "\"skipped\":1"));
    free(resp);
    quota_status st;
    quota_get_status(q, U1.id, &ms[0], plat_now(), &st);
    CHECK(st.cooldown_until == 0);
    CHECK(store_key_get(q, U1.id, "Groq", kb, sizeof kb) && !strcmp(kb, "gsk_new_EFGH"));
    CHECK(!store_key_get(q, 0, "Groq", kb, sizeof kb)); /* ключи других пользователей не затронуты */

    /* пустой ключ удаляет */
    CHECK(API("POST", "/api/keys", "{\"provider\":\"Groq\",\"key\":\"\"}", &resp) == 200);
    free(resp);
    CHECK(!store_key_get(q, U1.id, "Groq", kb, sizeof kb));
    store_key_set(q, U1.id, "OpenAI", NULL);
}

/* ---------------- настройки роутера ---------------- */

static void test_settings_api(quota_db *q) {
    set_settings(q, DEFAULTS);
    model_cfg ms[1] = { mk("a", "A", "ok") };
    app_config cfg = mkcfg(ms, 1);
    router rt = { &cfg, q };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;
    CHECK(API("GET", "/api/settings", "", &resp) == 200);
    CHECK(strstr(resp, "\"threshold\":90") && strstr(resp, "\"strategy\":\"free\"") && strstr(resp, "\"encryption_available\":true"));
    free(resp);
    CHECK(API("POST", "/api/settings", "{\"threshold\":30}", &resp) == 400);
    free(resp);
    CHECK(API("POST", "/api/settings", "{\"strategy\":\"fastest\"}", &resp) == 400);
    free(resp);
    CHECK(api(&ctx, "POST", "/api/settings", "{\"threshold\":75}", NULL, &resp, NULL) == 401);
    free(resp);
    CHECK(API("POST", "/api/settings", "{\"threshold\":75,\"strategy\":\"quality\"}", &resp) == 200);
    CHECK(strstr(resp, "\"threshold\":75") && strstr(resp, "\"strategy\":\"quality\"") && strstr(resp, "\"autoSwitch\":true"));
    free(resp);
    /* настройки сохраняются в БД, и они личные */
    router_settings s;
    settings_get(q, U1.id, &s);
    CHECK(s.threshold == 75 && s.strategy == STRATEGY_QUALITY);
    settings_get(q, 0, &s);
    CHECK(s.threshold == 90 && s.strategy == STRATEGY_FREE);
    CHECK(API("POST", "/api/settings", DEFAULTS, &resp) == 200);
    free(resp);
    set_settings(q, DEFAULTS);
}

static void test_threshold(quota_db *q) {
    /* порог 50%: при лимите 10 запросов/сутки переключение после 5-го */
    set_settings(q, "{\"threshold\":50}");
    model_cfg ms[2] = { mk("thr", "Thr", "ok"), mk("res", "Res", "ok") };
    ms[0].limits.rpd = 10;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    int first = 0;
    for (int i = 0; i < 7; i++) {
        router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
        if (rep.model == &ms[0]) first++;
        router_reply_free(&rep);
    }
    CHECK(first == 5);
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.attempts[0].kind == RES_LOCAL_LIMIT && strstr(rep.attempts[0].message, "порог 50%"));
    router_reply_free(&rep);
    /* при выключенном автопереключении порог не действует — модель работает до 100% */
    set_settings(q, "{\"autoSwitch\":false}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[0]);
    router_reply_free(&rep);
    quota_reset(q, 0, "thr");
    quota_reset(q, 0, "res");
    set_settings(q, DEFAULTS);
}

static void test_autoswitch_off(quota_db *q) {
    model_cfg ms[2] = { mk("main", "Main", "quota"), mk("backup", "Backup", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"autoSwitch\":false}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(!rep.ok && rep.nattempts == 1 && strstr(rep.error, "автопереключение"));
    router_reply_free(&rep);
    quota_reset(q, 0, "main");
    set_settings(q, "{\"autoSwitch\":true}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);
    quota_reset(q, 0, "main");
    set_settings(q, DEFAULTS);
}

static void test_strategies(quota_db *q) {
    /* f1: бесплатная, слабая, загружена на 80%; f2: бесплатная, средняя, свободна; p: платная, сильная */
    model_cfg ms[3] = { mk("f1", "F1", "ok"), mk("f2", "F2", "ok"), mk("p", "P", "ok") };
    ms[0].priority = 3; ms[0].limits.rpd = 10;
    ms[1].priority = 2; ms[1].limits.rpd = 10;
    ms[2].priority = 1; xstrlcpy(ms[2].tier, "paid", sizeof ms[2].tier);
    for (int i = 0; i < 8; i++) quota_record(q, 0, &ms[0], plat_now(), 1);
    app_config cfg = mkcfg(ms, 3);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"strategy\":\"cheap\",\"threshold\":100}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[0]); /* дешевле: бесплатные по порядку конфига */
    router_reply_free(&rep);
    set_settings(q, "{\"strategy\":\"free\"}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[1]); /* свободнее: бесплатная с наибольшим запасом */
    router_reply_free(&rep);
    set_settings(q, "{\"strategy\":\"quality\"}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.model == &ms[2]); /* качественнее: сильнейшая, даже платная */
    router_reply_free(&rep);
    for (int i = 0; i < 3; i++) quota_reset(q, 0, ms[i].id);
    set_settings(q, DEFAULTS);
}

static void test_keep_context(quota_db *q) {
    model_cfg ms[2] = { mk("lim", "Lim", "rate_limit"), mk("ctx", "Ctx", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    chat_msg hist[] = { { "user", "раз" }, { "assistant", "ответ" }, { "user", "два" } };
    router_reply rep;
    router_chat(&rt, hist, 3, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && strstr(rep.text, "сообщений: 3"));
    router_reply_free(&rep);
    quota_reset(q, 0, "lim");
    set_settings(q, "{\"keepContext\":false}");
    router_chat(&rt, hist, 3, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && strstr(rep.text, "сообщений: 1") && strstr(rep.text, "два"));
    router_reply_free(&rep);
    quota_reset(q, 0, "lim");
    set_settings(q, DEFAULTS);
}

static void test_disabled(quota_db *q) {
    model_cfg ms[2] = { mk("off", "Off", "ok"), mk("on", "On", "ok") };
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    set_settings(q, "{\"disabled\":[\"off\"]}");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.nattempts == 1 && !rep.switched);
    router_reply_free(&rep);
    /* закрепить выключенную модель тоже нельзя */
    router_chat(&rt, HELLO, 1, "off", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1]);
    router_reply_free(&rep);
    set_settings(q, DEFAULTS);
}

static void test_catalog_auto(quota_db *q) {
    /* модель каталога без ключа в каскаде не участвует, с ключом — участвует */
    model_cfg ms[2] = { mk("cat-x", "CatX", "ok"), mk("last", "Last", "ok") };
    xstrlcpy(ms[0].provider, "openai", sizeof ms[0].provider);
    xstrlcpy(ms[0].model, "x", sizeof ms[0].model);
    xstrlcpy(ms[0].base_url, "http://127.0.0.1:9", sizeof ms[0].base_url);
    xstrlcpy(ms[0].key_name, "TestProv", sizeof ms[0].key_name);
    ms[0].auto_added = 1;
    app_config cfg = mkcfg(ms, 2);
    router rt = { &cfg, q };
    router_reply rep;
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[1] && rep.nattempts == 1 && !rep.switched);
    router_reply_free(&rep);
    store_key_set(q, 0, "TestProv", "tp-key");
    router_chat(&rt, HELLO, 1, NULL, NULL, &rep);
    CHECK(rep.nattempts == 2 && rep.attempts[0].model == &ms[0] && rep.attempts[0].kind == RES_NETWORK);
    router_reply_free(&rep);
    store_key_set(q, 0, "TestProv", NULL);
    quota_reset(q, 0, "cat-x");

    /* загрузка catalog.json: дубликат id из config.json пропускается */
    const char *path = "test_catalog.json";
    FILE *f = fopen(path, "w");
    fputs("{\"models\":[{\"id\":\"last\",\"label\":\"Dup\",\"provider\":\"openai\",\"model\":\"m\",\"key_name\":\"Groq\"},"
          "{\"id\":\"cat-groq-y\",\"label\":\"Y\",\"provider\":\"openai\",\"base_url\":\"https://api.groq.com/openai/v1\",\"model\":\"y\","
          "\"tier\":\"free\",\"priority\":1,\"limits\":{\"rpd\":1000}},{\"bad\":true}]}", f);
    fclose(f);
    model_cfg *buf = calloc(CFG_MAX_MODELS, sizeof *buf);
    buf[0] = mk("last", "Last", "ok");
    app_config c2 = mkcfg(buf, 1);
    char err[200];
    CHECK(config_load_catalog(&c2, path, err, sizeof err) == 1);
    CHECK(c2.nmodels == 2 && c2.models[1].auto_added && !strcmp(c2.models[1].key_name, "Groq") && c2.models[1].limits.rpd == 1000);
    free(buf);
    remove(path);
}

static void test_cloudflare_key(void) {
    model_cfg m = mk("cf", "CF", "ok");
    xstrlcpy(m.provider, "openai", sizeof m.provider);
    xstrlcpy(m.model, "@cf/meta/llama", sizeof m.model);
    xstrlcpy(m.key_name, "Cloudflare Workers AI", sizeof m.key_name);
    xstrlcpy(m.base_url, "http://127.0.0.1:9/{account}/ai/v1", sizeof m.base_url);
    chat_request req = { HELLO, 1, NULL };
    call_result r;
    provider_openai(&m, "token-without-account", &req, 1000, &r);
    CHECK(r.kind == RES_AUTH && strstr(r.error, "ACCOUNT_ID:API_TOKEN"));
    provider_openai(&m, "acc123:tok", &req, 1000, &r);
    CHECK(r.kind == RES_NETWORK); /* формат принят, дошли до сети */
    call_result_free(&r);
    call_result c;
    memset(&c, 0, sizeof c);
    c.kind = RES_BAD_REQUEST;
    c.http_status = 404;
    CHECK(router_cooldown_seconds(&c, 0) == 6 * 3600);
}

/* ---------------- шифрование ключей ---------------- */

static void test_encryption(quota_db *q) {
    char enc[4096], dec[600], raw[4096];
    CHECK(crypto_available());
    CHECK(crypto_encrypt("sk-secret-123", enc, sizeof enc) == 0 && crypto_is_encrypted(enc) && !strstr(enc, "secret"));
    CHECK(crypto_decrypt(enc, dec, sizeof dec) == 0 && !strcmp(dec, "sk-secret-123"));
    char enc2[4096];
    crypto_encrypt("sk-secret-123", enc2, sizeof enc2);
    CHECK(strcmp(enc, enc2) != 0); /* случайный nonce: одинаковые ключи шифруются по-разному */
    char bad[4096];
    strcpy(bad, enc);
    bad[strlen(bad) - 6] = bad[strlen(bad) - 6] == 'A' ? 'B' : 'A'; /* подмена байта */
    CHECK(crypto_decrypt(bad, dec, sizeof dec) != 0);

    /* включение шифрования перешифровывает уже сохранённые ключи */
    store_key_set(q, 0, "EncProv", "plain-key-777");
    CHECK(store_key_get_raw(q, 0, "EncProv", raw, sizeof raw) && !strcmp(raw, "plain-key-777"));
    set_settings(q, "{\"encryptKeys\":true}");
    CHECK(store_key_get_raw(q, 0, "EncProv", raw, sizeof raw) && crypto_is_encrypted(raw) && !strstr(raw, "777"));
    CHECK(store_key_get(q, 0, "EncProv", dec, sizeof dec) && !strcmp(dec, "plain-key-777"));
    store_key_set(q, 0, "EncProv2", "new-key-888"); /* новые ключи тоже шифруются */
    CHECK(store_key_get_raw(q, 0, "EncProv2", raw, sizeof raw) && crypto_is_encrypted(raw));
    /* выключение — обратно в открытый вид */
    set_settings(q, "{\"encryptKeys\":false}");
    CHECK(store_key_get_raw(q, 0, "EncProv", raw, sizeof raw) && !strcmp(raw, "plain-key-777"));
    CHECK(store_key_get_raw(q, 0, "EncProv2", raw, sizeof raw) && !strcmp(raw, "new-key-888"));

    /* чужой мастер-ключ не расшифрует */
    crypto_shutdown();
    remove("test_master2.key");
    char err[200];
    CHECK(crypto_init("test_master2.key", err, sizeof err) == 0);
    CHECK(crypto_decrypt(enc, dec, sizeof dec) != 0);
    crypto_shutdown();
    CHECK(crypto_init("test_master.key", err, sizeof err) == 0); /* обратно свой ключ */
    CHECK(crypto_decrypt(enc, dec, sizeof dec) == 0);
    remove("test_master2.key");
    store_key_set(q, 0, "EncProv", NULL);
    store_key_set(q, 0, "EncProv2", NULL);
    set_settings(q, DEFAULTS);
}

/* ---------------- картинки, аудио, видео ---------------- */

static model_cfg mk_media(const char *id, const char *label, const char *behavior, const char *kind) {
    model_cfg m = mk(id, label, behavior);
    xstrlcpy(m.kind, kind, sizeof m.kind);
    return m;
}

static void test_detect_kind(void) {
    CHECK(router_detect_kind("Нарисуй кота в шляпе", NULL) == KIND_IMAGE);
    CHECK(router_detect_kind("Сделай логотип для кофейни", NULL) == KIND_IMAGE);
    CHECK(router_detect_kind("СГЕНЕРИРУЙ КАРТИНКУ: закат над морем", NULL) == KIND_IMAGE);
    CHECK(router_detect_kind("draw a cat on a skateboard", NULL) == KIND_IMAGE);
    CHECK(router_detect_kind("Сгенерируй видео: волны на рассвете", NULL) == KIND_VIDEO);
    CHECK(router_detect_kind("Make a video of a rocket launch", NULL) == KIND_VIDEO);
    CHECK(router_detect_kind("Озвучь: добрый вечер", NULL) == KIND_AUDIO);
    CHECK(router_detect_kind("прочитай вслух предыдущий ответ", NULL) == KIND_AUDIO);
    /* обычные вопросы — текст */
    CHECK(router_detect_kind("Как нарисовать график в Excel?", NULL) == KIND_CHAT);
    CHECK(router_detect_kind("Что такое видеокарта?", NULL) == KIND_CHAT);
    CHECK(router_detect_kind("Привет", NULL) == KIND_CHAT);
    /* специализация чата */
    CHECK(router_detect_kind("кот в космосе", "images") == KIND_IMAGE);
    CHECK(router_detect_kind("как убрать шум из записи?", "audio") == KIND_CHAT);
    CHECK(router_detect_kind("как сделать плавный переход?", "video") == KIND_CHAT);
    CHECK(router_detect_kind("кот в космосе", "marketing") == KIND_CHAT);
}

static void test_media_utils(void) {
    size_t n = 0;
    unsigned char *d = b64_decode("iVBORw0KGgo=", 12, &n); /* сигнатура PNG */
    CHECK(d && n == 8 && sniff_mime(d, n) && !strcmp(sniff_mime(d, n), "image/png"));
    free(d);
    CHECK(b64_decode("@@@", 3, &n) == NULL);
    const unsigned char jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    CHECK(!strcmp(sniff_mime(jpg, 4), "image/jpeg"));
    const unsigned char mp4[] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm' };
    CHECK(!strcmp(sniff_mime(mp4, sizeof mp4), "video/mp4"));
    const unsigned char pcm[4] = { 1, 2, 3, 4 };
    size_t wl = 0;
    unsigned char *w = wav_from_pcm(pcm, 4, 24000, 1, &wl);
    CHECK(w && wl == 48 && !strcmp(sniff_mime(w, wl), "audio/wav") && w[24] == 0xC0 && w[25] == 0x5D && w[40] == 4 && w[44] == 1);
    free(w);
    CHECK(has_cyrillic("кот") && !has_cyrillic("cat"));
}

static int file_exists(const char *dir, const char *name) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void test_media_cascade(quota_db *q) {
    /* текстовая модель готовит промпт; первая «рисовалка» упирается в квоту → рисует вторая */
    model_cfg ms[4] = { mk("txt", "Txt", "ok"), mk_media("img1", "Img1", "quota", "image"), mk_media("img2", "Img2", "ok", "image"),
                        mk_media("vid", "Vid", "ok", "video") };
    ms[3].enabled = 0;
    app_config cfg = mkcfg(ms, 4);
    router rt = { &cfg, q };
    router_reply rep;
    chat_msg m1[] = { { "user", "Нарисуй кота в шляпе" } };
    router_chat(&rt, m1, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.kind == KIND_IMAGE && rep.model == &ms[2] && rep.switched);
    CHECK(rep.prompt_model == &ms[0] && rep.prompt && strstr(rep.prompt, "Нарисуй кота"));
    CHECK(rep.attempts[0].kind == RES_QUOTA && rep.nattempts == 2); /* попытки текстовой модели в каскад картинок не попадают */
    CHECK(!strcmp(rep.media_mime, "image/png") && file_exists("test_media/u0", rep.media_file)); /* папка пользователя */
    char *js = router_reply_json(&rep);
    char url[96];
    snprintf(url, sizeof url, "\"url\":\"/api/media/%s\"", rep.media_file);
    CHECK(js && strstr(js, "\"kind\":\"image\"") && strstr(js, "\"badge\":\"Нарисовал: Img2\"") && strstr(js, url) &&
          strstr(js, "\"prompt_model\":\"Txt\"") && strstr(js, "\"mime\":\"image/png\""));
    free(js);
    router_reply_free(&rep);

    /* английский запрос без контекста идёт в модель как есть */
    chat_msg m2[] = { { "user", "draw a red fox" } };
    router_chat(&rt, m2, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.model == &ms[2] && !rep.prompt_model && !strcmp(rep.prompt, "draw a red fox"));
    router_reply_free(&rep);

    /* закреплённая «рисовалка»: картинка даже без слова «нарисуй» */
    chat_msg m3[] = { { "user", "кот в космосе" } };
    router_chat(&rt, m3, 1, "img2", NULL, &rep);
    CHECK(rep.ok && rep.kind == KIND_IMAGE && rep.model == &ms[2]);
    router_reply_free(&rep);

    /* закреплённая текстовая модель в чате «Изображения» — обычный текстовый ответ */
    router_chat(&rt, m3, 1, "txt", "images", &rep);
    CHECK(rep.ok && rep.kind == KIND_CHAT && rep.model == &ms[0] && rep.text);
    router_reply_free(&rep);

    /* видео: единственная модель выключена */
    chat_msg m4[] = { { "user", "Сгенерируй видео с котом" } };
    router_chat(&rt, m4, 1, "auto", NULL, &rep);
    CHECK(!rep.ok && rep.kind == KIND_VIDEO && strstr(rep.error, "для видео"));
    router_reply_free(&rep);
    ms[3].enabled = 1;
    router_chat(&rt, m4, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.kind == KIND_VIDEO && !strcmp(rep.media_mime, "video/mp4") && strstr(rep.media_file, ".mp4"));
    router_reply_free(&rep);
    quota_reset(q, 0, "img1");
}

static void test_audio(quota_db *q) {
    /* Cloudflare-озвучка не говорит по-русски: молча пропускается, озвучивает следующая модель */
    model_cfg ms[3] = { mk("txt", "Txt", "ok"), mk_media("cf-tts", "CF TTS", "ok", "audio"), mk_media("g-tts", "Gemini TTS", "ok", "audio") };
    xstrlcpy(ms[1].provider, "cloudflare", sizeof ms[1].provider);
    xstrlcpy(ms[1].key_name, "CF", sizeof ms[1].key_name);
    store_key_set(q, 0, "CF", "acc:tok");
    app_config cfg = mkcfg(ms, 3);
    router rt = { &cfg, q };
    router_reply rep;
    chat_msg m1[] = { { "user", "Озвучь: «Добрый вечер, мир!»" } };
    router_chat(&rt, m1, 1, "auto", NULL, &rep);
    CHECK(rep.ok && rep.kind == KIND_AUDIO && rep.model == &ms[2] && rep.nattempts == 1 && !rep.switched);
    CHECK(!rep.prompt_model && rep.prompt && !strcmp(rep.prompt, "Добрый вечер, мир!")); /* текст после двоеточия, без модели */
    CHECK(!strcmp(rep.media_mime, "audio/wav"));
    char *js = router_reply_json(&rep);
    CHECK(js && strstr(js, "\"badge\":\"Озвучил: Gemini TTS\"") && strstr(js, "\"prompt_model\":null"));
    free(js);
    router_reply_free(&rep);

    /* «озвучь предыдущий ответ» — текст готовит текстовая модель из истории */
    chat_msg m2[] = { { "user", "Расскажи анекдот" }, { "assistant", "Колобок повесился." }, { "user", "озвучь предыдущий ответ" } };
    router_chat(&rt, m2, 3, "auto", NULL, &rep);
    CHECK(rep.ok && rep.kind == KIND_AUDIO && rep.prompt_model == &ms[0]);
    router_reply_free(&rep);
    store_key_set(q, 0, "CF", NULL);
}

/* ---------------- пользователи и сессии ---------------- */

static void test_auth(quota_db *q) {
    char norm[64], err[200];
    CHECK(auth_check_login("ab", norm, sizeof norm, err, sizeof err) != 0);
    CHECK(auth_check_login("имя", norm, sizeof norm, err, sizeof err) != 0);
    CHECK(auth_check_login("a b c", norm, sizeof norm, err, sizeof err) != 0);
    CHECK(auth_check_login("User.Name@Mail", norm, sizeof norm, err, sizeof err) == 0 && !strcmp(norm, "user.name@mail"));
    CHECK(auth_check_password("short", "x", err, sizeof err) != 0);
    CHECK(auth_check_password("alice-alice", "alice-alice", err, sizeof err) != 0);

    auth_user a, b;
    CHECK(auth_register(q, "Alice", "secret-pass-1", 180, &a, err, sizeof err) == 0 && a.id > U1.id && !strcmp(a.login, "alice"));
    CHECK(auth_register(q, "ALICE", "another-pass-2", 0, &b, err, sizeof err) != 0 && strstr(err, "занят"));
    CHECK(auth_register(q, "weakling", "123", 0, &b, err, sizeof err) != 0);

    /* пароль в БД — только хеш */
    sqlite3_stmt *st;
    char stored[200] = "";
    quota_lock(q);
    if (sqlite3_prepare_v2(quota_sqlite(q), "SELECT pass FROM users WHERE login='alice'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) xstrlcpy(stored, (const char *)sqlite3_column_text(st, 0), sizeof stored);
        sqlite3_finalize(st);
    }
    quota_unlock(q);
    CHECK(!strncmp(stored, "pbkdf2_sha256$", 14) && !strstr(stored, "secret-pass-1"));

    long retry = 0;
    CHECK(auth_login(q, "alice", "secret-pass-1", 180, &b, &retry, err, sizeof err) == 0 && b.id == a.id && b.tz_min == 180);
    CHECK(auth_login(q, "alice", "wrong-pass-1", 180, &b, &retry, err, sizeof err) == -1);
    CHECK(auth_login(q, "nobody", "whatever-123", 180, &b, &retry, err, sizeof err) == -1 && strstr(err, "неверный логин или пароль"));

    /* подбор пароля: после 5 ошибок — блокировка, даже с верным паролем */
    auth_user bob;
    CHECK(auth_register(q, "bob", "bob-password-1", 0, &bob, err, sizeof err) == 0);
    for (int i = 0; i < 5; i++) CHECK(auth_login(q, "bob", "guess-guess", 0, &b, &retry, err, sizeof err) == -1);
    CHECK(auth_login(q, "bob", "bob-password-1", 0, &b, &retry, err, sizeof err) == -2 && retry > 0 && retry <= 60);

    /* сессии */
    char tok[AUTH_TOKEN_LEN + 1];
    CHECK(auth_session_create(q, a.id, tok) == 0 && strlen(tok) == 64);
    auth_user who;
    CHECK(auth_session_user(q, tok, &who) == 1 && who.id == a.id && !strcmp(who.login, "alice"));
    CHECK(auth_session_user(q, "0000000000000000000000000000000000000000000000000000000000000000", &who) == 0);
    char cookie[200], parsed[AUTH_TOKEN_LEN + 1];
    snprintf(cookie, sizeof cookie, "theme=dark; iai_session=%s; other=1", tok);
    CHECK(auth_cookie_token(cookie, parsed) && !strcmp(parsed, tok));
    CHECK(!auth_cookie_token("iai_session=abc", parsed));
    CHECK(!auth_cookie_token("iai_session=zz00000000000000000000000000000000000000000000000000000000000000", parsed));
    auth_session_delete(q, tok);
    CHECK(auth_session_user(q, tok, &who) == 0);
}

static void test_auth_api(quota_db *q) {
    model_cfg ms[1] = { mk("m", "M", "ok") };
    app_config cfg = mkcfg(ms, 1);
    cfg.allow_registration = 1;
    router rt = { &cfg, q, 0 };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp, sc[320], cookie[200];

    CHECK(api(&ctx, "POST", "/api/auth/register", "{\"login\":\"carol\",\"password\":\"carol-pass-1\",\"tz\":180}", NULL, &resp, sc) == 200);
    CHECK(strstr(resp, "\"login\":\"carol\"") && strstr(sc, "iai_session=") && strstr(sc, "HttpOnly") && strstr(sc, "SameSite=Lax"));
    free(resp);
    snprintf(cookie, sizeof cookie, "%.*s", (int)strcspn(sc, ";"), sc);
    CHECK(api(&ctx, "GET", "/api/auth/me?tz=-300", "", cookie, &resp, NULL) == 200 && strstr(resp, "\"tz\":-300"));
    free(resp);
    CHECK(api(&ctx, "GET", "/api/auth/me", "", NULL, &resp, NULL) == 401 && strstr(resp, "\"registration\":true"));
    free(resp);
    CHECK(api(&ctx, "POST", "/api/auth/login", "{\"login\":\"carol\",\"password\":\"nope-nope\"}", NULL, &resp, sc) == 401 && !sc[0]);
    free(resp);
    CHECK(api(&ctx, "POST", "/api/auth/login", "{\"login\":\"carol\",\"password\":\"carol-pass-1\"}", NULL, &resp, sc) == 200 && strstr(sc, "iai_session="));
    free(resp);
    /* выход: cookie стирается, старая сессия больше не действует */
    CHECK(api(&ctx, "POST", "/api/auth/logout", "", cookie, &resp, sc) == 200 && strstr(sc, "Max-Age=0"));
    free(resp);
    CHECK(api(&ctx, "GET", "/api/auth/me", "", cookie, &resp, NULL) == 401);
    free(resp);
    /* регистрацию можно закрыть */
    cfg.allow_registration = 0;
    CHECK(api(&ctx, "POST", "/api/auth/register", "{\"login\":\"dave\",\"password\":\"dave-pass-1\"}", NULL, &resp, NULL) == 403);
    free(resp);
}

static void test_isolation(quota_db *q) {
    /* у каждого пользователя свои ключи, настройки и лимиты; ключи из .env — только владельцу */
    model_cfg ms[2] = { mk("env-model", "EnvModel", "ok"), mk("lim", "Lim", "ok") };
    xstrlcpy(ms[0].provider, "openai", sizeof ms[0].provider);
    xstrlcpy(ms[0].model, "x", sizeof ms[0].model);
    xstrlcpy(ms[0].key_name, "EnvProv", sizeof ms[0].key_name);
    xstrlcpy(ms[0].api_key_env, "IAI_TEST_ENV_KEY", sizeof ms[0].api_key_env);
    ms[1].limits.rpd = 3;
    plat_setenv_if_unset("IAI_TEST_ENV_KEY", "env-secret-1");
    app_config cfg = mkcfg(ms, 2);
    char err[200];
    auth_user other;
    CHECK(auth_register(q, "erin", "erin-pass-1", 0, &other, err, sizeof err) == 0);
    router owner_rt = { &cfg, q, U1.id }, other_rt = { &cfg, q, other.id };
    char kb[600];
    const char *src;
    CHECK(quota_owner_uid(q) == U1.id);
    CHECK(router_api_key(&owner_rt, &ms[0], kb, sizeof kb, &src) && !strcmp(src, "env"));
    CHECK(!router_api_key(&other_rt, &ms[0], kb, sizeof kb, &src));
    xstrlcpy(cfg.env_keys, "all", sizeof cfg.env_keys);
    CHECK(router_api_key(&other_rt, &ms[0], kb, sizeof kb, &src) && !strcmp(src, "env"));
    xstrlcpy(cfg.env_keys, "none", sizeof cfg.env_keys);
    CHECK(!router_api_key(&owner_rt, &ms[0], kb, sizeof kb, &src));
    xstrlcpy(cfg.env_keys, "owner", sizeof cfg.env_keys);

    store_key_set(q, other.id, "EnvProv", "erin-own-key");
    CHECK(router_api_key(&other_rt, &ms[0], kb, sizeof kb, &src) && !strcmp(kb, "erin-own-key") && !strcmp(src, "settings"));
    const char *k = router_api_key(&owner_rt, &ms[0], kb, sizeof kb, &src);
    CHECK(k && !strcmp(k, "env-secret-1")); /* владелец её ключ не видит */

    /* лимиты: расход одного не трогает другого */
    for (int i = 0; i < 3; i++) quota_record(q, other.id, &ms[1], plat_now(), 5);
    char reason[200];
    CHECK(quota_check(q, other.id, &ms[1], plat_now(), 100, reason, sizeof reason) == QUOTA_LOCAL_LIMIT);
    CHECK(quota_check(q, U1.id, &ms[1], plat_now(), 100, reason, sizeof reason) == QUOTA_OK);
    /* шифрование — тоже личная настройка */
    CHECK(quota_set_encryption(q, other.id, 1, 1) == 1);
    char raw[4096];
    CHECK(store_key_get_raw(q, other.id, "EnvProv", raw, sizeof raw) && crypto_is_encrypted(raw));
    CHECK(!quota_encryption_on(q, U1.id));
    quota_set_encryption(q, other.id, 0, 1);
    store_key_set(q, other.id, "EnvProv", NULL);
}

static void test_legacy_migration(void) {
    /* база прошлой версии: общие ключи и настройки → достаются первому зарегистрированному */
    const char *db = "test_legacy.db";
    remove(db);
    char err[200];
    quota_db *q2 = quota_open(db, err, sizeof err);
    CHECK(q2 != NULL);
    if (!q2) return;
    sqlite3_exec(quota_sqlite(q2),
                 "CREATE TABLE api_keys(provider TEXT PRIMARY KEY, api_key TEXT NOT NULL, updated_ts INTEGER NOT NULL);"
                 "INSERT INTO api_keys VALUES('Groq','gsk_legacy_1',1);",
                 NULL, NULL, NULL);
    quota_kv_set(q2, "router_settings", "{\"threshold\":70,\"strategy\":\"quality\"}");
    auth_user first, second;
    CHECK(auth_register(q2, "owner", "owner-pass-1", 0, &first, err, sizeof err) == 0);
    CHECK(auth_register(q2, "guest", "guest-pass-1", 0, &second, err, sizeof err) == 0);
    char kb[200];
    CHECK(store_key_get(q2, first.id, "Groq", kb, sizeof kb) && !strcmp(kb, "gsk_legacy_1"));
    CHECK(!store_key_get(q2, second.id, "Groq", kb, sizeof kb));
    router_settings s;
    settings_get(q2, first.id, &s);
    CHECK(s.threshold == 70 && s.strategy == STRATEGY_QUALITY);
    settings_get(q2, second.id, &s);
    CHECK(s.threshold == 90);
    CHECK(quota_owner_uid(q2) == first.id);
    quota_close(q2);
    remove(db);
    remove("test_legacy.db-wal");
    remove("test_legacy.db-shm");
}

/* ---------------- статистика ---------------- */

static void test_stats(quota_db *q) {
    char d[16];
    stats_day(0, 180, d);
    CHECK(!strcmp(d, "1970-01-01"));
    stats_day(86400 - 3600, 180, d); /* 23:00 UTC = 02:00 следующего дня по Москве */
    CHECK(!strcmp(d, "1970-01-02"));
    stats_day(3600, -300, d);        /* 01:00 UTC = 20:00 предыдущего дня в Нью-Йорке */
    CHECK(!strcmp(d, "1969-12-31"));

    model_cfg ms[2] = { mk("st-a", "StatA", "ok"), mk("st-img", "StatImg", "ok") };
    ms[0].limits.rpd = 50;
    ms[0].limits.tpd = 100000;
    xstrlcpy(ms[1].kind, "image", sizeof ms[1].kind);
    app_config cfg = mkcfg(ms, 2);
    char err[200], tok[AUTH_TOKEN_LEN + 1], cookie[128];
    auth_user fay;
    CHECK(auth_register(q, "fay", "fay-pass-12", 180, &fay, err, sizeof err) == 0);
    auth_session_create(q, fay.id, tok);
    snprintf(cookie, sizeof cookie, "iai_session=%s", tok);
    router rt = { &cfg, q, 0 };
    server_ctx ctx = { &cfg, &rt, q };
    char *resp;

    CHECK(api(&ctx, "GET", "/api/auth/me", "", cookie, &resp, NULL) == 200); /* открыл сайт — посещение */
    free(resp);
    CHECK(api(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"user\",\"content\":\"Расскажи о погоде\"}]}", cookie, &resp, NULL) == 200);
    free(resp);
    CHECK(api(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"user\",\"content\":\"Нарисуй дом\"}]}", cookie, &resp, NULL) == 200);
    char url[96] = "";
    const char *u = strstr(resp, "/api/media/");
    if (u) snprintf(url, sizeof url, "%.*s", (int)strcspn(u, "\""), u);
    free(resp);
    char dir[300];
    router_media_dir(&cfg, fay.id, dir, sizeof dir);
    CHECK(url[0] && file_exists(dir, url + strlen("/api/media/"))); /* файл — в личной папке */

    long long used = stats_tokens_today(q, fay.id);
    CHECK(used > 0);
    CHECK(api(&ctx, "GET", "/api/stats", "", cookie, &resp, NULL) == 200);
    json *j = json_parse(resp, NULL, 0);
    char today[16];
    stats_day(plat_now(), 180, today);
    CHECK(j && !strcmp(json_str(json_get(j, "today"), ""), today) && json_num(json_get(j, "tz"), 0) == 180);
    const json *days = json_get(j, "days");
    CHECK(json_len(days) == 1 && json_bool(json_path(days, "0.visited"), 0) && json_num(json_path(days, "0.requests"), 0) == 2);
    CHECK(json_num(json_path(days, "0.media"), 0) == 1);
    CHECK(json_num(json_get(j, "used_today"), 0) == used && json_num(json_get(j, "daily_limit"), 0) == 1000000);
    /* текстовая модель потратила 2 запроса: ответ в чате и перевод промпта для картинки */
    CHECK(json_num(json_path(j, "left.requests"), -1) == 50 - 2 && json_num(json_path(j, "left.tokens"), -1) > 99000);
    CHECK(!strcmp(json_str(json_path(j, "models.0.id"), ""), "st-a"));
    json_free(j);
    free(resp);
    /* у другого пользователя своя статистика */
    CHECK(API("GET", "/api/stats", "", &resp) == 200 && !strstr(resp, "\"requests\":2"));
    free(resp);

    /* личный дневной лимит токенов */
    char lim[96];
    snprintf(lim, sizeof lim, "{\"dailyTokenLimit\":%lld}", used);
    CHECK(api(&ctx, "POST", "/api/settings", lim, cookie, &resp, NULL) == 200 && strstr(resp, "\"dailyTokenLimit\":"));
    free(resp);
    CHECK(api(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"user\",\"content\":\"Ещё\"}]}", cookie, &resp, NULL) == 503);
    CHECK(strstr(resp, "Дневной лимит токенов исчерпан"));
    free(resp);
    CHECK(api(&ctx, "POST", "/api/settings", "{\"dailyTokenLimit\":-5}", cookie, &resp, NULL) == 400);
    free(resp);
    CHECK(api(&ctx, "POST", "/api/settings", "{\"dailyTokenLimit\":0}", cookie, &resp, NULL) == 200); /* 0 — без лимита */
    free(resp);
    CHECK(api(&ctx, "POST", "/api/chat", "{\"messages\":[{\"role\":\"user\",\"content\":\"Ещё\"}]}", cookie, &resp, NULL) == 200);
    free(resp);
}

int main(void) {
    log_set_level(LOG_ERROR); /* в тестах не шумим */
    const char *db = "test_quota.db";
    remove(db);
    remove("test_quota.db-wal");
    remove("test_quota.db-shm");
    char err[256];
    quota_db *q = quota_open(db, err, sizeof err);
    if (!q) { fprintf(stderr, "quota_open: %s\n", err); return 1; }
    remove("test_master.key");
    if (crypto_init("test_master.key", err, sizeof err) != 0) { fprintf(stderr, "crypto: %s\n", err); return 1; }
    auth_set_iterations(2000); /* в тестах PBKDF2 полегче */
    char tok[AUTH_TOKEN_LEN + 1];
    if (auth_register(q, "tester", "tester-pass-1", 180, &U1, err, sizeof err) != 0 || auth_session_create(q, U1.id, tok) != 0) {
        fprintf(stderr, "test user: %s\n", err);
        return 1;
    }
    snprintf(COOKIE1, sizeof COOKIE1, "iai_session=%s", tok);

    test_json();
    test_classify();
    test_cooldown();
    test_cascade(q);
    test_local_limit(q);
    test_errors_and_order(q);
    test_history(q);
    test_api(q);
    test_keys(q);
    test_settings_api(q);
    test_threshold(q);
    test_autoswitch_off(q);
    test_strategies(q);
    test_keep_context(q);
    test_disabled(q);
    test_catalog_auto(q);
    test_cloudflare_key();
    test_encryption(q);
    test_detect_kind();
    test_media_utils();
    test_media_cascade(q);
    test_audio(q);
    test_auth(q);
    test_auth_api(q);
    test_isolation(q);
    test_legacy_migration();
    test_stats(q);

    quota_close(q);
    crypto_shutdown();
    remove("test_master.key");
    remove(db);
#ifdef _WIN32
    if (system("rmdir /s /q test_media 2>nul")) {}
#else
    if (system("rm -rf test_media")) {}
#endif
    remove("test_quota.db-wal");
    remove("test_quota.db-shm");
    printf("%s: %d проверок пройдено, %d провалено\n", g_fail ? "FAIL" : "OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
