#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings.h"
#include "stats.h"
#include "platform.h"
#include "strbuf.h"

#define MAX_COOLDOWN (2L * 86400)

long router_cooldown_seconds(const call_result *res, int64_t now) {
    long s = 0;
    switch (res->kind) {
        case RES_RATE_LIMIT:
            s = res->retry_after_s > 0 ? res->retry_after_s : 60;
            break;
        case RES_QUOTA:
            if (res->retry_after_s > 0) s = res->retry_after_s;          /* провайдер сам сказал, сколько ждать */
            else if (res->limit_daily) s = (long)(quota_next_day(now) - now); /* суточный лимит — до конца суток UTC */
            else if (res->limit_billing) s = 3600;                     /* кончились деньги — проверяем раз в час */
            else s = 60;                                               /* похоже на минутный лимит */
            break;
        case RES_AUTH: s = 600; break;                                 /* неверный ключ — не долбим провайдера */
        case RES_SERVER: s = res->retry_after_s > 0 ? res->retry_after_s : 30; break;
        case RES_NETWORK: s = 15; break;
        case RES_BAD_REQUEST: s = res->http_status == 404 ? 6 * 3600 : 0; break; /* нет такой модели — не дёргаем 6 ч */
        default: s = 0;                                                /* bad_response — без паузы */
    }
    if (s > MAX_COOLDOWN) s = MAX_COOLDOWN;
    return s;
}

/* Ключи из .env — общие для сервера; чужим пользователям их не отдаём. */
static int router_env_allowed(router *r) {
    if (!strcmp(r->cfg->env_keys, "all") || r->uid == 0) return 1; /* uid 0 — без авторизации (тесты, CLI) */
    if (!strcmp(r->cfg->env_keys, "none")) return 0;
    return r->quota && r->uid == quota_owner_uid(r->quota);   /* "owner" — по умолчанию */
}

const char *router_api_key(router *r, const model_cfg *m, char *buf, size_t len, const char **source) {
    const char *src = NULL, *key = NULL;
    if (!strcmp(m->provider, "mock")) { src = "mock"; key = "mock"; }
    else if (m->keyless) { src = "none"; key = ""; }
    else if (r->quota && store_key_get(r->quota, r->uid, m->key_name, buf, len)) { src = "settings"; key = buf; }
    else if (router_env_allowed(r) && (key = config_api_key(m)) != NULL) src = "env";
    if (source) *source = src;
    return key;
}

static void new_request_id(char *out, size_t n) {
    static unsigned counter = 0;
    unsigned c = __sync_fetch_and_add(&counter, 1);
    snprintf(out, n, "%llx%04x", (unsigned long long)plat_now_ms(), c & 0xFFFF);
}

/* Подготовка истории: последние max_history сообщений, первое — от пользователя, без пустых,
 * подряд идущие сообщения одной роли склеиваются (этого требуют Anthropic и Gemini). */
typedef struct {
    chat_msg *msgs;
    char    **owned;
    size_t    n, nowned;
} norm_msgs;

static int normalize(const chat_msg *in, size_t n, int max_history, norm_msgs *out) {
    memset(out, 0, sizeof *out);
    size_t start = n > (size_t)max_history ? n - (size_t)max_history : 0;
    out->msgs = calloc(n - start + 1, sizeof *out->msgs);
    out->owned = calloc(n - start + 1, sizeof *out->owned);
    if (!out->msgs || !out->owned) return -1;
    for (size_t i = start; i < n; i++) {
        const char *role = strcmp(in[i].role, "assistant") ? "user" : "assistant";
        if (!in[i].content || !*in[i].content) continue;
        if (out->n == 0 && !strcmp(role, "assistant")) continue; /* начинаем с пользователя */
        if (out->n && !strcmp(out->msgs[out->n - 1].role, role)) {
            strbuf sb;
            sb_init(&sb);
            sb_append(&sb, out->msgs[out->n - 1].content);
            sb_append(&sb, "\n\n");
            sb_append(&sb, in[i].content);
            if (sb.oom) { sb_free(&sb); return -1; }
            char *merged = sb_take(&sb);
            out->owned[out->nowned++] = merged;
            out->msgs[out->n - 1].content = merged;
            continue;
        }
        out->msgs[out->n].role = role;
        out->msgs[out->n].content = in[i].content;
        out->n++;
    }
    return 0;
}

