#include "provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "../strbuf.h"

const char *result_kind_code(result_kind k) {
    switch (k) {
        case RES_OK: return "ok";
        case RES_RATE_LIMIT: return "rate_limit";
        case RES_QUOTA: return "quota";
        case RES_AUTH: return "auth";
        case RES_BAD_REQUEST: return "bad_request";
        case RES_SERVER: return "server_error";
        case RES_NETWORK: return "network";
        case RES_BAD_RESPONSE: return "bad_response";
        case RES_NOT_CONFIGURED: return "no_api_key";
        case RES_LOCAL_LIMIT: return "local_limit";
        case RES_COOLDOWN: return "cooldown";
        case RES_DISABLED: return "disabled";
    }
    return "unknown";
}

int result_is_limit(result_kind k) { return k == RES_RATE_LIMIT || k == RES_QUOTA || k == RES_LOCAL_LIMIT || k == RES_COOLDOWN; }

void call_result_free(call_result *r) {
    free(r->text);
    r->text = NULL;
    free(r->media);
    r->media = NULL;
    r->media_len = 0;
}

provider_call_fn provider_for(const model_cfg *m) {
    media_kind k = model_kind(m);
    if (!strcmp(m->provider, "mock")) return provider_mock;
    if (!strcmp(m->provider, "cloudflare")) return provider_cloudflare;
    if (!strcmp(m->provider, "pollinations")) return provider_pollinations;
    if (!strcmp(m->provider, "gemini") && k == KIND_IMAGE) return provider_gemini_image;
    if (!strcmp(m->provider, "gemini") && k == KIND_AUDIO) return provider_gemini_tts;
    if (!strcmp(m->provider, "gemini") && k == KIND_VIDEO) return provider_gemini_veo;
    if (!strcmp(m->provider, "gemini")) return provider_gemini;
    if (!strcmp(m->provider, "openai")) return provider_openai;
    if (!strcmp(m->provider, "anthropic")) return provider_anthropic;
    if (!strcmp(m->provider, "mock")) return provider_mock;
    return NULL;
}

/* Поиск подстроки без учёта регистра (ASCII). */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

result_kind classify_http_error(long status, const char *body) {
    if (status == 0) return RES_NETWORK;
    if (status == 429) {
        /* Отличаем «исчерпана квота/бесплатный лимит» от кратковременного rate limit:
         *  Google: status RESOURCE_EXHAUSTED + "quota"; OpenAI: code insufficient_quota;
         *  Groq/OpenRouter: "per day"/"daily". */
        if (contains_ci(body, "insufficient_quota") || contains_ci(body, "quota") || contains_ci(body, "per day") ||
            contains_ci(body, "daily") || contains_ci(body, "billing"))
            return RES_QUOTA;
        return RES_RATE_LIMIT;
    }
    if (status == 402) return RES_QUOTA;                           /* payment required */
    if (status == 401 || status == 403) return RES_AUTH;
    if (status == 400 && (contains_ci(body, "api key") || contains_ci(body, "api_key"))) return RES_AUTH; /* Gemini */
    if (status == 400 && contains_ci(body, "credit balance")) return RES_QUOTA; /* Anthropic: нет средств */
    if (status == 408) return RES_NETWORK;
    if (status >= 500) return RES_SERVER;                          /* в т.ч. 529 overloaded */
    if (status >= 400) return RES_BAD_REQUEST;
    return RES_BAD_RESPONSE;
}

