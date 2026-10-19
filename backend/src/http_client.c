#include "http_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define MAX_RESPONSE (16u << 20) /* 16 МБ — защита от бесконечного ответа */

typedef struct {
    http_response *r;
    size_t         max;
} sink;

int http_global_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1; }
void http_global_cleanup(void) { curl_global_cleanup(); }

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    sink *k = ud;
    http_response *r = k->r;
    size_t n = size * nmemb;
    if (r->body.len + n > k->max) return 0; /* прервать передачу */
    sb_append_n(&r->body, ptr, n);
    return r->body.oom ? 0 : n;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *ud) {
    http_response *r = ud;
    size_t n = size * nmemb;
    static const char name[] = "retry-after:";
    if (n > sizeof name - 1) {
        size_t i;
        for (i = 0; i < sizeof name - 1; i++)
            if (tolower((unsigned char)ptr[i]) != name[i]) break;
        if (i == sizeof name - 1) {
            const char *v = ptr + i;
            while (v < ptr + n && (*v == ' ' || *v == '\t')) v++;
            if (v < ptr + n && isdigit((unsigned char)*v)) r->retry_after_s = strtol(v, NULL, 10);
            /* формат HTTP-date не разбираем: роутер подставит паузу по умолчанию */
        }
    }
    return n;
}

/* Общая часть: method — "GET" или "POST" (body != NULL). */
static int perform(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                   size_t max_bytes, int json_headers, http_response *out) {
    memset(out, 0, sizeof *out);
    sb_init(&out->body);
    CURL *c = curl_easy_init();
    if (!c) { strcpy(out->error, "curl_easy_init failed"); return -1; }

    struct curl_slist *hl = NULL;
    if (json_headers) {
        hl = curl_slist_append(hl, "Content-Type: application/json");
        hl = curl_slist_append(hl, "Accept: application/json");
    }
    for (int i = 0; i < nheaders; i++) hl = curl_slist_append(hl, headers[i]);

    sink k = { out, max_bytes ? max_bytes : MAX_RESPONSE };
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
        /* видео Google и картинки Pollinations отдаются через редирект; заголовки с ключом
         * curl по умолчанию не пересылает на другой хост */
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); /* обязательно в многопоточном сервере */
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "infinityAI-router/0.1");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(c);
    int result = 0;
    if (rc != CURLE_OK) {
        if (rc == CURLE_WRITE_ERROR && out->body.len + 1 >= k.max)
            snprintf(out->error, sizeof out->error, "ответ больше %zu МБ", k.max >> 20);
        else
            snprintf(out->error, sizeof out->error, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
        out->status = 0;
        result = -1;
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
        char *ct = NULL;
        if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) xstrlcpy(out->content_type, ct, sizeof out->content_type);
    }
    if (!out->body.data) sb_append_n(&out->body, "", 0);
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return result;
}

int http_post_json(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                   http_response *out) {
    return perform(url, headers, nheaders, body, timeout_ms, MAX_RESPONSE, 1, out);
}

int http_post_json_big(const char *url, const char *const *headers, int nheaders, const char *body, long timeout_ms,
                       size_t max_bytes, http_response *out) {
    return perform(url, headers, nheaders, body, timeout_ms, max_bytes, 1, out);
}

int http_get(const char *url, const char *const *headers, int nheaders, long timeout_ms, size_t max_bytes,
             http_response *out) {
    return perform(url, headers, nheaders, NULL, timeout_ms, max_bytes, 0, out);
}

void http_response_free(http_response *r) { sb_free(&r->body); }
