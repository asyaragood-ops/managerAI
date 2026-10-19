/* Динамическая строка + безопасная запись JSON-строк. */
#ifndef IAI_STRBUF_H
#define IAI_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char  *data; /* всегда завершается '\0' (если не NULL) */
    size_t len;
    size_t cap;
    int    oom;  /* 1 — была ошибка выделения памяти */
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_reset(strbuf *sb);
void sb_append_n(strbuf *sb, const char *s, size_t n);
void sb_append(strbuf *sb, const char *s);
void sb_appendc(strbuf *sb, char c);
void sb_printf(strbuf *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Добавить строку в кавычках с экранированием по правилам JSON. NULL -> null. */
void sb_json_str(strbuf *sb, const char *s);
/* Забрать буфер (вызывающий освобождает free()). */
char *sb_take(strbuf *sb);

/* strdup, который есть везде. */
char *xstrdup(const char *s);
/* Копирование с обрезкой (всегда завершает '\0'). */
void xstrlcpy(char *dst, const char *src, size_t size);

#endif
