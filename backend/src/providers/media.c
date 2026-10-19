/* Генерация картинок, речи и видео.
 *
 *  cloudflare    — Workers AI: POST /accounts/{account}/ai/run/{model}; ответ — JSON с base64 или сам файл
 *                  (FLUX.1 schnell, SDXL, DreamShaper — картинки; MeloTTS, Deepgram Aura — речь)
 *  pollinations  — GET image.pollinations.ai/prompt/{текст}: картинка без ключа (≈1 запрос в 15 с)
 *  gemini image  — generateContent с responseModalities IMAGE (Gemini Image)
 *  gemini tts    — generateContent с responseModalities AUDIO: PCM 24 кГц → WAV
 *  gemini veo    — predictLongRunning → опрос операции → скачивание MP4
 *
 * Результат — байты файла в call_result.media и его MIME-тип; роутер сохраняет файл и отдаёт ссылку. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_client.h"
#include "../json.h"
#include "../platform.h"
#include "../strbuf.h"
#include "provider.h"

#define GEMINI_BASE "https://generativelanguage.googleapis.com/v1beta"
#define CF_BASE     "https://api.cloudflare.com/client/v4/accounts/{account}/ai/run"
#define POLL_BASE   "https://image.pollinations.ai/prompt"

#define MAX_IMAGE (40u << 20)
#define MAX_AUDIO (40u << 20)
#define MAX_VIDEO (200u << 20)

/* Забрать тело ответа как файл. */
static void take_media(call_result *out, http_response *r, const char *fallback_mime) {
    out->media_len = r->body.len;
    out->media = (unsigned char *)sb_take(&r->body);
    const char *mime = sniff_mime(out->media, out->media_len);
    xstrlcpy(out->media_mime, mime ? mime : fallback_mime, sizeof out->media_mime);
    out->kind = RES_OK;
    out->http_status = 200;
}

/* Декодировать base64 из JSON в call_result. 0 — успех. */
static int take_b64(call_result *out, const char *b64, const char *fallback_mime) {
    size_t n = 0;
    unsigned char *d = b64 ? b64_decode(b64, strlen(b64), &n) : NULL;
    if (!d || n == 0) {
        free(d);
        out->kind = RES_BAD_RESPONSE;
        xstrlcpy(out->error, "в ответе нет данных файла", sizeof out->error);
        return -1;
    }
    out->media = d;
    out->media_len = n;
    const char *mime = sniff_mime(d, n);
    xstrlcpy(out->media_mime, mime ? mime : fallback_mime, sizeof out->media_mime);
    out->kind = RES_OK;
    out->http_status = 200;
    return 0;
}

static int starts_with(const char *s, const char *p) { return s && !strncmp(s, p, strlen(p)); }

static void urlencode(strbuf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            sb_appendc(b, (char)*p);
        } else {
            sb_appendc(b, '%');
            sb_appendc(b, hex[*p >> 4]);
            sb_appendc(b, hex[*p & 15]);
        }
    }
}

/* ------------------------------------------------------------ Cloudflare Workers AI ---- */

void provider_cloudflare(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    media_kind kind = model_kind(m);
    const char *prompt = request_prompt(req);
    int is_aura = strstr(m->model, "aura") != NULL;
    if (kind == KIND_AUDIO && has_cyrillic(prompt)) {
        /* MeloTTS и Aura не говорят по-русски — пусть озвучит следующая модель (Gemini TTS) */
        out->kind = RES_BAD_REQUEST;
        snprintf(out->error, sizeof out->error, "%s не озвучивает русский текст", m->label);
        return;
    }
    char account[80];
    const char *token = NULL;
    if (split_account_key(m, api_key, account, sizeof account, &token, out) != 0) return;

    const char *base = m->base_url[0] ? m->base_url : CF_BASE;
    const char *ph = strstr(base, "{account}");
    char url[700];
    if (ph)
        snprintf(url, sizeof url, "%.*s%s%s/%s", (int)(ph - base), base, account, ph + strlen("{account}"), m->model);
    else
        snprintf(url, sizeof url, "%s/%s", base, m->model);
    char auth[640];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *headers[] = { auth };

    strbuf b;
    sb_init(&b);
    if (kind == KIND_AUDIO && is_aura) {
        sb_append(&b, "{\"text\":");
        sb_json_str(&b, prompt);
    } else if (kind == KIND_AUDIO) {
        sb_append(&b, "{\"lang\":\"en\",\"prompt\":");
        sb_json_str(&b, prompt);
    } else {
        sb_append(&b, "{\"prompt\":");
        sb_json_str(&b, prompt);
    }
    sb_append(&b, "}");
    if (b.oom) { sb_free(&b); out->kind = RES_BAD_REQUEST; strcpy(out->error, "нет памяти"); return; }

    http_response r;
    http_post_json_big(url, headers, 1, b.data, timeout_ms, kind == KIND_AUDIO ? MAX_AUDIO : MAX_IMAGE, &r);
    sb_free(&b);
    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    const char *fallback = kind == KIND_AUDIO ? "audio/mpeg" : "image/png";
    if (starts_with(r.content_type, "image/") || starts_with(r.content_type, "audio/") ||
        starts_with(r.content_type, "application/octet-stream")) {
        take_media(out, &r, fallback); /* модель отдала сам файл */
        http_response_free(&r);
        return;
    }
    json *j = json_parse(r.body.data, NULL, 0);
    const json *res = json_get(j, "result");
    const char *b64 = json_str(json_get(res, kind == KIND_AUDIO ? "audio" : "image"), NULL);
    if (!b64) b64 = json_str(res, NULL);
    if (!j) {
        out->kind = RES_BAD_RESPONSE;
        xstrlcpy(out->error, "ответ не JSON и не файл", sizeof out->error);
    } else {
        take_b64(out, b64, fallback);
    }
    json_free(j);
    http_response_free(&r);
}

