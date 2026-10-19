#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "json.h"
#include "log.h"

#define DEFAULT_DAILY_TOKENS 1000000LL
#define MAX_DAILY_TOKENS     10000000000LL

void settings_defaults(router_settings *s) {
    memset(s, 0, sizeof *s);
    s->auto_switch = 1;
    s->threshold = 90;
    s->strategy = STRATEGY_FREE;
    s->keep_context = 1;
    s->notify_switch = 1;
    s->encrypt_keys = 0;
    s->daily_tokens = DEFAULT_DAILY_TOKENS;
}

const char *strategy_code(strategy_t s) {
    switch (s) {
        case STRATEGY_CHEAP: return "cheap";
        case STRATEGY_QUALITY: return "quality";
        default: return "free";
    }
}

static int parse_strategy(const char *v, strategy_t *out) {
    if (!strcmp(v, "cheap")) *out = STRATEGY_CHEAP;
    else if (!strcmp(v, "free")) *out = STRATEGY_FREE;
    else if (!strcmp(v, "quality")) *out = STRATEGY_QUALITY;
    else return -1;
    return 0;
}

int settings_is_disabled(const router_settings *s, const char *model_id) {
    for (int i = 0; i < s->ndisabled; i++)
        if (!strcmp(s->disabled[i], model_id)) return 1;
    return 0;
}

/* Разбор JSON поверх dst. Неизвестные поля игнорируются. */
static int parse_into(router_settings *dst, const json *j, char *err, size_t errlen) {
    if (!j || j->type != JSON_OBJECT) { snprintf(err, errlen, "нужен JSON-объект настроек"); return -1; }
    const json *v;
    if ((v = json_get(j, "autoSwitch"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "autoSwitch: true/false"); return -1; }
        dst->auto_switch = v->boolean;
    }
    if ((v = json_get(j, "threshold"))) {
        double t = json_num(v, -1);
        if (t < 50 || t > 100) { snprintf(err, errlen, "threshold: число от 50 до 100"); return -1; }
        dst->threshold = (int)t;
    }
    if ((v = json_get(j, "strategy"))) {
        if (parse_strategy(json_str(v, ""), &dst->strategy) != 0) { snprintf(err, errlen, "strategy: cheap | free | quality"); return -1; }
    }
    if ((v = json_get(j, "keepContext"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "keepContext: true/false"); return -1; }
        dst->keep_context = v->boolean;
    }
    if ((v = json_get(j, "notifySwitch"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "notifySwitch: true/false"); return -1; }
        dst->notify_switch = v->boolean;
    }
    if ((v = json_get(j, "encryptKeys"))) {
        if (v->type != JSON_BOOL) { snprintf(err, errlen, "encryptKeys: true/false"); return -1; }
        dst->encrypt_keys = v->boolean;
    }
    if ((v = json_get(j, "dailyTokenLimit"))) {
        double t = json_num(v, -1);
        if (v->type != JSON_NUMBER || t < 0 || t > (double)MAX_DAILY_TOKENS) {
            snprintf(err, errlen, "dailyTokenLimit: число токенов от 0 (без лимита) до 10 млрд");
            return -1;
        }
        dst->daily_tokens = (long long)t;
    }
    if ((v = json_get(j, "disabled"))) {
        if (v->type != JSON_ARRAY) { snprintf(err, errlen, "disabled: массив id моделей"); return -1; }
        dst->ndisabled = 0;
        for (const json *c = v->child; c && dst->ndisabled < CFG_MAX_MODELS; c = c->next) {
            const char *id = json_str(c, NULL);
            if (id && *id && strlen(id) < 64) xstrlcpy(dst->disabled[dst->ndisabled++], id, 64);
        }
    }
    return 0;
}

static void to_json(const router_settings *s, strbuf *b) {
    sb_printf(b, "{\"autoSwitch\":%s,\"threshold\":%d,\"strategy\":\"%s\",\"keepContext\":%s,\"notifySwitch\":%s,\"encryptKeys\":%s,"
                 "\"dailyTokenLimit\":%lld,\"disabled\":[",
              s->auto_switch ? "true" : "false", s->threshold, strategy_code(s->strategy), s->keep_context ? "true" : "false",
              s->notify_switch ? "true" : "false", s->encrypt_keys ? "true" : "false", s->daily_tokens);
    for (int i = 0; i < s->ndisabled; i++) {
        if (i) sb_appendc(b, ',');
        sb_json_str(b, s->disabled[i]);
    }
    sb_append(b, "]}");
}

static void kv_name(int64_t uid, char *out, size_t n) { snprintf(out, n, "router_settings:%lld", (long long)uid); }

void settings_get(quota_db *q, int64_t uid, router_settings *s) {
    settings_defaults(s);
    char name[48];
    kv_name(uid, name, sizeof name);
    char *saved = quota_kv_get(q, name);
    if (saved) {
        json *j = json_parse(saved, NULL, 0);
        char err[128];
        if (j && parse_into(s, j, err, sizeof err) != 0) settings_defaults(s);
        json_free(j);
        free(saved);
    }
    s->encrypt_keys = quota_encryption_on(q, uid); /* фактическое состояние ключей */
}

int settings_apply_json(quota_db *q, int64_t uid, const char *json_text, char *err, size_t errlen) {
    char perr[128];
    json *j = json_parse(json_text ? json_text : "", perr, sizeof perr);
    if (!j) { snprintf(err, errlen, "ошибка JSON: %s", perr); return -1; }
    router_settings *next = malloc(sizeof *next);
    if (!next) { json_free(j); snprintf(err, errlen, "нет памяти"); return -1; }
    settings_get(q, uid, next);
    int was_encrypted = next->encrypt_keys;
    int rc = parse_into(next, j, err, errlen);
    json_free(j);
    if (rc == 0 && next->encrypt_keys && !crypto_available()) {
        snprintf(err, errlen, "шифрование недоступно: не удалось создать мастер-ключ (см. лог сервера)");
        rc = -1;
    }
    if (rc == 0 && next->encrypt_keys != was_encrypted) {
        int n = quota_set_encryption(q, uid, next->encrypt_keys, 1);
        if (n < 0) { snprintf(err, errlen, "не удалось перешифровать ключи"); rc = -1; }
        else LOGI("settings", "пользователь %lld: шифрование ключей %s, обработано ключей: %d", (long long)uid,
                  next->encrypt_keys ? "включено" : "выключено", n);
    }
    if (rc == 0) {
        strbuf b;
        sb_init(&b);
        to_json(next, &b);
        char name[48];
        kv_name(uid, name, sizeof name);
        quota_kv_set(q, name, b.data);
        sb_free(&b);
    }
    free(next);
    return rc;
}

void settings_to_json(quota_db *q, int64_t uid, strbuf *b) {
    router_settings *s = malloc(sizeof *s);
    if (!s) { sb_append(b, "{}"); return; }
    settings_get(q, uid, s);
    to_json(s, b);
    free(s);
}
