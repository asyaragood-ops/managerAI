#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "platform.h"
#include "strbuf.h"

static char *read_file(const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    strbuf sb;
    sb_init(&sb);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sb_append_n(&sb, buf, n);
        if (sb.len > max) { sb_free(&sb); fclose(f); return NULL; }
    }
    fclose(f);
    return sb_take(&sb);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

void config_load_env(const char *path) {
    char *text = read_file(path, 1 << 20);
    if (!text) return;
    char *save = text;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        if (!strncmp(l, "export ", 7)) l = trim(l + 7);
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(l), *v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
            v[vl - 1] = '\0';
            v++;
        }
        if (*k) plat_setenv_if_unset(k, v);
    }
    free(save);
}

static void copy_str(char *dst, size_t size, const json *obj, const char *key, const char *def) {
    xstrlcpy(dst, json_str(json_get(obj, key), def), size);
}

/* Имя провайдера по умолчанию — такое же, как в «Настройках» фронтенда. */
static void default_key_name(model_cfg *m) {
    if (m->key_name[0]) return;
    static const struct { const char *needle, *name; } hosts[] = {
        { "groq.com", "Groq" }, { "openrouter.ai", "OpenRouter" }, { "mistral.ai", "Mistral" },
        { "cerebras.ai", "Cerebras" }, { "nvidia.com", "NVIDIA NIM" }, { "github.ai", "GitHub Models" },
        { "inference.ai.azure.com", "GitHub Models" }, { "sambanova.ai", "SambaNova" }, { "scaleway", "Scaleway" },
        { "cohere", "Cohere" }, { "hyperbolic.xyz", "Hyperbolic" }, { "deepseek.com", "DeepSeek" }, { "x.ai", "xAI" },
        { "cloudflare.com", "Cloudflare Workers AI" }, { "kilo.ai", "Kilo Gateway" },
    };
    const char *name = "OpenAI";
    if (!strcmp(m->provider, "gemini")) name = "Google AI Studio";
    else if (!strcmp(m->provider, "cloudflare")) name = "Cloudflare Workers AI";
    else if (!strcmp(m->provider, "pollinations")) name = "Pollinations";
    else if (!strcmp(m->provider, "anthropic")) name = "Anthropic";
    else if (!strcmp(m->provider, "mock")) name = "mock";
    else
        for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++)
            if (strstr(m->base_url, hosts[i].needle)) { name = hosts[i].name; break; }
    xstrlcpy(m->key_name, name, sizeof m->key_name);
}

static int parse_model(const json *o, model_cfg *m, char *err, size_t errlen, size_t idx) {
    memset(m, 0, sizeof *m);
    copy_str(m->id, sizeof m->id, o, "id", "");
    copy_str(m->label, sizeof m->label, o, "label", "");
    copy_str(m->provider, sizeof m->provider, o, "provider", "");
    copy_str(m->model, sizeof m->model, o, "model", "");
    copy_str(m->base_url, sizeof m->base_url, o, "base_url", "");
    copy_str(m->api_key_env, sizeof m->api_key_env, o, "api_key_env", "");
    copy_str(m->key_name, sizeof m->key_name, o, "key_name", "");
    copy_str(m->tier, sizeof m->tier, o, "tier", "free");
    copy_str(m->max_tokens_field, sizeof m->max_tokens_field, o, "max_tokens_field", "max_tokens");
    copy_str(m->mock_behavior, sizeof m->mock_behavior, o, "mock_behavior", "ok");
    copy_str(m->kind, sizeof m->kind, o, "kind", !strcmp(m->provider, "pollinations") ? "image" : "chat");
    m->keyless = json_bool(json_get(o, "keyless"), !strcmp(m->provider, "pollinations"));
    m->max_tokens = (int)json_num(json_get(o, "max_tokens"), 2048);
    m->enabled = json_bool(json_get(o, "enabled"), 1);
    m->priority = (int)json_num(json_get(o, "priority"), 2);
    const json *lim = json_get(o, "limits");
    m->limits.rpm = (int)json_num(json_get(lim, "rpm"), 0);
    m->limits.rpd = (int)json_num(json_get(lim, "rpd"), 0);
    m->limits.rpmo = (int)json_num(json_get(lim, "rpmo"), 0);
    m->limits.tpd = (long)json_num(json_get(lim, "tpd"), 0);
    const json *sk = json_get(o, "skills");
    for (size_t i = 0; i < json_len(sk) && m->nskills < CFG_MAX_SKILLS; i++) {
        const char *s = json_str(json_at(sk, i), NULL);
        if (s) xstrlcpy(m->skills[m->nskills++], s, sizeof m->skills[0]);
    }

    if (!*m->id || !*m->provider || (!*m->model && strcmp(m->provider, "mock"))) {
        snprintf(err, errlen, "cascade[%zu]: обязательны поля id, provider и model", idx);
        return -1;
    }
    if (strcmp(m->provider, "gemini") && strcmp(m->provider, "openai") && strcmp(m->provider, "anthropic") &&
        strcmp(m->provider, "cloudflare") && strcmp(m->provider, "pollinations") && strcmp(m->provider, "mock")) {
        snprintf(err, errlen, "cascade[%zu] (%s): неизвестный provider \"%s\" (gemini|openai|anthropic|cloudflare|pollinations|mock)",
                 idx, m->id, m->provider);
        return -1;
    }
    if (kind_parse(m->kind) < 0) {
        snprintf(err, errlen, "cascade[%zu] (%s): неизвестный kind \"%s\" (chat|image|audio|video)", idx, m->id, m->kind);
        return -1;
    }
    /* что умеет каждый адаптер */
    media_kind k = model_kind(m);
    int ok = !strcmp(m->provider, "mock") || (!strcmp(m->provider, "gemini")) ||
             (k == KIND_CHAT && (!strcmp(m->provider, "openai") || !strcmp(m->provider, "anthropic"))) ||
             (!strcmp(m->provider, "cloudflare") && (k == KIND_IMAGE || k == KIND_AUDIO)) ||
             (!strcmp(m->provider, "pollinations") && k == KIND_IMAGE);
    if (!ok) {
        snprintf(err, errlen, "cascade[%zu] (%s): provider \"%s\" не умеет kind \"%s\"", idx, m->id, m->provider, m->kind);
        return -1;
    }
    default_key_name(m);
    if (!*m->label) xstrlcpy(m->label, m->model[0] ? m->model : m->id, sizeof m->label);
    if (m->max_tokens <= 0) m->max_tokens = 2048;
    return 0;
}