static void norm_free(norm_msgs *nm) {
    for (size_t i = 0; i < nm->nowned; i++) free(nm->owned[i]);
    free(nm->owned);
    free(nm->msgs);
}

/* Кандидат каскада и ключи сортировки. */
typedef struct {
    size_t idx;
    int    tier;   /* 0 free, 1 trial, 2 paid */
    int    skill;  /* 0 — навык указан явно, 1 — универсальная модель, 2 — навык не подходит */
    int    prio;   /* качество: 1 — сильнейшие */
    int    load;   /* загруженность по локальным лимитам, 0–100 (для «Свободнее») */
} cand;

static int tier_rank(const char *t) { return !strcmp(t, "paid") ? 2 : !strcmp(t, "trial") ? 1 : 0; }

static int cand_cmp(const cand *a, const cand *b, strategy_t st) {
#define CMP(x) do { if (a->x != b->x) return a->x < b->x ? -1 : 1; } while (0)
    switch (st) {
        case STRATEGY_QUALITY: CMP(skill); CMP(prio); CMP(tier); break;           /* сильнейшие первыми, даже платные */
        case STRATEGY_FREE:    CMP(tier); CMP(skill); CMP(load); CMP(prio); break; /* бесплатные с наибольшим запасом */
        default:               CMP(tier); CMP(skill); break;                      /* дешевле: бесплатные, затем пробные, платные */
    }
    CMP(idx); /* порядок config.json, затем каталог */
#undef CMP
    return 0;
}

/* Порядок каскада по настройкам. Модели каталога без ключа и выключенные на сайте не участвуют.
 * Закреплённая в чате модель всегда идёт первой. Возвращает число кандидатов в order. */
static size_t build_order(router *r, const router_settings *st, const char *prefer, const char *skill, media_kind kind,
                          size_t *order) {
    const app_config *cfg = r->cfg;
    cand *c = malloc(cfg->nmodels * sizeof *c);
    if (!c) return 0;
    size_t n = 0, k = 0;
    int pinned = -1;
    if (prefer && *prefer && strcmp(prefer, "auto"))
        for (size_t i = 0; i < cfg->nmodels; i++)
            if (!strcmp(cfg->models[i].id, prefer) && cfg->models[i].enabled && !settings_is_disabled(st, cfg->models[i].id) &&
                model_kind(&cfg->models[i]) == kind) { pinned = (int)i; order[k++] = i; break; }
    int64_t now = plat_now();
    for (size_t i = 0; i < cfg->nmodels; i++) {
        const model_cfg *m = &cfg->models[i];
        if ((int)i == pinned || !m->enabled || settings_is_disabled(st, m->id) || model_kind(m) != kind) continue;
        if (m->auto_added) {
            char kb[600];
            if (!router_api_key(r, m, kb, sizeof kb, NULL)) continue; /* модель каталога без ключа — не в каскаде */
        }
        cand x = { i, tier_rank(m->tier), !skill || !*skill ? 0 : m->nskills == 0 ? 1 : model_has_skill(m, skill) ? 0 : 2,
                   m->priority, 0 };
        if (st->strategy == STRATEGY_FREE) {
            int l = quota_load_pct(r->quota, r->uid, m, now);
            x.load = l < 0 ? 50 : l; /* лимит неизвестен — считаем загруженной наполовину */
        }
        /* вставка с сохранением порядка (кандидатов не больше нескольких сотен) */
        size_t j = n++;
        while (j > 0 && cand_cmp(&x, &c[j - 1], st->strategy) < 0) { c[j] = c[j - 1]; j--; }
        c[j] = x;
    }
    for (size_t i = 0; i < n; i++) order[k++] = c[i].idx;
    free(c);
    return k;
}

static void add_attempt(router_reply *out, const model_cfg *m, result_kind kind, long status, long latency, const char *msg) {
    if (out->nattempts >= ROUTER_MAX_ATTEMPTS) return;
    attempt_info *a = &out->attempts[out->nattempts++];
    a->model = m;
    a->kind = kind;
    a->http_status = status;
    a->latency_ms = latency;
    xstrlcpy(a->message, msg ? msg : "", sizeof a->message);
}

