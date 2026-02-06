/*
 * json.c - C23 recursive descent JSON parser
 *
 * One production per RFC 8259 grammar rule. Parser state is a single cursor.
 * Douglas Crockford designed JSON as minimal data interchange.
 * This parser honors that: parse, navigate, extract, free. Nothing else.
 */

#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Internal capacity for growable arrays */
constexpr int INITIAL_CAP = 8;

/* --- Value representation --- */

typedef struct {
    char           *key;
    json_value_t   *value;
} json_pair_t;

struct json_value {
    json_type_t type;
    union {
        double       number;
        bool         boolean;
        char        *string;
        struct { json_pair_t  *pairs;    int count; int cap; } object;
        struct { json_value_t **elements; int count; int cap; } array;
    } u;
};

/* --- Helpers --- */

static json_value_t *alloc_value(json_type_t type)
{
    json_value_t *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
        ++*p;
}

/* --- Forward declarations --- */

static json_value_t *parse_value(const char **p);

/* --- String parsing (RFC 8259 section 7) --- */

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = s[i];
        if      (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
        else return -1;
    }
    return v;
}

static int utf8_encode(unsigned cp, char *out)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Parse a JSON string (opening '"' already consumed). Returns malloc'd string. */
static char *parse_string_raw(const char **p)
{
    const char *s = *p;

    /* First pass: measure length */
    size_t len = 0;
    const char *q = s;
    while (*q && *q != '"') {
        if (*q == '\\') {
            q++;
            if (*q == 'u') {
                int cp = hex4(q + 1);
                if (cp < 0) return nullptr;
                q += 5;
                /* Surrogate pair */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (q[0] != '\\' || q[1] != 'u') return nullptr;
                    int lo = hex4(q + 2);
                    if (lo < 0 || lo < 0xDC00 || lo > 0xDFFF) return nullptr;
                    unsigned full = 0x10000 + ((unsigned)(cp - 0xD800) << 10) + (unsigned)(lo - 0xDC00);
                    q += 6;
                    char tmp[4];
                    len += (size_t)utf8_encode(full, tmp);
                } else {
                    char tmp[4];
                    len += (size_t)utf8_encode((unsigned)cp, tmp);
                }
            } else {
                if (!*q) return nullptr;
                q++;
                len++;
            }
        } else {
            q++;
            len++;
        }
    }
    if (*q != '"') return nullptr;

    /* Second pass: build string */
    char *out = malloc(len + 1);
    if (!out) return nullptr;

    size_t pos = 0;
    q = s;
    while (*q != '"') {
        if (*q == '\\') {
            q++;
            switch (*q) {
            case '"':  out[pos++] = '"';  q++; break;
            case '\\': out[pos++] = '\\'; q++; break;
            case '/':  out[pos++] = '/';  q++; break;
            case 'b':  out[pos++] = '\b'; q++; break;
            case 'f':  out[pos++] = '\f'; q++; break;
            case 'n':  out[pos++] = '\n'; q++; break;
            case 'r':  out[pos++] = '\r'; q++; break;
            case 't':  out[pos++] = '\t'; q++; break;
            case 'u': {
                int cp = hex4(q + 1);
                q += 5;
                unsigned codepoint = (unsigned)cp;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    int lo = hex4(q + 2);
                    codepoint = 0x10000 + ((unsigned)(cp - 0xD800) << 10) + (unsigned)(lo - 0xDC00);
                    q += 6;
                }
                pos += (size_t)utf8_encode(codepoint, out + pos);
                break;
            }
            default: out[pos++] = *q++; break;
            }
        } else {
            out[pos++] = *q++;
        }
    }
    out[pos] = '\0';
    *p = q + 1; /* skip closing '"' */
    return out;
}

static json_value_t *parse_string_value(const char **p)
{
    if (**p != '"') return nullptr;
    ++*p; /* skip opening '"' */

    char *s = parse_string_raw(p);
    if (!s) return nullptr;

    json_value_t *v = alloc_value(JSON_STRING);
    if (!v) { free(s); return nullptr; }
    v->u.string = s;
    return v;
}

/* --- Number parsing (RFC 8259 section 6) --- */

static json_value_t *parse_number(const char **p)
{
    const char *start = *p;
    char *end = nullptr;
    double num = strtod(start, &end);

    if (end == start) return nullptr;

    json_value_t *v = alloc_value(JSON_NUMBER);
    if (!v) return nullptr;
    v->u.number = num;
    *p = end;
    return v;
}

/* --- Literal parsing (true, false, null) --- */

static json_value_t *parse_literal(const char **p)
{
    if (strncmp(*p, "true", 4) == 0) {
        json_value_t *v = alloc_value(JSON_BOOL);
        if (v) v->u.boolean = true;
        *p += 4;
        return v;
    }
    if (strncmp(*p, "false", 5) == 0) {
        json_value_t *v = alloc_value(JSON_BOOL);
        if (v) v->u.boolean = false;
        *p += 5;
        return v;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return alloc_value(JSON_NULL);
    }
    return nullptr;
}

/* --- Object parsing (RFC 8259 section 4) --- */

