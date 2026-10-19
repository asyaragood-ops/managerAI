/* Тестовый провайдер без сети: позволяет проверить каскад без ключей.
 * mock_behavior: ok | rate_limit | quota | daily_quota | server_error | auth_error | network */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../strbuf.h"
#include "provider.h"

void provider_mock(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    (void)api_key;
    (void)timeout_ms;
    memset(out, 0, sizeof *out);
    const char *b = m->mock_behavior;
    if (!strcmp(b, "rate_limit")) {
        fill_http_error(out, 429, "{\"error\":{\"message\":\"Rate limit reached (mock)\"}}", 7, NULL);
        return;
    }
    if (!strcmp(b, "quota")) {
        fill_http_error(out, 429,
                        "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota (mock)\",\"status\":\"RESOURCE_EXHAUSTED\"}}",
                        0, NULL);
        return;
    }
    if (!strcmp(b, "daily_quota")) {
        fill_http_error(out, 429,
                        "{\"error\":{\"message\":\"Quota exceeded: GenerateRequestsPerDayPerProjectPerModel-FreeTier (mock)\"}}", 0,
                        NULL);
        return;
    }
    if (!strcmp(b, "server_error")) { fill_http_error(out, 503, "{\"error\":{\"message\":\"Service unavailable (mock)\"}}", 0, NULL); return; }
    if (!strcmp(b, "auth_error")) { fill_http_error(out, 401, "{\"error\":{\"message\":\"Invalid API key (mock)\"}}", 0, NULL); return; }
    if (!strcmp(b, "network")) { fill_http_error(out, 0, "", 0, "connection refused (mock)"); return; }

    const char *last = req->n ? req->msgs[req->n - 1].content : "";
    media_kind kind = model_kind(m);
    if (kind != KIND_CHAT) {
        /* «файл» с правильной сигнатурой: роутер сохраняет его и отдаёт ссылку */
        static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
        static const unsigned char mp4[] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0 };
        static const unsigned char pcm[64] = { 0 };
        if (kind == KIND_AUDIO) {
            out->media = wav_from_pcm(pcm, sizeof pcm, 24000, 1, &out->media_len);
        } else {
            const unsigned char *src = kind == KIND_IMAGE ? png : mp4;
            out->media = malloc(16);
            if (out->media) { memcpy(out->media, src, 16); out->media_len = 16; }
        }
        xstrlcpy(out->media_mime, kind == KIND_IMAGE ? "image/png" : kind == KIND_AUDIO ? "audio/wav" : "video/mp4", sizeof out->media_mime);
        out->text = xstrdup(last); /* для тестов: что получила модель */
        out->kind = out->media ? RES_OK : RES_BAD_RESPONSE;
        out->http_status = 200;
        return;
    }
    strbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "[%s] Тестовый ответ на: «%.200s» (сообщений: %zu)", m->label, last, req->n);
    out->kind = RES_OK;
    out->http_status = 200;
    out->text = sb_take(&sb);
    out->tokens_in = (long)strlen(last) / 4 + 1;
    out->tokens_out = 12;
}