int config_load(const char *path, app_config *cfg, char *err, size_t errlen) {
    memset(cfg, 0, sizeof *cfg);
    char *text = read_file(path, 4 << 20);
    if (!text) { snprintf(err, errlen, "не удалось прочитать %s", path); return -1; }
    char perr[160];
    json *root = json_parse(text, perr, sizeof perr);
    free(text);
    if (!root) { snprintf(err, errlen, "%s: ошибка JSON: %s", path, perr); return -1; }

    copy_str(cfg->host, sizeof cfg->host, root, "host", "127.0.0.1");
    cfg->port = (int)json_num(json_get(root, "port"), 8080);
    copy_str(cfg->db_path, sizeof cfg->db_path, root, "db_path", "data/infinity.db");
    cfg->timeout_ms = (long)json_num(json_get(root, "timeout_ms"), 60000);
    cfg->max_history = (int)json_num(json_get(root, "max_history"), 20);
    copy_str(cfg->cors_origin, sizeof cfg->cors_origin, root, "cors_origin", "http://localhost:4200");
    copy_str(cfg->system_prompt, sizeof cfg->system_prompt, root, "system_prompt", "");

    copy_str(cfg->catalog_path, sizeof cfg->catalog_path, root, "catalog_path", "catalog.json");
    copy_str(cfg->media_dir, sizeof cfg->media_dir, root, "media_dir", "");
    copy_str(cfg->env_keys, sizeof cfg->env_keys, root, "env_keys", "owner");
    if (strcmp(cfg->env_keys, "owner") && strcmp(cfg->env_keys, "all") && strcmp(cfg->env_keys, "none")) {
        snprintf(err, errlen, "%s: env_keys — owner | all | none", path);
        json_free(root);
        return -1;
    }
    cfg->allow_registration = json_bool(json_get(root, "allow_registration"), 1);
    cfg->cookie_secure = json_bool(json_get(root, "cookie_secure"), 0);
    if (!cfg->media_dir[0]) { /* по умолчанию — папка media рядом с базой */
        const char *sl = strrchr(cfg->db_path, '/'), *bs = strrchr(cfg->db_path, '\\');
        if (bs > sl) sl = bs;
        if (sl) snprintf(cfg->media_dir, sizeof cfg->media_dir, "%.*s/media", (int)(sl - cfg->db_path), cfg->db_path);
        else xstrlcpy(cfg->media_dir, "media", sizeof cfg->media_dir);
    }
    const json *cascade = json_get(root, "cascade");
    size_t n = json_len(cascade);
    if (cascade && cascade->type != JSON_ARRAY) {
        snprintf(err, errlen, "%s: \"cascade\" должен быть массивом", path);
        json_free(root);
        return -1;
    }
    if (n > CFG_MAX_MODELS) {
        snprintf(err, errlen, "%s: в каскаде больше %d моделей", path, CFG_MAX_MODELS);
        json_free(root);
        return -1;
    }
    cfg->models = calloc(CFG_MAX_MODELS, sizeof *cfg->models);
    if (!cfg->models) { snprintf(err, errlen, "нет памяти"); json_free(root); return -1; }
    for (size_t i = 0; i < n; i++) {
        if (parse_model(json_at(cascade, i), &cfg->models[i], err, errlen, i) != 0) {
            json_free(root);
            config_free(cfg);
            return -1;
        }
        for (size_t k = 0; k < i; k++)
            if (!strcmp(cfg->models[k].id, cfg->models[i].id)) {
                snprintf(err, errlen, "cascade: повторяющийся id \"%s\"", cfg->models[i].id);
                json_free(root);
                config_free(cfg);
                return -1;
            }
        cfg->nmodels++;
    }
    cfg->nexplicit = cfg->nmodels;
    json_free(root);
    if (cfg->port <= 0 || cfg->port > 65535) { snprintf(err, errlen, "некорректный port"); config_free(cfg); return -1; }
    if (cfg->max_history < 1) cfg->max_history = 1;

    /* каталог: путь относительно папки config.json */
    if (cfg->catalog_path[0]) {
        char full[512];
        const char *slash = strrchr(path, '/');
        const char *bslash = strrchr(path, '\\');
        if (bslash > slash) slash = bslash;
        int absolute = cfg->catalog_path[0] == '/' || cfg->catalog_path[0] == '\\' || (cfg->catalog_path[0] && cfg->catalog_path[1] == ':');
        if (slash && !absolute) snprintf(full, sizeof full, "%.*s/%s", (int)(slash - path), path, cfg->catalog_path);
        else xstrlcpy(full, cfg->catalog_path, sizeof full);
        int added = config_load_catalog(cfg, full, err, errlen);
        if (added < 0) { config_free(cfg); return -1; }
    }
    if (cfg->nmodels == 0) {
        snprintf(err, errlen, "%s: нет ни одной модели — заполните \"cascade\" или положите рядом catalog.json", path);
        config_free(cfg);
        return -1;
    }
    return 0;
}

