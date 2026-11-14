// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#include "jread.h"
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64
#define JSON_INT_LIMIT 9223372036854775808.0 /* 2^63, first double past long long */

typedef struct {
    const char *key;
    json_value *val;
} member;

struct json_value {
    json_type type;     /* value kind, JSON_* */
    union {             /* payload selected by type */
        int    boolean; /* JSON_BOOL, 0 or 1 */
        double number;  /* JSON_NUMBER */
        char  *string;  /* JSON_STRING, decoded, NUL-terminated */
        struct {
            json_value **items;
            size_t       n;
        } array; /* JSON_ARRAY: elements and their count */
        /* JSON_OBJECT: members in source order and their count */
        struct {
            member *members;
            size_t  n;
        } object;
    } u;
};

/* parser state: read position, success flag, current nesting depth */
typedef struct {
    const char *p;
    int         ok;
    int         depth;
} parser;

static json_value *parse_value(parser *ps);

static void skip_ws(parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static json_value *alloc_val(json_type t)
{
    json_value *v = (json_value *)calloc(1, sizeof *v);
    if (v)
        v->type = t;
    return v;
}

static int hex4(const char **sp, unsigned *code)
{
    const char *s = *sp;
    unsigned    c = 0;
    for (int i = 0; i < 4; i++) {
        char h = s[i];
        c <<= 4;
        if (h >= '0' && h <= '9')
            c |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f')
            c |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F')
            c |= (unsigned)(h - 'A' + 10);
        else
            return -1;
    }
    *sp   = s + 4;
    *code = c;
    return 0;
}

static char *parse_string_raw(parser *ps)
{
    const char *s   = ps->p;
    size_t      cap = 16, len = 0;
    char       *out = (char *)malloc(cap);
    if (!out)
        return NULL;

    while (*s && *s != '"') {
        char   enc[4];
        size_t k = 1;
        enc[0]   = *s++;
        if (enc[0] == '\\') {
            switch (*s++) {
            case '"':
                enc[0] = '"';
                break;
            case '\\':
                enc[0] = '\\';
                break;
            case '/':
                enc[0] = '/';
                break;
            case 'b':
                enc[0] = '\b';
                break;
            case 'f':
                enc[0] = '\f';
                break;
            case 'n':
                enc[0] = '\n';
                break;
            case 'r':
                enc[0] = '\r';
                break;
            case 't':
                enc[0] = '\t';
                break;
            case 'u': {
                unsigned code;
                if (hex4(&s, &code) || code == 0 || (code >= 0xDC00 && code <= 0xDFFF))
                    goto fail;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    unsigned lo;
                    if (s[0] != '\\' || s[1] != 'u')
                        goto fail;
                    s += 2;
                    if (hex4(&s, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                        goto fail;
                    code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (code < 0x80) {
                    enc[0] = (char)code;
                } else if (code < 0x800) {
                    enc[0] = (char)(0xC0 | (code >> 6));
                    enc[1] = (char)(0x80 | (code & 0x3F));
                    k      = 2;
                } else if (code < 0x10000) {
                    enc[0] = (char)(0xE0 | (code >> 12));
                    enc[1] = (char)(0x80 | ((code >> 6) & 0x3F));
                    enc[2] = (char)(0x80 | (code & 0x3F));
                    k      = 3;
                } else {
                    enc[0] = (char)(0xF0 | (code >> 18));
                    enc[1] = (char)(0x80 | ((code >> 12) & 0x3F));
                    enc[2] = (char)(0x80 | ((code >> 6) & 0x3F));
                    enc[3] = (char)(0x80 | (code & 0x3F));
                    k      = 4;
                }
                break;
            }
            default:
                goto fail;
            }
        }
        while (len + k >= cap) {
            cap *= 2;
            char *n = (char *)realloc(out, cap);
            if (!n)
                goto fail;
            out = n;
        }
        memcpy(out + len, enc, k);
        len += k;
    }
    if (*s != '"')
        goto fail;
    ps->p    = s + 1;
    out[len] = '\0';
    return out;

fail:
    free(out);
    return NULL;
}

static json_value *parse_string(parser *ps)
{
    ps->p++;
    char *str = parse_string_raw(ps);
    if (!str) {
        ps->ok = 0;
        return NULL;
    }
    json_value *v = alloc_val(JSON_STRING);
    if (!v) {
        free(str);
        ps->ok = 0;
        return NULL;
    }
    v->u.string = str;
    return v;
}

static json_value *parse_number(parser *ps)
{
    char  *end;
    double d = strtod(ps->p, &end);
    if (end == ps->p) {
        ps->ok = 0;
        return NULL;
    }
    ps->p         = end;
    json_value *v = alloc_val(JSON_NUMBER);
    if (!v) {
        ps->ok = 0;
        return NULL;
    }
    v->u.number = d;
    return v;
}

static json_value *parse_literal(parser *ps, const char *lit, json_type t, int boolval)
{
    size_t n = strlen(lit);
    if (strncmp(ps->p, lit, n) != 0) {
        ps->ok = 0;
        return NULL;
    }
    ps->p += n;
    json_value *v = alloc_val(t);
    if (!v) {
        ps->ok = 0;
        return NULL;
    }
    if (t == JSON_BOOL)
        v->u.boolean = boolval;
    return v;
}

static json_value *parse_array(parser *ps)
{
    ps->p++;
    json_value *v = alloc_val(JSON_ARRAY);
    if (!v) {
        ps->ok = 0;
        return NULL;
    }
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return v;
    }

    size_t cap = 0;
    for (;;) {
        json_value *item = parse_value(ps);
        if (!ps->ok)
            goto fail;
        if (v->u.array.n == cap) {
            cap             = cap ? cap * 2 : 4;
            json_value **ni = (json_value **)realloc(v->u.array.items, cap * sizeof *ni);
            if (!ni) {
                json_free(item);
                goto fail;
            }
            v->u.array.items = ni;
        }
        v->u.array.items[v->u.array.n++] = item;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return v;
        }
        goto fail;
    }

fail:
    ps->ok = 0;
    json_free(v);
    return NULL;
}

static json_value *parse_object(parser *ps)
{
    ps->p++;
    json_value *v = alloc_val(JSON_OBJECT);
    if (!v) {
        ps->ok = 0;
        return NULL;
    }
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return v;
    }

    size_t      cap = 0;
    char       *key = NULL;
    json_value *val = NULL;
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '"')
            goto fail;
        ps->p++;
        key = parse_string_raw(ps);
        if (!key)
            goto fail;
        skip_ws(ps);
        if (*ps->p != ':')
            goto fail;
        ps->p++;
        val = parse_value(ps);
        if (!ps->ok)
            goto fail;

        if (v->u.object.n == cap) {
            cap        = cap ? cap * 2 : 4;
            member *nm = (member *)realloc(v->u.object.members, cap * sizeof *nm);
            if (!nm)
                goto fail;
            v->u.object.members = nm;
        }
        v->u.object.members[v->u.object.n].key = key;
        v->u.object.members[v->u.object.n].val = val;
        v->u.object.n++;
        key = NULL;
        val = NULL;

        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return v;
        }
        goto fail;
    }

