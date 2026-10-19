/* Google AI Studio (Gemini API): POST {base}/models/{model}:generateContent */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_client.h"
#include "../json.h"
#include "../strbuf.h"
#include "provider.h"

#define GEMINI_BASE "https://generativelanguage.googleapis.com/v1beta"

static char *build_body(const model_cfg *m, const chat_request *req) {
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{");
    if (req->system && *req->system) {
        sb_append(&b, "\"systemInstruction\":{\"parts\":[{\"text\":");
        sb_json_str(&b, req->system);
        sb_append(&b, "}]},");
    }
    sb_append(&b, "\"contents\":[");
    for (size_t i = 0; i < req->n; i++) {
        if (i) sb_appendc(&b, ',');
        /* у Gemini роль ассистента называется "model" */
        sb_append(&b, "{\"role\":");
        sb_json_str(&b, strcmp(req->msgs[i].role, "assistant") ? "user" : "model");
        sb_append(&b, ",\"parts\":[{\"text\":");
        sb_json_str(&b, req->msgs[i].content);
        sb_append(&b, "}]}");
    }
    sb_printf(&b, "],\"generationConfig\":{\"maxOutputTokens\":%d}}", m->max_tokens);
    return b.oom ? (sb_free(&b), NULL) : sb_take(&b);
}

void provider_gemini(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    char url[512];
    snprintf(url, sizeof url, "%s/models/%s:generateContent", m->base_url[0] ? m->base_url : GEMINI_BASE, m->model);
    char hkey[320];
    snprintf(hkey, sizeof hkey, "x-goog-api-key: %s", api_key);
    const char *headers[] = { hkey };

    char *body = build_body(m, req);
    if (!body) { out->kind = RES_BAD_REQUEST; strcpy(out->error, "нет памяти для запроса"); return; }
    http_response r;
    http_post_json(url, headers, 1, body, timeout_ms, &r);
    free(body);

    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    out->http_status = 200;
    json *j = json_parse(r.body.data, NULL, 0);
    if (!j) {
        out->kind = RES_BAD_RESPONSE;
        strcpy(out->error, "ответ Gemini не является JSON");
        http_response_free(&r);
        return;
    }
    strbuf text;
    sb_init(&text);
    const json *parts = json_path(j, "candidates.0.content.parts");
    for (size_t i = 0; i < json_len(parts); i++) {
        const json *p = json_at(parts, i);
        if (json_bool(json_get(p, "thought"), 0)) continue; /* пропускаем «мысли» thinking-моделей */
        sb_append(&text, json_str(json_get(p, "text"), ""));
    }
    out->tokens_in = (long)json_num(json_path(j, "usageMetadata.promptTokenCount"), 0);
    out->tokens_out = (long)json_num(json_path(j, "usageMetadata.candidatesTokenCount"), 0);
    if (text.len == 0) {
        const char *block = json_str(json_path(j, "promptFeedback.blockReason"), NULL);
        const char *finish = json_str(json_path(j, "candidates.0.finishReason"), NULL);
        out->kind = block ? RES_BAD_REQUEST : RES_BAD_RESPONSE;
        snprintf(out->error, sizeof out->error, "пустой ответ Gemini (%s)",
                 block ? block : finish ? finish : "нет кандидатов");
        sb_free(&text);
    } else {
        out->kind = RES_OK;
        out->text = sb_take(&text);
    }
    json_free(j);
    http_response_free(&r);
}