/* Один проход каскада по моделям нужного типа. Добавляет попытки в out; при успехе заполняет ответ.
 * req_last — тот же запрос без истории (для «Сохранять контекст» = выкл). */
static void run_cascade(router *r, const router_settings *st, const chat_request *req, const chat_request *req_last,
                        const char *prefer, const char *skill, media_kind kind, long timeout_ms, router_reply *out) {
    const app_config *cfg = r->cfg;
    size_t *order = malloc((cfg->nmodels + 1) * sizeof *order);
    if (!order) { xstrlcpy(out->error, "нет памяти", sizeof out->error); return; }
    int threshold = st->auto_switch ? st->threshold : 100;
    size_t norder = build_order(r, st, prefer, skill, kind, order);
    const char *text = request_prompt(req);
    int tried_real = 0;  /* сколько моделей действительно вызывали (для флага switched) */
    int considered = 0;  /* моделей с ключом, до которых дошла очередь (для «без автопереключения») */

    for (size_t oi = 0; oi < norder; oi++) {
        const model_cfg *m = &cfg->models[order[oi]];
        int64_t now = plat_now();
        char reason[200];

        /* озвучка только на английском (Cloudflare MeloTTS/Aura) русский текст не прочитает — молча пропускаем */
        if (kind == KIND_AUDIO && !strcmp(m->provider, "cloudflare") && has_cyrillic(text)) continue;

        /* --- предварительные проверки: есть ключ, не исчерпан локальный лимит --- */
        if (!st->auto_switch && considered > 0) {
            LOGI("router", "[%s] автопереключение выключено в «Настройках» — резервные ИИ не используются", out->request_id);
            break;
        }
        char keybuf[600];
        const char *key = router_api_key(r, m, keybuf, sizeof keybuf, NULL);
        if (!key) {
            snprintf(reason, sizeof reason, "нет API-ключа «%s» (Настройки → API-ключи)", m->key_name);
            LOGI("router", "[%s] %s: пропуск — %s", out->request_id, m->label, reason);
            add_attempt(out, m, RES_NOT_CONFIGURED, 0, 0, reason);
            continue;
        }
        considered++;
        quota_state qs = quota_check(r->quota, r->uid, m, now, threshold, reason, sizeof reason);
        if (qs != QUOTA_OK) {
            result_kind k = qs == QUOTA_COOLDOWN ? RES_COOLDOWN : RES_LOCAL_LIMIT;
            LOGW("router", "[%s] %s: Основной лимит исчерпан (%s). %s", out->request_id, m->label, reason,
                 !st->auto_switch ? "Автопереключение выключено." : oi + 1 < norder ? "Переключаюсь на резервный ИИ..." : "Резервных ИИ в каскаде больше нет.");
            add_attempt(out, m, k, 0, 0, reason);
            quota_log_attempt(r->quota, r->uid, now, out->request_id, m->id, result_kind_code(k), 0, reason);
            continue;
        }

        /* --- «try»: вызов провайдера --- */
        provider_call_fn call = provider_for(m);
        call_result res;
        int64_t c0 = plat_now_ms();
        const chat_request *use = (tried_real > 0 && !st->keep_context) ? req_last : req;
        LOGI("router", "[%s] → %s (%s/%s)%s", out->request_id, m->label, m->provider, m->model,
             use == req_last && req_last != req ? " — без истории чата (контекст не передаётся)" : "");
        call(m, key, use, timeout_ms, &res);
        long latency = (long)(plat_now_ms() - c0);
        tried_real++;

        if (res.kind == RES_OK && kind != KIND_CHAT && (!res.media || !res.media_len)) {
            res.kind = RES_BAD_RESPONSE;
            xstrlcpy(res.error, "модель не вернула файл", sizeof res.error);
        }
        if (res.kind == RES_OK) {
            quota_record(r->quota, r->uid, m, plat_now(), res.tokens_in + res.tokens_out);
            quota_log_attempt(r->quota, r->uid, plat_now(), out->request_id, m->id, "ok", res.http_status, NULL);
            add_attempt(out, m, RES_OK, res.http_status, latency, "");
            out->ok = 1;
            out->model = m;
            out->switched = out->nattempts > 1;
            out->tokens_in = res.tokens_in;
            out->tokens_out = res.tokens_out;
            if (kind == KIND_CHAT) {
                out->text = res.text; /* передаём владение */
                res.text = NULL;
            } else {
                out->media = res.media;
                out->media_len = res.media_len;
                xstrlcpy(out->media_mime, res.media_mime, sizeof out->media_mime);
                res.media = NULL;
            }
            call_result_free(&res);
            LOGI("router", "[%s] ✓ %s: %s за %ld мс%s", out->request_id,
                 kind == KIND_IMAGE ? "Нарисовал" : kind == KIND_AUDIO ? "Озвучил" : kind == KIND_VIDEO ? "Снял видео" : "Ответил",
                 m->label, latency, out->switched ? " — после переключения" : "");
            break;
        }

        /* --- «catch»: разбираем ошибку и решаем, куда идти дальше --- */
        long cd = router_cooldown_seconds(&res, plat_now());
        const char *next = !st->auto_switch ? "Автопереключение выключено."
                         : oi + 1 < norder ? "Переключаюсь на резервный ИИ..." : "Резервных ИИ в каскаде больше нет.";
        if (result_is_limit(res.kind))
            LOGW("router", "[%s] %s: Основной лимит исчерпан (%s). %s", out->request_id, m->label, res.error, next);
        else
            LOGW("router", "[%s] %s: ошибка %s (%s). %s", out->request_id, m->label, result_kind_code(res.kind), res.error, next);
        if (cd > 0) {
            char why[160];
            snprintf(why, sizeof why, "%s: %.120s", result_kind_code(res.kind), res.error);
            quota_set_cooldown(r->quota, r->uid, m->id, plat_now() + cd, why);
            LOGI("router", "[%s] %s на паузе %ld с", out->request_id, m->label, cd);
        }
        quota_log_attempt(r->quota, r->uid, plat_now(), out->request_id, m->id, result_kind_code(res.kind), res.http_status, res.error);
        add_attempt(out, m, res.kind, res.http_status, latency, res.error);
        call_result_free(&res);
    }

    if (!out->ok) {
        static const char *what[] = { "", " для изображений", " для озвучки", " для видео" };
        if (!st->auto_switch && considered > 0)
            xstrlcpy(out->error, "Модель недоступна, а автопереключение выключено в «Настройках»", sizeof out->error);
        else if (tried_real == 0 && norder == 0 && kind != KIND_CHAT)
            snprintf(out->error, sizeof out->error, "Нет моделей%s: включите их на странице «Модели» или добавьте ключ провайдера", what[kind]);
        else if (tried_real == 0)
            snprintf(out->error, sizeof out->error, "Нет доступных моделей%s: не заданы API-ключи или исчерпаны все лимиты", what[kind]);
        else
            snprintf(out->error, sizeof out->error, "Все ИИ%s в каскаде недоступны или исчерпали лимиты", what[kind]);
    }
    free(order);
}

