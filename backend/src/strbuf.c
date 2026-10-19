#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf *sb) { sb->data = NULL; sb->len = sb->cap = 0; sb->oom = 0; }

void sb_free(strbuf *sb) { free(sb->data); sb_init(sb); }

void sb_reset(strbuf *sb) { sb->len = 0; if (sb->data) sb->data[0] = '\0'; }

static int sb_grow(strbuf *sb, size_t extra) {
    if (sb->oom) return -1;
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return 0;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p) { sb->oom = 1; return -1; }
    sb->data = p;
    sb->cap = cap;
    return 0;
}

void sb_append_n(strbuf *sb, const char *s, size_t n) {
    if (!n) { if (!sb->data && sb_grow(sb, 0) == 0) sb->data[0] = '\0'; return; }
    if (sb_grow(sb, n) != 0) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(strbuf *sb, const char *s) { if (s) sb_append_n(sb, s, strlen(s)); }

void sb_appendc(strbuf *sb, char c) { sb_append_n(sb, &c, 1); }

void sb_printf(strbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && sb_grow(sb, (size_t)n) == 0) {
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
        sb->len += (size_t)n;
    }
    va_end(ap2);
}

void sb_json_str(strbuf *sb, const char *s) {
    if (!s) { sb_append(sb, "null"); return; }
    static const char hex[] = "0123456789abcdef";
    sb_appendc(sb, '"');
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *run = p;
    for (; *p; p++) {
        const char *esc = NULL;
        char u[7];
        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            default:
                if (*p < 0x20) {
                    u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                    u[4] = hex[*p >> 4]; u[5] = hex[*p & 15]; u[6] = '\0';
                    esc = u;
                }
        }
        if (esc) {
            sb_append_n(sb, (const char *)run, (size_t)(p - run));
            sb_append(sb, esc);
            run = p + 1;
        }
    }
    sb_append_n(sb, (const char *)run, (size_t)(p - run));
    sb_appendc(sb, '"');
}

char *sb_take(strbuf *sb) {
    char *d = sb->data;
    if (!d) d = xstrdup("");
    sb_init(sb);
    return d;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void xstrlcpy(char *dst, const char *src, size_t size) {
    if (!size) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}