int config_load_catalog(app_config *cfg, const char *path, char *err, size_t errlen) {
    char *text = read_file(path, 8 << 20);
    if (!text) return 0; /* каталога нет — работаем только с явным каскадом */
    char perr[160];
    json *root = json_parse(text, perr, sizeof perr);
    free(text);
    if (!root) { snprintf(err, errlen, "%s: ошибка JSON: %s", path, perr); return -1; }
    const json *arr = json_get(root, "models");
    int added = 0;
    for (size_t i = 0; i < json_len(arr) && cfg->nmodels < CFG_MAX_MODELS; i++) {
        model_cfg m;
        char merr[200];
        if (parse_model(json_at(arr, i), &m, merr, sizeof merr, i) != 0) continue; /* битую запись пропускаем */
        int dup = 0;
        for (size_t k = 0; k < cfg->nmodels; k++)
            if (!strcmp(cfg->models[k].id, m.id)) { dup = 1; break; } /* явная запись в config.json главнее */
        if (dup) continue;
        m.auto_added = 1;
        cfg->models[cfg->nmodels++] = m;
        added++;
    }
    json_free(root);
    return added;
}

void config_free(app_config *cfg) {
    free(cfg->models);
    cfg->models = NULL;
    cfg->nmodels = 0;
}

const char *config_api_key(const model_cfg *m) {
    if (!strcmp(m->provider, "mock")) return "mock";
    if (!m->api_key_env[0]) return NULL;
    const char *k = getenv(m->api_key_env);
    return (k && *k) ? k : NULL;
}

int model_has_skill(const model_cfg *m, const char *skill) {
    if (!skill || !*skill) return 1;
    if (m->nskills == 0) return 1; /* модель без списка навыков считается универсальной */
    for (int i = 0; i < m->nskills; i++)
        if (!strcmp(m->skills[i], skill)) return 1;
    return 0;
}

media_kind model_kind(const model_cfg *m) {
    int k = kind_parse(m->kind);
    return k < 0 ? KIND_CHAT : (media_kind)k;
}

const char *kind_code(media_kind k) {
    switch (k) {
        case KIND_IMAGE: return "image";
        case KIND_AUDIO: return "audio";
        case KIND_VIDEO: return "video";
        default: return "chat";
    }
}

int kind_parse(const char *s) {
    if (!s || !*s || !strcmp(s, "chat")) return KIND_CHAT;
    if (!strcmp(s, "image")) return KIND_IMAGE;
    if (!strcmp(s, "audio")) return KIND_AUDIO;
    if (!strcmp(s, "video")) return KIND_VIDEO;
    return -1;
}