/* ------------------------------------------------------------ что хочет пользователь ---- */

/* Нижний регистр для ASCII и кириллицы; длина в байтах не меняется (важно для смещений). */
static void lower_utf8(const char *in, char *out, size_t cap) {
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p && i + 2 < cap; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') { out[i++] = (char)(c + 32); continue; }
        if (c == 0xD0 && p[1] >= 0x90 && p[1] <= 0x9F) { out[i++] = (char)0xD0; out[i++] = (char)(p[1] + 0x20); p++; continue; } /* А–П */
        if (c == 0xD0 && p[1] >= 0xA0 && p[1] <= 0xAF) { out[i++] = (char)0xD1; out[i++] = (char)(p[1] - 0x20); p++; continue; } /* Р–Я */
        if (c == 0xD0 && p[1] == 0x81) { out[i++] = (char)0xD1; out[i++] = (char)0x91; p++; continue; }                       /* Ё */
        out[i++] = (char)c;
    }
    out[i] = '\0';
}

/* Глагол из списка, а за ним (в пределах пары слов) — существительное из списка. */
static int verb_noun(const char *s, const char *const *verbs, const char *const *nouns) {
    for (int v = 0; verbs[v]; v++) {
        const char *p = s;
        while ((p = strstr(p, verbs[v])) != NULL) {
            p += strlen(verbs[v]);
            char win[96];
            xstrlcpy(win, p, sizeof win);
            for (int n = 0; nouns[n]; n++)
                if (strstr(win, nouns[n])) return 1;
        }
    }
    return 0;
}