fail:
    free(key);
    json_free(val);
    ps->ok = 0;
    json_free(v);
    return NULL;
}

static json_value *parse_value(parser *ps)
{
    skip_ws(ps);
    switch (*ps->p) {
    case '"':
        return parse_string(ps);
    case '[':
    case '{': {
        if (ps->depth >= JSON_MAX_DEPTH) {
            ps->ok = 0;
            return NULL;
        }
        ps->depth++;
        json_value *v = (*ps->p == '[') ? parse_array(ps) : parse_object(ps);
        ps->depth--;
        return v;
    }
    case 't':
        return parse_literal(ps, "true", JSON_BOOL, 1);
    case 'f':
        return parse_literal(ps, "false", JSON_BOOL, 0);
    case 'n':
        return parse_literal(ps, "null", JSON_NULL, 0);
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
            return parse_number(ps);
        ps->ok = 0;
        return NULL;
    }
}

json_value *json_parse(const char *text)
{
    if (!text)
        return NULL;
    parser      ps = { text, 1, 0 };
    json_value *v  = parse_value(&ps);
    if (!ps.ok)
        return NULL;
    skip_ws(&ps);
    if (*ps.p != '\0') {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(json_value *v)
{
    if (!v)
        return;
    switch (v->type) {
    case JSON_STRING:
        free(v->u.string);
        break;
    case JSON_ARRAY:
        for (size_t i = 0; i < v->u.array.n; i++)
            json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < v->u.object.n; i++) {
            free((char *)v->u.object.members[i].key);
            json_free(v->u.object.members[i].val);
        }
        free(v->u.object.members);
        break;
    default:
        break;
    }
    free(v);
}

int json_is(const json_value *v, json_type t)
{
    return v && v->type == t;
}

const char *json_string(const json_value *v)
{
    return (v && v->type == JSON_STRING) ? v->u.string : "";
}
double json_number(const json_value *v)
{
    return (v && v->type == JSON_NUMBER) ? v->u.number : 0.0;
}
long long json_int(const json_value *v)
{
    if (!v || v->type != JSON_NUMBER)
        return 0;
    double d = v->u.number;
    return d >= -JSON_INT_LIMIT && d < JSON_INT_LIMIT ? (long long)d : 0;
}
int json_bool(const json_value *v)
{
    return (v && v->type == JSON_BOOL) ? v->u.boolean : 0;
}

size_t json_array_size(const json_value *v)
{
    return (v && v->type == JSON_ARRAY) ? v->u.array.n : 0;
}
const json_value *json_array_get(const json_value *v, size_t i)
{
    if (!v || v->type != JSON_ARRAY || i >= v->u.array.n)
        return NULL;
    return v->u.array.items[i];
}
const json_value *json_object_get(const json_value *v, const char *key)
{
    if (!v || v->type != JSON_OBJECT)
        return NULL;
    for (size_t i = 0; i < v->u.object.n; i++)
        if (!strcmp(v->u.object.members[i].key, key))
            return v->u.object.members[i].val;
    return NULL;
}

const json_value *json_object_find(const json_value *v, const char *key)
{
    const json_value *r = json_object_get(v, key);
    if (r || !v)
        return r;
    if (v->type == JSON_OBJECT)
        for (size_t i = 0; i < v->u.object.n && !r; i++)
            r = json_object_find(v->u.object.members[i].val, key);
    else if (v->type == JSON_ARRAY)
        for (size_t i = 0; i < v->u.array.n && !r; i++)
            r = json_object_find(v->u.array.items[i], key);
    return r;
}