/* ------------------------------------------------------------ Pollinations (без ключа) ---- */

void provider_pollinations(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    (void)api_key;
    memset(out, 0, sizeof *out);
    strbuf u;
    sb_init(&u);
    sb_append(&u, m->base_url[0] ? m->base_url : POLL_BASE);
    sb_appendc(&u, '/');
    urlencode(&u, request_prompt(req));
    sb_printf(&u, "?width=1024&height=1024&nologo=true&private=true&seed=%u", (unsigned)(plat_now_ms() % 1000000007));
    if (m->model[0]) {
        sb_append(&u, "&model=");
        urlencode(&u, m->model);
    }
    if (u.oom) { sb_free(&u); out->kind = RES_BAD_REQUEST; strcpy(out->error, "нет памяти"); return; }
    http_response r;
    http_get(u.data, NULL, 0, timeout_ms, MAX_IMAGE, &r);
    sb_free(&u);
    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
    } else if (!starts_with(r.content_type, "image/") && !sniff_mime((unsigned char *)r.body.data, r.body.len)) {
        out->kind = RES_BAD_RESPONSE;
        out->http_status = 200;
        snprintf(out->error, sizeof out->error, "вместо картинки пришло: %.120s", r.body.data);
    } else {
        take_media(out, &r, "image/jpeg");
    }
    http_response_free(&r);
}

/* ------------------------------------------------------------ Gemini: картинки и речь ---- */

static void gemini_generate(const model_cfg *m, const char *api_key, const char *body, long timeout_ms, size_t max,
                            http_response *r) {
    char url[512], hkey[320];
    snprintf(url, sizeof url, "%s/models/%s:generateContent", m->base_url[0] ? m->base_url : GEMINI_BASE, m->model);
    snprintf(hkey, sizeof hkey, "x-goog-api-key: %s", api_key);
    const char *headers[] = { hkey };
    http_post_json_big(url, headers, 1, body, timeout_ms, max, r);
}

/* Найти inlineData в ответе generateContent. Возвращает узел или NULL; text — первый текстовый фрагмент. */
static const json *find_inline(const json *j, const char **text) {
    const json *parts = json_path(j, "candidates.0.content.parts");
    *text = NULL;
    for (size_t i = 0; i < json_len(parts); i++) {
        const json *p = json_at(parts, i);
        const json *d = json_get(p, "inlineData");
        if (!d) d = json_get(p, "inline_data");
        if (d) return d;
        if (!*text) *text = json_str(json_get(p, "text"), NULL);
    }
    return NULL;
}

/* Почему модель не вернула файл: блокировка, причина остановки или её текст. */
static void no_media_reason(const json *j, const char *text, const char *what, call_result *out) {
    const char *block = json_str(json_path(j, "promptFeedback.blockReason"), NULL);
    const char *finish = json_str(json_path(j, "candidates.0.finishReason"), NULL);
    out->kind = RES_BAD_RESPONSE;
    out->http_status = 200;
    if (block) snprintf(out->error, sizeof out->error, "запрос заблокирован фильтром (%s)", block);
    else if (text) snprintf(out->error, sizeof out->error, "модель ответила текстом вместо %s: %.200s", what, text);
    else snprintf(out->error, sizeof out->error, "модель не вернула %s%s%s", what, finish ? ": " : "", finish ? finish : "");
}

void provider_gemini_image(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":");
    sb_json_str(&b, request_prompt(req));
    sb_append(&b, "}]}],\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"IMAGE\"]}}");
    http_response r;
    gemini_generate(m, api_key, b.data, timeout_ms, MAX_IMAGE, &r);
    sb_free(&b);
    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    json *j = json_parse(r.body.data, NULL, 0);
    const char *text;
    const json *d = find_inline(j, &text);
    if (d) take_b64(out, json_str(json_get(d, "data"), NULL), "image/png");
    else no_media_reason(j, text, "картинку", out);
    json_free(j);
    http_response_free(&r);
}

