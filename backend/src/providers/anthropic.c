/* Anthropic Messages API: POST https://api.anthropic.com/v1/messages */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_client.h"
#include "../json.h"
#include "../strbuf.h"
#include "provider.h"

#define ANTHROPIC_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"

static char *build_body(const model_cfg *m, const chat_request *req) {
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"model\":");
    sb_json_str(&b, m->model);
    sb_printf(&b, ",\"max_tokens\":%d", m->max_tokens);
    if (req->system && *req->system) {
        sb_append(&b, ",\"system\":");
        sb_json_str(&b, req->system);
    }
    sb_append(&b, ",\"messages\":[");
    for (size_t i = 0; i < req->n; i++) {
        if (i) sb_appendc(&b, ',');
        sb_append(&b, "{\"role\":");
        sb_json_str(&b, req->msgs[i].role);
        sb_append(&b, ",\"content\":");
        sb_json_str(&b, req->msgs[i].content);
        sb_append(&b, "}");
    }
    sb_append(&b, "]}");
    return b.oom ? (sb_free(&b), NULL) : sb_take(&b);
}

void provider_anthropic(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms,
                        call_result *out) {
    memset(out, 0, sizeof *out);
    char hkey[320];
    snprintf(hkey, sizeof hkey, "x-api-key: %s", api_key);
    const char *headers[] = { hkey, "anthropic-version: " ANTHROPIC_VERSION };

    char *body = build_body(m, req);
    if (!body) { out->kind = RES_BAD_REQUEST; strcpy(out->error, "нет памяти для запроса"); return; }
    http_response r;
    http_post_json(m->base_url[0] ? m->base_url : ANTHROPIC_URL, headers, 2, body, timeout_ms, &r);
    free(body);

    if (r.status != 200) {
        fill_http_error(out, r.status, r.body.data, r.retry_after_s, r.error);
        http_response_free(&r);
        return;
    }
    out->http_status = 200;
    json *j = json_parse(r.body.data, NULL, 0);
    strbuf text;
    sb_init(&text);
    const json *content = json_get(j, "content");
    for (size_t i = 0; i < json_len(content); i++) {
        const json *blk = json_at(content, i);
        if (!strcmp(json_str(json_get(blk, "type"), ""), "text")) sb_append(&text, json_str(json_get(blk, "text"), ""));
    }
    if (!j || text.len == 0) {
        out->kind = RES_BAD_RESPONSE;
        snprintf(out->error, sizeof out->error, "пустой ответ Claude (stop_reason: %s)",
                 json_str(json_get(j, "stop_reason"), "нет"));
        sb_free(&text);
    } else {
        out->kind = RES_OK;
        out->text = sb_take(&text);
        out->tokens_in = (long)json_num(json_path(j, "usage.input_tokens"), 0);
        out->tokens_out = (long)json_num(json_path(j, "usage.output_tokens"), 0);
    }
    json_free(j);
    http_response_free(&r);
}