static int has_any(const char *s, const char *const *words) {
    for (int i = 0; words[i]; i++)
        if (strstr(s, words[i])) return 1;
    return 0;
}

media_kind router_detect_kind(const char *text, const char *skill) {
    static const char *const make[] = { "сгенерируй", "создай", "сделай", "нарисуй", "изобрази", "сними", "смонтируй", "оживи",
                                        "generate", "create", "make", "draw", "render", NULL };
    static const char *const video_n[] = { "видео", "ролик", "клип", "анимаци", "мультик", "video", "clip", "animation", NULL };
    static const char *const audio_direct[] = { "озвучь", "озвучить", "прочитай вслух", "прочти вслух", "зачитай", "произнеси",
                                                "скажи вслух", "read aloud", "read this aloud", "text to speech", "text-to-speech", NULL };
    static const char *const audio_v[] = { "сгенерируй", "создай", "сделай", "запиши", "generate", "create", "make", NULL };
    static const char *const audio_n[] = { "аудио", "озвучк", "голосом", "голосовое", "mp3", "audio", "voiceover", "voice over", "speech", NULL };
    static const char *const image_direct[] = { "нарисуй", "изобрази", "draw ", "draw me", NULL };
    static const char *const image_n[] = { "картин", "изображени", "фото", "рисун", "логотип", "иллюстрац", "постер", "обложк",
                                           "аватар", "иконк", "баннер", "обои", "эскиз", "арт ", "image", "picture", "photo",
                                           "illustration", "logo", "poster", "wallpaper", NULL };
    char low[1200];
    lower_utf8(text ? text : "", low, sizeof low);
    if (verb_noun(low, make, video_n)) return KIND_VIDEO;
    if (has_any(low, audio_direct) || verb_noun(low, audio_v, audio_n)) return KIND_AUDIO;
    if (has_any(low, image_direct) || verb_noun(low, make, image_n)) return KIND_IMAGE;
    /* в чате «Изображения» рисуем по умолчанию; в «Аудио» и «Монтаже видео» чаще спрашивают о работе со звуком
     * и видео, поэтому там озвучка и генерация видео — только по явной просьбе или с закреплённой моделью */
    if (skill && !strcmp(skill, "images")) return KIND_IMAGE;
    return KIND_CHAT;
}

/* Какой тип ответа нужен: закреплённая медиа-модель → явная просьба в тексте → закреплённая текстовая
 * модель → специализация чата → текст. */
static media_kind decide_kind(router *r, const char *prefer, const char *skill, const char *text) {
    const model_cfg *pinned = NULL;
    if (prefer && *prefer && strcmp(prefer, "auto"))
        for (size_t i = 0; i < r->cfg->nmodels; i++)
            if (!strcmp(r->cfg->models[i].id, prefer)) { pinned = &r->cfg->models[i]; break; }
    if (pinned && model_kind(pinned) != KIND_CHAT) return model_kind(pinned);
    media_kind byText = router_detect_kind(text, NULL);
    if (byText != KIND_CHAT) return byText;
    if (pinned) return KIND_CHAT;
    return router_detect_kind("", skill);
}