void provider_gemini_tts(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":");
    sb_json_str(&b, request_prompt(req));
    sb_append(&b, "}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
                  "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}}}");
    http_response r;
    gemini_generate(m, api_key, b.data, timeout_ms, MAX_AUDIO, &r);
    sb_free(&b);
    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    json *j = json_parse(r.body.data, NULL, 0);
    const char *text;
    const json *d = find_inline(j, &text);
    if (!d) {
        no_media_reason(j, text, "аудио", out);
    } else if (take_b64(out, json_str(json_get(d, "data"), NULL), "audio/wav") == 0 && !sniff_mime(out->media, out->media_len)) {
        /* «сырой» PCM: audio/L16;codec=pcm;rate=24000 — добавляем WAV-заголовок, чтобы играл браузер */
        const char *mime = json_str(json_get(d, "mimeType"), "");
        const char *rs = strstr(mime, "rate=");
        int rate = rs ? atoi(rs + 5) : 24000;
        if (rate < 8000 || rate > 96000) rate = 24000;
        size_t wl = 0;
        unsigned char *wav = wav_from_pcm(out->media, out->media_len, rate, 1, &wl);
        if (wav) {
            free(out->media);
            out->media = wav;
            out->media_len = wl;
            xstrlcpy(out->media_mime, "audio/wav", sizeof out->media_mime);
        }
    }
    json_free(j);
    http_response_free(&r);
}

/* ------------------------------------------------------------ Gemini Veo: видео ---- */

void provider_gemini_veo(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    const char *base = m->base_url[0] ? m->base_url : GEMINI_BASE;
    char url[700], hkey[320];
    snprintf(url, sizeof url, "%s/models/%s:predictLongRunning", base, m->model);
    snprintf(hkey, sizeof hkey, "x-goog-api-key: %s", api_key);
    const char *headers[] = { hkey };
    int64_t deadline = plat_now_ms() + timeout_ms;

    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"instances\":[{\"prompt\":");
    sb_json_str(&b, request_prompt(req));
    sb_append(&b, "}],\"parameters\":{\"aspectRatio\":\"16:9\"}}");
    http_response r;
    http_post_json(url, headers, 1, b.data, 60000, &r);
    sb_free(&b);
    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    json *j = json_parse(r.body.data, NULL, 0);
    char op[300] = "";
    xstrlcpy(op, json_str(json_get(j, "name"), ""), sizeof op);
    json_free(j);
    http_response_free(&r);
    if (!op[0] || strstr(op, "..") || strchr(op, '?')) {
        out->kind = RES_BAD_RESPONSE;
        xstrlcpy(out->error, "Veo не вернул id операции", sizeof out->error);
        return;
    }

    /* опрос: видео генерируется от 10 секунд до нескольких минут */
    char uri[1024] = "";
    for (;;) {
        if (plat_now_ms() > deadline) {
            out->kind = RES_NETWORK;
            snprintf(out->error, sizeof out->error, "видео не готово за %ld с", timeout_ms / 1000);
            return;
        }
        plat_sleep_ms(m->base_url[0] && strstr(m->base_url, "127.0.0.1") ? 200 : 10000); /* тестовый сервер — быстрее */
        snprintf(url, sizeof url, "%s/%s", base, op);
        http_get(url, headers, 1, 60000, 1u << 20, &r);
        if (r.status != 200) {
            fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
            http_response_free(&r);
            return;
        }
        j = json_parse(r.body.data, NULL, 0);
        int done = json_bool(json_get(j, "done"), 0);
        const json *err = json_get(j, "error");
        if (err) {
            /* ошибка внутри операции: те же правила, что и для HTTP-ошибок (квота, фильтр…) */
            long code = (long)json_num(json_get(err, "code"), 13);
            /* коды google.rpc → HTTP: RESOURCE_EXHAUSTED → 429, INVALID_ARGUMENT → 400, PERMISSION_DENIED → 403 … */
            long http = code >= 400 ? code : code == 8 ? 429 : code == 3 || code == 9 || code == 11 ? 400
                      : code == 7 || code == 16 ? 403 : code == 5 ? 404 : 500;
            fill_http_error(out, http, r.body.data, 0, NULL);
            json_free(j);
            http_response_free(&r);
            return;
        }
        if (done) {
            const json *resp = json_path(j, "response.generateVideoResponse");
            xstrlcpy(uri, json_str(json_path(resp, "generatedSamples.0.video.uri"), ""), sizeof uri);
            if (!uri[0]) {
                const char *why = json_str(json_path(resp, "raiMediaFilteredReasons.0"), NULL);
                out->kind = RES_BAD_RESPONSE;
                out->http_status = 200;
                snprintf(out->error, sizeof out->error, "видео не создано%s%.200s", why ? ": " : "", why ? why : "");
            }
        }
        json_free(j);
        http_response_free(&r);
        if (done) break;
    }
    if (!uri[0]) return;

    /* скачивание: ключ — параметром, чтобы он не уходил в заголовке на хост редиректа */
    strbuf du;
    sb_init(&du);
    sb_append(&du, uri);
    sb_append(&du, strchr(uri, '?') ? "&key=" : "?key=");
    urlencode(&du, api_key);
    long left = (long)(deadline - plat_now_ms());
    http_get(du.data, NULL, 0, left > 120000 ? left : 120000, MAX_VIDEO, &r);
    sb_free(&du);
    if (r.status != 200) fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
    else take_media(out, &r, "video/mp4");
    http_response_free(&r);
}
