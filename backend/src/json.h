/* Небольшой JSON-парсер (дерево) для ответов провайдеров, запросов фронтенда и конфига.
 * Для записи JSON используется strbuf + sb_json_str. */
#ifndef IAI_JSON_H
#define IAI_JSON_H

#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json json;
struct json {
    json_type type;
    char     *key;    /* имя поля, если узел — член объекта */
    char     *str;    /* JSON_STRING */
    double    num;    /* JSON_NUMBER */
    int       boolean;/* JSON_BOOL */
    json     *child;  /* первый элемент массива/объекта */
    json     *next;   /* следующий сосед */
};

/* Разбор текста. При ошибке возвращает NULL и пишет описание в err (если задан). */
json *json_parse(const char *text, char *err, size_t errlen);
void  json_free(json *j);

/* Поле объекта (NULL, если нет или j не объект). */
const json *json_get(const json *j, const char *key);
/* Элемент массива по индексу. */
const json *json_at(const json *j, size_t i);
size_t json_len(const json *j);
/* Путь вида "candidates.0.content.parts" (числа — индексы массивов). */
const json *json_path(const json *j, const char *path);

/* Безопасные геттеры со значением по умолчанию. */
const char *json_str(const json *j, const char *def);
double      json_num(const json *j, double def);
int         json_bool(const json *j, int def);

#endif