static json_value_t *parse_object(const char **p)
{
    if (**p != '{') return nullptr;
    ++*p;

    json_value_t *obj = alloc_value(JSON_OBJECT);
    if (!obj) return nullptr;
    obj->u.object.pairs = nullptr;
    obj->u.object.count = 0;
    obj->u.object.cap = 0;

    skip_ws(p);

    if (**p == '}') {
        ++*p;
        return obj;
    }

    for (;;) {
        skip_ws(p);

        /* Key */
        if (**p != '"') goto fail;
        ++*p;
        char *key = parse_string_raw(p);
        if (!key) goto fail;

        skip_ws(p);
        if (**p != ':') { free(key); goto fail; }
        ++*p;

        /* Value */
        skip_ws(p);
        json_value_t *val = parse_value(p);
        if (!val) { free(key); goto fail; }

        /* Grow if needed */
        if (obj->u.object.count == obj->u.object.cap) {
            int new_cap = obj->u.object.cap ? obj->u.object.cap * 2 : INITIAL_CAP;
            json_pair_t *np = realloc(obj->u.object.pairs,
                                      (size_t)new_cap * sizeof(json_pair_t));
            if (!np) { free(key); json_free(val); goto fail; }
            obj->u.object.pairs = np;
            obj->u.object.cap = new_cap;
        }

        obj->u.object.pairs[obj->u.object.count++] = (json_pair_t){ .key = key, .value = val };

        skip_ws(p);
        if (**p == ',') { ++*p; continue; }
        if (**p == '}') { ++*p; return obj; }
        goto fail;
    }

fail:
    json_free(obj);
    return nullptr;
}

/* --- Array parsing (RFC 8259 section 5) --- */

static json_value_t *parse_array(const char **p)
{
    if (**p != '[') return nullptr;
    ++*p;

    json_value_t *arr = alloc_value(JSON_ARRAY);
    if (!arr) return nullptr;
    arr->u.array.elements = nullptr;
    arr->u.array.count = 0;
    arr->u.array.cap = 0;

    skip_ws(p);

    if (**p == ']') {
        ++*p;
        return arr;
    }

    for (;;) {
        skip_ws(p);
        json_value_t *val = parse_value(p);
        if (!val) goto fail;

        /* Grow if needed */
        if (arr->u.array.count == arr->u.array.cap) {
            int new_cap = arr->u.array.cap ? arr->u.array.cap * 2 : INITIAL_CAP;
            json_value_t **ne = realloc(arr->u.array.elements,
                                        (size_t)new_cap * sizeof(json_value_t *));
            if (!ne) { json_free(val); goto fail; }
            arr->u.array.elements = ne;
            arr->u.array.cap = new_cap;
        }

        arr->u.array.elements[arr->u.array.count++] = val;

        skip_ws(p);
        if (**p == ',') { ++*p; continue; }
        if (**p == ']') { ++*p; return arr; }
        goto fail;
    }

fail:
    json_free(arr);
    return nullptr;
}

/* --- Value parsing (RFC 8259 section 3) --- */

static json_value_t *parse_value(const char **p)
{
    skip_ws(p);

    switch (**p) {
    case '"': return parse_string_value(p);
    case '{': return parse_object(p);
    case '[': return parse_array(p);
    case 't': case 'f': case 'n':
        return parse_literal(p);
    default:
        /* Must be a number: digit, minus, or decimal point */
        if (**p == '-' || (**p >= '0' && **p <= '9'))
            return parse_number(p);
        return nullptr;
    }
}

/* --- Public API --- */

json_value_t *json_parse(const char *text)
{
    if (!text) return nullptr;

    const char *p = text;
    json_value_t *root = parse_value(&p);
    if (!root) return nullptr;

    /* Verify no trailing content beyond whitespace */
    skip_ws(&p);
    if (*p != '\0') {
        json_free(root);
        return nullptr;
    }

    return root;
}

const json_value_t *json_get(const json_value_t *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key) return nullptr;

    for (int i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.pairs[i].key, key) == 0)
            return obj->u.object.pairs[i].value;
    }

    return nullptr;
}

const json_value_t *json_at(const json_value_t *arr, int index)
{
    if (!arr || arr->type != JSON_ARRAY) return nullptr;
    if (index < 0 || index >= arr->u.array.count) return nullptr;
    return arr->u.array.elements[index];
}

const json_value_t *json_path(const json_value_t *root, const char *dotpath)
{
    if (!root || !dotpath) return nullptr;

    const json_value_t *cur = root;

    /* Walk dot-separated keys */
    const char *p = dotpath;
    while (*p && cur) {
        const char *dot = strchr(p, '.');
        size_t keylen = dot ? (size_t)(dot - p) : strlen(p);

        /* Extract key segment */
        char key[256];
        if (keylen >= sizeof(key)) return nullptr;
        memcpy(key, p, keylen);
        key[keylen] = '\0';

        cur = json_get(cur, key);
        p = dot ? dot + 1 : p + keylen;
    }

    return cur;
}

json_type_t json_type(const json_value_t *val)
{
    return val ? val->type : JSON_NULL;
}

const char *json_string(const json_value_t *val)
{
    if (!val || val->type != JSON_STRING) return nullptr;
    return val->u.string;
}

double json_number(const json_value_t *val)
{
    if (!val || val->type != JSON_NUMBER) return 0.0;
    return val->u.number;
}

bool json_bool(const json_value_t *val)
{
    if (!val || val->type != JSON_BOOL) return false;
    return val->u.boolean;
}

int json_count(const json_value_t *val)
{
    if (!val) return 0;
    if (val->type == JSON_ARRAY)  return val->u.array.count;
    if (val->type == JSON_OBJECT) return val->u.object.count;
    return 0;
}

void json_free(json_value_t *val)
{
    if (!val) return;

    switch (val->type) {
    case JSON_STRING:
        free(val->u.string);
        break;
    case JSON_OBJECT:
        for (int i = 0; i < val->u.object.count; i++) {
            free(val->u.object.pairs[i].key);
            json_free(val->u.object.pairs[i].value);
        }
        free(val->u.object.pairs);
        break;
    case JSON_ARRAY:
        for (int i = 0; i < val->u.array.count; i++)
            json_free(val->u.array.elements[i]);
        free(val->u.array.elements);
        break;
    default:
        break;
    }

    free(val);
}
