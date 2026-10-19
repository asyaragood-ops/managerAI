#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strbuf.h"

#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *start;
    char       *err;
    size_t      errlen;
    int         failed;
} parser;

static void fail(parser *ps, const char *msg) {
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->err && ps->errlen)
        snprintf(ps->err, ps->errlen, "%s (позиция %ld)", msg, (long)(ps->p - ps->start));
}

static void skip_ws(parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static json *node_new(json_type t) {
    json *j = calloc(1, sizeof *j);
    if (j) j->type = t;
    return j;
}

void json_free(json *j) {
    while (j) {
        json *next = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = next;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(parser *ps, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0) { fail(ps, "некорректная \\u-последовательность"); return -1; }
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(strbuf *sb, unsigned cp) {
    char b[4];
    if (cp < 0x80) { b[0] = (char)cp; sb_append_n(sb, b, 1); }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); sb_append_n(sb, b, 2); }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F));
        sb_append_n(sb, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F));
        sb_append_n(sb, b, 4);
    }
}

static char *parse_string_raw(parser *ps) {
    if (*ps->p != '"') { fail(ps, "ожидалась строка"); return NULL; }
    ps->p++;
    strbuf sb;
    sb_init(&sb);
    sb_append_n(&sb, "", 0);
    for (;;) {
        const char *run = ps->p;
        while (*ps->p && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20) ps->p++;
        sb_append_n(&sb, run, (size_t)(ps->p - run));
        char c = *ps->p;
        if (c == '"') { ps->p++; break; }
        if (c == '\0') { fail(ps, "незакрытая строка"); sb_free(&sb); return NULL; }
        if ((unsigned char)c < 0x20) { fail(ps, "управляющий символ в строке"); sb_free(&sb); return NULL; }
        /* c == '\\' */
        ps->p++;
        char e = *ps->p++;
        switch (e) {
            case '"': sb_appendc(&sb, '"'); break;
            case '\\': sb_appendc(&sb, '\\'); break;
            case '/': sb_appendc(&sb, '/'); break;
            case 'b': sb_appendc(&sb, '\b'); break;
            case 'f': sb_appendc(&sb, '\f'); break;
            case 'n': sb_appendc(&sb, '\n'); break;
            case 'r': sb_appendc(&sb, '\r'); break;
            case 't': sb_appendc(&sb, '\t'); break;
            case 'u': {
                unsigned cp;
                if (read_hex4(ps, &cp) != 0) { sb_free(&sb); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF) { /* суррогатная пара */
                    unsigned lo;
                    if (ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        if (read_hex4(ps, &lo) != 0) { sb_free(&sb); return NULL; }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else cp = 0xFFFD;
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                put_utf8(&sb, cp);
                break;
            }
            default: fail(ps, "неизвестная escape-последовательность"); sb_free(&sb); return NULL;
        }
    }
    if (sb.oom) { fail(ps, "нет памяти"); sb_free(&sb); return NULL; }
    return sb_take(&sb);
}

static json *parse_value(parser *ps, int depth);

static json *parse_number(parser *ps) {
    const char *s = ps->p;
    if (*ps->p == '-') ps->p++;
    if (!isdigit((unsigned char)*ps->p)) { fail(ps, "некорректное число"); return NULL; }
    while (isdigit((unsigned char)*ps->p)) ps->p++;
    if (*ps->p == '.') { ps->p++; while (isdigit((unsigned char)*ps->p)) ps->p++; }
    if (*ps->p == 'e' || *ps->p == 'E') {
        ps->p++;
        if (*ps->p == '+' || *ps->p == '-') ps->p++;
        while (isdigit((unsigned char)*ps->p)) ps->p++;
    }
    json *j = node_new(JSON_NUMBER);
    if (!j) { fail(ps, "нет памяти"); return NULL; }
    j->num = strtod(s, NULL);
    return j;
}

static json *parse_container(parser *ps, int depth, int is_obj) {
    json *j = node_new(is_obj ? JSON_OBJECT : JSON_ARRAY);
    if (!j) { fail(ps, "нет памяти"); return NULL; }
    ps->p++; /* { или [ */
    skip_ws(ps);
    char close = is_obj ? '}' : ']';
    if (*ps->p == close) { ps->p++; return j; }
    json **tail = &j->child;
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            key = parse_string_raw(ps);
            if (!key) break;
            skip_ws(ps);
            if (*ps->p != ':') { fail(ps, "ожидалось ':'"); free(key); break; }
            ps->p++;
        }
        json *v = parse_value(ps, depth + 1);
        if (!v) { free(key); break; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return j; }
        fail(ps, is_obj ? "ожидалось ',' или '}'" : "ожидалось ',' или ']'");
        break;
    }
    json_free(j);
    return NULL;
}

static json *parse_value(parser *ps, int depth) {
    if (depth > JSON_MAX_DEPTH) { fail(ps, "слишком глубокая вложенность"); return NULL; }
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{') return parse_container(ps, depth, 1);
    if (c == '[') return parse_container(ps, depth, 0);
    if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s) return NULL;
        json *j = node_new(JSON_STRING);
        if (!j) { free(s); fail(ps, "нет памяти"); return NULL; }
        j->str = s;
        return j;
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(ps);
    if (!strncmp(ps->p, "true", 4)) { ps->p += 4; json *j = node_new(JSON_BOOL); if (j) j->boolean = 1; return j; }
    if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return node_new(JSON_BOOL); }
    if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return node_new(JSON_NULL); }
    fail(ps, c ? "неожиданный символ" : "неожиданный конец данных");
    return NULL;
}

json *json_parse(const char *text, char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (!text) return NULL;
    parser ps = { text, text, err, errlen, 0 };
    /* BOM */
    if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB && (unsigned char)ps.p[2] == 0xBF) ps.p += 3;
    json *j = parse_value(&ps, 0);
    if (!j) return NULL;
    skip_ws(&ps);
    if (*ps.p) { fail(&ps, "лишние данные после JSON"); json_free(j); return NULL; }
    return j;
}

const json *json_get(const json *j, const char *key) {
    if (!j || j->type != JSON_OBJECT || !key) return NULL;
    for (const json *c = j->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const json *json_at(const json *j, size_t i) {
    if (!j || (j->type != JSON_ARRAY && j->type != JSON_OBJECT)) return NULL;
    for (const json *c = j->child; c; c = c->next, i--)
        if (i == 0) return c;
    return NULL;
}

size_t json_len(const json *j) {
    size_t n = 0;
    if (j && (j->type == JSON_ARRAY || j->type == JSON_OBJECT))
        for (const json *c = j->child; c; c = c->next) n++;
    return n;
}

const json *json_path(const json *j, const char *path) {
    char seg[128];
    while (j && path && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof seg) return NULL;
        memcpy(seg, path, n);
        seg[n] = '\0';
        if (j->type == JSON_ARRAY) {
            char *end;
            unsigned long idx = strtoul(seg, &end, 10);
            if (*end) return NULL;
            j = json_at(j, idx);
        } else {
            j = json_get(j, seg);
        }
        path = dot ? dot + 1 : NULL;
    }
    return j;
}

const char *json_str(const json *j, const char *def) { return (j && j->type == JSON_STRING) ? j->str : def; }
double json_num(const json *j, double def) { return (j && j->type == JSON_NUMBER) ? j->num : def; }
int json_bool(const json *j, int def) { return (j && j->type == JSON_BOOL) ? j->boolean : def; }