void extract_error_message(const char *body, char *out, size_t outlen) {
    out[0] = '\0';
    json *j = json_parse(body, NULL, 0);
    if (j) {
        const char *msg = json_str(json_path(j, "error.message"), NULL);  /* OpenAI/Gemini/Anthropic */
        if (!msg) msg = json_str(json_get(j, "message"), NULL);
        if (!msg) msg = json_str(json_get(j, "error"), NULL);
        if (!msg) msg = json_str(json_path(j, "0.error.message"), NULL);  /* Gemini иногда отдаёт массив */
        if (!msg) msg = json_str(json_path(j, "errors.0.message"), NULL); /* Cloudflare */
        if (msg) xstrlcpy(out, msg, outlen);
        json_free(j);
    }
    if (!out[0] && body) {
        /* не JSON — берём начало тела, без переводов строк */
        xstrlcpy(out, body, outlen > 200 ? 200 : outlen);
        for (char *p = out; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

/* Google присылает задержку в теле: "retryDelay": "34s". */
static long parse_retry_delay(const char *body) {
    const char *p = body ? strstr(body, "\"retryDelay\"") : NULL;
    if (!p) return 0;
    p = strchr(p + 12, '"');
    if (!p) return 0;
    p++;
    long v = strtol(p, NULL, 10);
    return v > 0 ? v : 0;
}

void fill_http_error(call_result *out, long status, const char *body, long header_retry_after, const char *net_error) {
    out->http_status = status;
    out->kind = classify_http_error(status, body);
    out->retry_after_s = header_retry_after > 0 ? header_retry_after : parse_retry_delay(body);
    out->limit_daily = contains_ci(body, "PerDay") || contains_ci(body, "per day") || contains_ci(body, "daily");
    /* Google пишет «check your plan and billing details» даже на минутном лимите — поэтому «billing» не используем */
    out->limit_billing = status == 402 || contains_ci(body, "insufficient_quota") || contains_ci(body, "credit balance");
    if (status == 0) {
        snprintf(out->error, sizeof out->error, "нет ответа: %s", net_error && *net_error ? net_error : "сетевая ошибка");
        return;
    }
    char msg[400];
    extract_error_message(body, msg, sizeof msg);
    snprintf(out->error, sizeof out->error, "HTTP %ld: %s", status, msg[0] ? msg : "без описания");
}

/* ------------------------------------------------------------ медиа-утилиты ---- */

unsigned char *b64_decode(const char *s, size_t len, size_t *out_len) {
    static signed char tab[256];
    static int ready = 0;
    if (!ready) {
        memset(tab, -1, sizeof tab);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; a[i]; i++) tab[(unsigned char)a[i]] = (signed char)i;
        tab['-'] = 62; /* base64url */
        tab['_'] = 63;
        ready = 1;
    }
    unsigned char *out = malloc(len / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '=') break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (tab[c] < 0) { free(out); return NULL; }
        v = (v << 6) | (unsigned)tab[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(v >> bits);
        }
    }
    *out_len = o;
    return out;
}

const char *sniff_mime(const unsigned char *d, size_t n) {
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "image/gif";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) return "audio/wav";
    if (n >= 3 && !memcmp(d, "ID3", 3)) return "audio/mpeg";
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "audio/mpeg";
    if (n >= 4 && !memcmp(d, "OggS", 4)) return "audio/ogg";
    if (n >= 12 && !memcmp(d + 4, "ftyp", 4)) return "video/mp4";
    if (n >= 4 && !memcmp(d, "\x1a\x45\xdf\xa3", 4)) return "video/webm";
    return NULL;
}

static void put_le(unsigned char *p, unsigned v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8 * i));
}

unsigned char *wav_from_pcm(const unsigned char *pcm, size_t n, int rate, int channels, size_t *out_len) {
    unsigned char *w = malloc(44 + n);
    if (!w) return NULL;
    memcpy(w, "RIFF", 4);
    put_le(w + 4, (unsigned)(36 + n), 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    put_le(w + 16, 16, 4);                               /* размер fmt-блока */
    put_le(w + 20, 1, 2);                                /* PCM */
    put_le(w + 22, (unsigned)channels, 2);
    put_le(w + 24, (unsigned)rate, 4);
    put_le(w + 28, (unsigned)(rate * channels * 2), 4);  /* байт в секунду */
    put_le(w + 32, (unsigned)(channels * 2), 2);
    put_le(w + 34, 16, 2);                               /* бит на отсчёт */
    memcpy(w + 36, "data", 4);
    put_le(w + 40, (unsigned)n, 4);
    memcpy(w + 44, pcm, n);
    *out_len = 44 + n;
    return w;
}

int split_account_key(const model_cfg *m, const char *key, char *account, size_t alen, const char **token, call_result *out) {
    const char *colon = strchr(key, ':');
    if (!colon || colon == key || !colon[1] || (size_t)(colon - key) >= alen || (size_t)(colon - key) > 64) {
        out->kind = RES_AUTH;
        snprintf(out->error, sizeof out->error, "для %s ключ вводится в виде ACCOUNT_ID:API_TOKEN", m->key_name);
        return -1;
    }
    memcpy(account, key, (size_t)(colon - key));
    account[colon - key] = '\0';
    *token = colon + 1;
    return 0;
}

const char *request_prompt(const chat_request *req) {
    return req->n ? req->msgs[req->n - 1].content : "";
}

int has_cyrillic(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
        if ((*p == 0xD0 || *p == 0xD1) && p[1] >= 0x80 && p[1] <= 0xBF) return 1;
    return 0;
}
