/*
 * json.h - C23 recursive descent JSON parser
 *
 * Mirrors RFC 8259 grammar. Read-only: parse, navigate, extract, free.
 * No writer (fprintf handles our known schemas).
 * No mutation, streaming, custom allocators, or Windows compat.
 */

#ifndef JSON_H
#define JSON_H

#include <stdbool.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

/* --- Parse --- */

/* Parse a JSON text. Returns nullptr on error. Caller must json_free(). */
[[nodiscard]] json_value_t *json_parse(const char *text);

/* --- Navigate --- */

/* Get value by key from an object. Returns nullptr if not object or key missing. */
[[nodiscard]] const json_value_t *json_get(const json_value_t *obj, const char *key);

/* Get value by index from an array. Returns nullptr if not array or out of bounds. */
[[nodiscard]] const json_value_t *json_at(const json_value_t *arr, int index);

/* Navigate a dot-separated path (e.g. "properties.periods"). */
[[nodiscard]] const json_value_t *json_path(const json_value_t *root, const char *dotpath);

/* --- Extract --- */

json_type_t  json_type(const json_value_t *val);
const char  *json_string(const json_value_t *val);   /* nullptr if not string */
double       json_number(const json_value_t *val);    /* 0.0 if not number   */
bool         json_bool(const json_value_t *val);      /* false if not bool   */
int          json_count(const json_value_t *val);     /* array/object count  */

/* --- Cleanup --- */

/* Free entire tree. Safe to call with nullptr. */
void json_free(json_value_t *val);

#endif /* JSON_H */