/* Убрать пробелы и кавычки по краям; сжать переводы строк. На месте. */
static void tidy(char *s) {
    for (char *p = s; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    char *a = s;
    for (;;) {
        if (*a == ' ' || *a == '"' || *a == '\'' || *a == '`') a++;
        else if (!strncmp(a, "«", 2) || !strncmp(a, "»", 2)) a += 2;
        else break;
    }
    memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    for (;;) {
        if (n && (s[n - 1] == ' ' || s[n - 1] == '"' || s[n - 1] == '\'' || s[n - 1] == '`')) n--;
        else if (n >= 2 && (!strncmp(s + n - 2, "«", 2) || !strncmp(s + n - 2, "»", 2))) n -= 2;
        else break;
    }
    s[n] = '\0';
}

/* Текст для озвучки без помощи модели: «озвучь: текст» → «текст». NULL — нужен разбор моделью
 * (например, «озвучь предыдущий ответ» или «расскажи голосом сказку»). */
static char *speech_text_direct(const char *msg, int explicit_trigger) {
    const char *colon = strchr(msg, ':');
    if (colon && colon - msg < 80 && strlen(colon + 1) >= 3) {
        char *t = xstrdup(colon + 1);
        if (t) tidy(t);
        if (t && *t) return t;
        free(t);
        return NULL;
    }
    if (!explicit_trigger) return xstrdup(msg); /* чат «Аудио» без команды — читаем сообщение как есть */
    return NULL;
}

/* Системные промпты для подготовки запроса к медиа-модели. */
static const char *const PROMPT_SYS[] = {
    NULL,
    "You write prompts for an image generation model. Using the conversation for context (the user may refer to "
    "earlier images, listed as [image: ...]), output ONE English prompt of at most 80 words describing exactly the image "
    "the user wants now: subject, setting, style, lighting, composition. Output only the prompt — no quotes, no comments.",
    "You prepare text for a text-to-speech engine. If the user gives text to read, output it verbatim. If the user asks "
    "to voice a previous answer, output that answer. If the user asks to narrate something new (a story, a poem, a "
    "greeting), write it in the user's language, at most 150 words. Output only the text to be spoken, without markdown.",
    "You write prompts for a text-to-video model that makes 8-second clips. Using the conversation for context, output ONE "
    "English prompt of at most 80 words: subject, action, setting, camera movement, style, sound. Output only the prompt.",
};

/* Подготовить запрос к медиа-модели. Текстовая модель (обычный каскад) переводит и уточняет промпт.
 * Возвращает malloc-строку; prompt_model — кто её написал (или NULL). */
static char *media_prompt(router *r, const router_settings *st, const norm_msgs *nm, media_kind kind, const char *request_id,
                          const model_cfg **prompt_model) {
    *prompt_model = NULL;
    const char *last = nm->msgs[nm->n - 1].content;
    char low[1200];
    lower_utf8(last, low, sizeof low);
    if (kind == KIND_AUDIO) {
        static const char *const triggers[] = { "озвуч", "вслух", "зачитай", "произнеси", "голос", "read aloud", "speech", "audio", "аудио", NULL };
        char *t = speech_text_direct(last, has_any(low, triggers));
        if (t) return t;
    } else if (!has_cyrillic(last) && nm->n == 1 && strlen(last) < 600) {
        return xstrdup(last); /* уже английский и без контекста — отдаём как есть */
    }

    /* просим текстовую модель: последние сообщения чата + системная инструкция */
    size_t from = nm->n > 6 ? nm->n - 6 : 0;
    chat_request req = { nm->msgs + from, nm->n - from, PROMPT_SYS[kind] };
    router_reply *tmp = calloc(1, sizeof *tmp);
    char *result = NULL;
    if (tmp) {
        xstrlcpy(tmp->request_id, request_id, sizeof tmp->request_id);
        run_cascade(r, st, &req, &req, NULL, NULL, KIND_CHAT, r->cfg->timeout_ms, tmp);
        if (tmp->ok && tmp->text) {
            tidy(tmp->text);
            if (strlen(tmp->text) >= 3) {
                result = tmp->text;
                tmp->text = NULL;
                *prompt_model = tmp->model;
                stats_record(r->quota, r->uid, 0, tmp->tokens_in, tmp->tokens_out, 0, 0); /* токены на промпт — тоже расход */
                LOGI("router", "[%s] Промпт для %s подготовил %s: %.200s", request_id, kind_code(kind), tmp->model->label, result);
            }
        } else {
            LOGW("router", "[%s] не удалось подготовить промпт текстовой моделью (%s) — отправляю запрос как есть", request_id, tmp->error);
        }
        router_reply_free(tmp);
        free(tmp);
    }
    if (!result) {
        result = xstrdup(last);
        if (result) tidy(result);
    }
    if (result && strlen(result) > 3800) result[3800] = '\0'; /* лимиты моделей на длину промпта */
    return result;
}

void router_media_dir(const app_config *cfg, int64_t uid, char *out, size_t n) {
    snprintf(out, n, "%s/u%lld", cfg->media_dir, (long long)uid);
}

static const char *mime_ext(const char *mime) {
    static const struct { const char *mime, *ext; } t[] = {
        { "image/png", "png" }, { "image/jpeg", "jpg" }, { "image/webp", "webp" }, { "image/gif", "gif" },
        { "audio/mpeg", "mp3" }, { "audio/wav", "wav" }, { "audio/ogg", "ogg" }, { "video/mp4", "mp4" }, { "video/webm", "webm" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strncmp(mime, t[i].mime, strlen(t[i].mime))) return t[i].ext;
    return NULL;
}

/* Сохранить файл в media_dir. 0 — успех, имя файла в out->media_file. */
static int save_media(const app_config *cfg, int64_t uid, router_reply *out) {
    const char *ext = mime_ext(out->media_mime);
    if (!ext) {
        const char *s = sniff_mime(out->media, out->media_len);
        if (s) { xstrlcpy(out->media_mime, s, sizeof out->media_mime); ext = mime_ext(s); }
    }
    if (!ext) { snprintf(out->error, sizeof out->error, "неизвестный формат файла (%s)", out->media_mime); return -1; }
    char dir[400];
    router_media_dir(cfg, uid, dir, sizeof dir); /* у каждого пользователя своя папка — чужие файлы не отдаются */
    if (plat_mkdirs(dir) != 0) { snprintf(out->error, sizeof out->error, "не удалось создать папку %.150s", dir); return -1; }
    snprintf(out->media_file, sizeof out->media_file, "%s.%s", out->request_id, ext);
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, out->media_file);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(out->media, 1, out->media_len, f) == out->media_len;
    if (f && fclose(f) != 0) ok = 0;
    if (!ok) { snprintf(out->error, sizeof out->error, "не удалось сохранить файл в %.150s", cfg->media_dir); out->media_file[0] = '\0'; return -1; }
    return 0;
}

void router_chat(router *r, const chat_msg *msgs, size_t n, const char *prefer, const char *skill, router_reply *out) {
    memset(out, 0, sizeof *out);
    new_request_id(out->request_id, sizeof out->request_id);
    int64_t t0 = plat_now_ms();
    const app_config *cfg = r->cfg;

    norm_msgs nm;
    if (normalize(msgs, n, cfg->max_history, &nm) != 0 || nm.n == 0) {
        norm_free(&nm);
        xstrlcpy(out->error, "пустой запрос: нет сообщений пользователя", sizeof out->error);
        return;
    }
    router_settings *st = malloc(sizeof *st);
    if (!st) {
        norm_free(&nm);
        xstrlcpy(out->error, "нет памяти", sizeof out->error);
        return;
    }
    settings_get(r->quota, r->uid, st);
    media_kind kind = decide_kind(r, prefer, skill, nm.msgs[nm.n - 1].content);
    out->kind = kind;
    long long used_today = st->daily_tokens > 0 ? stats_tokens_today(r->quota, r->uid) : 0;

    if (st->daily_tokens > 0 && used_today >= st->daily_tokens) {
        snprintf(out->error, sizeof out->error,
                 "Дневной лимит токенов исчерпан: израсходовано %lld из %lld. Поднять или снять лимит можно в «Настройках»",
                 used_today, st->daily_tokens);
    } else if (kind == KIND_CHAT) {
        chat_request req = { nm.msgs, nm.n, cfg->system_prompt };
        /* без передачи контекста резервная модель получает только последнее сообщение пользователя */
        chat_request req_last = { nm.msgs + nm.n - 1, 1, cfg->system_prompt };
        run_cascade(r, st, &req, &req_last, prefer, skill, KIND_CHAT, cfg->timeout_ms, out);
    } else {
        LOGI("router", "[%s] Запрос на %s", out->request_id,
             kind == KIND_IMAGE ? "изображение" : kind == KIND_AUDIO ? "озвучку" : "видео");
        out->prompt = media_prompt(r, st, &nm, kind, out->request_id, &out->prompt_model);
        if (!out->prompt) {
            xstrlcpy(out->error, "нет памяти", sizeof out->error);
        } else {
            chat_msg one = { "user", out->prompt };
            chat_request req = { &one, 1, NULL };
            long timeout = kind == KIND_VIDEO ? 600000 : 180000; /* генерация дольше текста; видео — до 10 минут */
            if (cfg->timeout_ms > timeout) timeout = cfg->timeout_ms;
            run_cascade(r, st, &req, &req, prefer, skill, kind, timeout, out);
            if (out->ok && save_media(cfg, r->uid, out) != 0) out->ok = 0;
        }
    }
    if (!out->ok) LOGE("router", "[%s] %s", out->request_id, out->error);
    else stats_record(r->quota, r->uid, 1, out->tokens_in, out->tokens_out, kind != KIND_CHAT, out->switched);
    out->latency_ms = (long)(plat_now_ms() - t0);
    free(st);
    norm_free(&nm);
}

void router_reply_free(router_reply *out) {
    free(out->text);
    out->text = NULL;
    free(out->media);
    out->media = NULL;
    out->media_len = 0;
    free(out->prompt);
    out->prompt = NULL;
}

static void model_json(strbuf *b, const model_cfg *m) {
    sb_append(b, "{\"id\":");
    sb_json_str(b, m->id);
    sb_append(b, ",\"label\":");
    sb_json_str(b, m->label);
    sb_append(b, ",\"provider\":");
    sb_json_str(b, m->provider);
    sb_append(b, ",\"model\":");
    sb_json_str(b, m->model);
    sb_append(b, ",\"tier\":");
    sb_json_str(b, m->tier);
    sb_append(b, "}");
}

char *router_reply_json(const router_reply *out) {
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":%s,\"request_id\":", out->ok ? "true" : "false");
    sb_json_str(&b, out->request_id);
    sb_append(&b, ",\"kind\":");
    sb_json_str(&b, kind_code(out->kind));
    if (out->prompt) {
        sb_append(&b, ",\"prompt\":");
        sb_json_str(&b, out->prompt);
        sb_append(&b, ",\"prompt_model\":");
        if (out->prompt_model) sb_json_str(&b, out->prompt_model->label);
        else sb_append(&b, "null");
    }
    if (out->ok) {
        char badge[96];
        static const char *verb[] = { "Ответил", "Нарисовал", "Озвучил", "Снял видео" };
        snprintf(badge, sizeof badge, "%s: %s", verb[out->kind], out->model->label);
        sb_append(&b, ",\"text\":");
        sb_json_str(&b, out->text ? out->text : "");
        if (out->kind != KIND_CHAT) {
            sb_append(&b, ",\"media\":{\"url\":");
            char url[128];
            snprintf(url, sizeof url, "/api/media/%s", out->media_file);
            sb_json_str(&b, url);
            sb_append(&b, ",\"mime\":");
            sb_json_str(&b, out->media_mime);
            sb_printf(&b, ",\"bytes\":%zu}", out->media_len);
        }
        sb_append(&b, ",\"badge\":");
        sb_json_str(&b, badge);
        sb_append(&b, ",\"model\":");
        model_json(&b, out->model);
        sb_printf(&b, ",\"switched\":%s,\"usage\":{\"input_tokens\":%ld,\"output_tokens\":%ld}", out->switched ? "true" : "false",
                  out->tokens_in, out->tokens_out);
    } else {
        sb_append(&b, ",\"error\":");
        sb_json_str(&b, out->error);
    }
    sb_append(&b, ",\"attempts\":[");
    for (int i = 0; i < out->nattempts; i++) {
        const attempt_info *a = &out->attempts[i];
        if (i) sb_appendc(&b, ',');
        sb_append(&b, "{\"model\":");
        model_json(&b, a->model);
        sb_append(&b, ",\"status\":");
        sb_json_str(&b, result_kind_code(a->kind));
        sb_printf(&b, ",\"limit\":%s,\"http_status\":%ld,\"latency_ms\":%ld,\"message\":", result_is_limit(a->kind) ? "true" : "false",
                  a->http_status, a->latency_ms);
        sb_json_str(&b, a->message);
        sb_append(&b, "}");
    }
    sb_printf(&b, "],\"latency_ms\":%ld}", out->latency_ms);
    return b.oom ? (sb_free(&b), NULL) : sb_take(&b);
}
