/* OpenAI Chat Completions и все совместимые API: OpenAI, Groq, OpenRouter, Mistral, Cerebras, Together,
 * GitHub Models, NVIDIA NIM, SambaNova, Scaleway и т.д. Различается только base_url. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_client.h"
#include "../json.h"
#include "../strbuf.h"
#include "provider.h"

#define OPENAI_BASE "https://api.openai.com/v1"

static char *build_body(const model_cfg *m, const chat_request *req) {
    strbuf b;
    sb_init(&b);
    sb_append(&b, "{\"model\":");
    sb_json_str(&b, m->model);
    sb_append(&b, ",\"messages\":[");
    int first = 1;
    if (req->system && *req->system) {
        sb_append(&b, "{\"role\":\"system\",\"content\":");
        sb_json_str(&b, req->system);
        sb_append(&b, "}");
        first = 0;
    }
    for (size_t i = 0; i < req->n; i++) {
        if (!first) sb_appendc(&b, ',');
        first = 0;
        sb_append(&b, "{\"role\":");
        sb_json_str(&b, req->msgs[i].role);
        sb_append(&b, ",\"content\":");
        sb_json_str(&b, req->msgs[i].content);
        sb_append(&b, "}");
    }
    sb_append(&b, "],");
    sb_json_str(&b, m->max_tokens_field[0] ? m->max_tokens_field : "max_tokens");
    sb_printf(&b, ":%d}", m->max_tokens);
    return b.oom ? (sb_free(&b), NULL) : sb_take(&b);
}

void provider_openai(const model_cfg *m, const char *api_key, const chat_request *req, long timeout_ms, call_result *out) {
    memset(out, 0, sizeof *out);
    char url[600], base_buf[512];
    const char *base = m->base_url[0] ? m->base_url : OPENAI_BASE;
    const char *token = api_key;
    /* Cloudflare Workers AI: в URL нужен ID аккаунта — ключ вводится как «ACCOUNT_ID:API_TOKEN» */
    const char *ph = strstr(base, "{account}");
    if (ph) {
        char account[80];
        if (split_account_key(m, api_key, account, sizeof account, &token, out) != 0) return;
        snprintf(base_buf, sizeof base_buf, "%.*s%s%s", (int)(ph - base), base, account, ph + strlen("{account}"));
        base = base_buf;
    }
    size_t bl = strlen(base);
    snprintf(url, sizeof url, "%s%schat/completions", base, (bl && base[bl - 1] == '/') ? "" : "/");
    char auth[640];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *headers[] = { auth };

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
    const char *text = json_str(json_path(j, "choices.0.message.content"), NULL);
    if (!j || !text || !*text) {
        /* некоторые шлюзы (OpenRouter) возвращают ошибку внутри 200 */
        char msg[300] = "";
        if (j) extract_error_message(r.body.data, msg, sizeof msg);
        out->kind = RES_BAD_RESPONSE;
        snprintf(out->error, sizeof out->error, "пустой ответ модели%s%s", msg[0] ? ": " : "", msg);
    } else {
        out->kind = RES_OK;
        out->text = xstrdup(text);
        out->tokens_in = (long)json_num(json_path(j, "usage.prompt_tokens"), 0);
        out->tokens_out = (long)json_num(json_path(j, "usage.completion_tokens"), 0);
    }
    json_free(j);
    http_response_free(&r);
}
