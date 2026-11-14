/* SPDX-License-Identifier: MIT */
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

#ifndef CORE_TST_STATS_JSON_H
#define CORE_TST_STATS_JSON_H

/*
 * Readers for the JSON estats, stats and litestats replies. A field is
 * looked up by its key name in STATS[0].MMID0Summary, in its Sysinfo and
 * Hashinfo blocks, and in every STATS[0].HBinfo.HB_<b>_Info block.
 *
 * sj_has() takes the fields in the text form, "Key[value]", so one
 * expectation reads the same against both replies: a JSON string must
 * equal the value, a JSON number must print as the value, and a JSON
 * array must hold the space-separated values in order. A trailing '%' on
 * a number is dropped. "Key[" alone asks only that the key be present.
 * Several fields in one call must be consecutive members of one block.
 */

#include "jread.h"

#include <stdio.h>
#include <string.h>

#define SJ_KEY_CAP  24  /* longest key or block name, NUL included */
#define SJ_KEYS_MAX 160 /* most members sj_keys() lists */
#define SJ_HB_MAX   16  /* most HB_<b>_Info blocks searched */

#define SJ_FN static __attribute__((unused))

/* A member found in the reply */
typedef struct {
    const char *val;               /* first byte of the raw value */
    const char *end;               /* one past its last byte */
    char        block[SJ_KEY_CAP]; /* enclosing block, "" for MMID0Summary itself */
} sj_val;

SJ_FN const char *sj_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

SJ_FN const char *sj_skip_str(const char *p)
{
    if (!p || *p != '"')
        return NULL;
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

SJ_FN const char *sj_skip(const char *p)
{
    p = sj_ws(p);
    if (!p || !*p)
        return NULL;
    if (*p == '"')
        return sj_skip_str(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p          = sj_ws(p + 1);
        if (p && *p == close)
            return p + 1;
        for (;;) {
            if (close == '}') {
                p = sj_ws(sj_skip_str(p));
                if (!p || *p != ':')
                    return NULL;
                p++;
            }
            p = sj_ws(sj_skip(p));
            if (!p)
                return NULL;
            if (*p == close)
                return p + 1;
            if (*p != ',')
                return NULL;
            p = sj_ws(p + 1);
        }
    }
    while (*p && !strchr(",]} \t\r\n", *p))
        p++;
    return p;
}

SJ_FN int sj_members(const char *obj, char keys[][SJ_KEY_CAP], const char **vals, int max)
{
    const char *p = sj_ws(obj);
    int         n = 0;

    if (!p || *p != '{')
        return -1;
    p = sj_ws(p + 1);
    if (*p == '}')
        return 0;
    for (;;) {
        const char *ke = sj_skip_str(p);
        size_t      kl;

        if (!ke)
            return -1;
        kl = (size_t)(ke - p - 2);
        if (n < max) {
            snprintf(keys[n], SJ_KEY_CAP, "%.*s", (int)(kl < SJ_KEY_CAP ? kl : SJ_KEY_CAP - 1),
                    p + 1);
            if (vals)
                vals[n] = NULL;
        }
        p = sj_ws(ke);
        if (*p != ':')
            return -1;
        p = sj_ws(p + 1);
        if (n < max && vals)
            vals[n] = p;
        p = sj_ws(sj_skip(p));
        if (!p)
            return -1;
        n++;
        if (*p == '}')
            return n;
        if (*p != ',')
            return -1;
        p = sj_ws(p + 1);
    }
}

SJ_FN const char *sj_member(const char *obj, const char *key)
{
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    const char *vals[SJ_KEYS_MAX];
    int         n = sj_members(obj, keys, vals, SJ_KEYS_MAX);

    for (int i = 0; i < n && i < SJ_KEYS_MAX; i++)
        if (!strcmp(keys[i], key))
            return vals[i];
    return NULL;
}

SJ_FN const char *sj_record(const char *reply)
{
    const char *p = sj_member(reply, "STATS");

    p = sj_ws(p);
    if (!p || *p != '[')
        return NULL;
    p = sj_ws(p + 1);
    return *p == '{' ? p : NULL;
}

SJ_FN const char *sj_block_obj(const char *reply, const char *block)
{
    const char *rec = sj_record(reply);
    const char *sum = rec ? sj_member(rec, "MMID0Summary") : NULL;
    const char *p;

    if (!sum || *sum != '{')
        return NULL;
    if (!*block)
        return sum;
    if (!strncmp(block, "HB_", 3)) {
        p = sj_member(rec, "HBinfo");
        p = p && *p == '{' ? sj_member(p, block) : NULL;
    } else {
        p = sj_member(sum, block);
    }
    return p && *p == '{' ? p : NULL;
}

SJ_FN int sj_find(const char *reply, const char *key, sj_val *v)
{
    static const char *const fixed[] = { "", "Sysinfo", "Hashinfo" };
    char                     block[SJ_KEY_CAP];

    for (int i = 0; i < (int)(sizeof fixed / sizeof *fixed) + SJ_HB_MAX; i++) {
        const char *obj, *val;

        if (i < (int)(sizeof fixed / sizeof *fixed))
            snprintf(block, sizeof block, "%s", fixed[i]);
        else
            snprintf(block, sizeof block, "HB_%d_Info", i - (int)(sizeof fixed / sizeof *fixed));
        obj = sj_block_obj(reply, block);
        val = obj ? sj_member(obj, key) : NULL;
        if (val && (*block || (*val != '{'))) {
            v->val = val;
            v->end = sj_skip(val);
            snprintf(v->block, sizeof v->block, "%s", block);
            return v->end != NULL;
        }
    }
    return 0;
}

SJ_FN int sj_key(const char *reply, const char *key)
{
    sj_val v;

    return sj_find(reply, key, &v);
}

SJ_FN char sj_type(const char *reply, const char *key)
{
    sj_val v;

    if (!sj_find(reply, key, &v))
        return 0;
    switch (*v.val) {
    case '"':
        return 's';
    case '[':
        return 'a';
    case '{':
        return 'o';
    case 't':
    case 'f':
        return 'b';
    case 'n':
        return 'z';
    default:
        return 'n';
    }
}

SJ_FN int sj_in(const char *reply, const char *key, const char *block)
{
    sj_val v;

    return sj_find(reply, key, &v) && !strcmp(v.block, block);
}

SJ_FN int sj_array_len(const char *reply, const char *key)
{
    sj_val      v;
    const char *p;
    int         n = 0;

    if (!sj_find(reply, key, &v) || *v.val != '[')
        return -1;
    p = sj_ws(v.val + 1);
    if (*p == ']')
        return 0;
    for (;;) {
        p = sj_ws(sj_skip(p));
        if (!p)
            return -1;
        n++;
        if (*p == ']')
            return n;
        p = sj_ws(p + 1);
    }
}

SJ_FN int sj_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

SJ_FN int sj_decode(const char *p, char *out, size_t cap)
{
    size_t n = 0;

    if (*p++ != '"')
        return -1;
    for (; *p != '"'; p++) {
        unsigned char c = (unsigned char)*p;

        if (!c || c < 0x20)
            return -1;
        if (c == '\\') {
            p++;
            switch (*p) {
            case '"':
            case '\\':
            case '/':
                c = (unsigned char)*p;
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'u': {
                int u = 0;
                for (int k = 1; k <= 4; k++) {
                    int h = sj_hex(p[k]);
                    if (h < 0)
                        return -1;
                    u = (u << 4) | h;
                }
                if (u > 0xff)
                    return -1;
                c = (unsigned char)u;
                p += 4;
                break;
            }
            default:
                return -1;
            }
        }
        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return 0;
}

SJ_FN int sj_str(const char *reply, const char *key, char *out, size_t cap)
{
    sj_val v;

    out[0] = '\0';
    if (!sj_find(reply, key, &v))
        return -1;
    return sj_decode(v.val, out, cap);
}

SJ_FN size_t sj_word(const char **p, const char *end, const char **w)
{
    const char *q;

    while (*p < end && **p == ' ')
        (*p)++;
    *w = *p;
    for (q = *p; q < end && *q != ' '; q++)
        ;
    *p = q;
    if (q > *w && q[-1] == '%')
        q--;
    return (size_t)(q - *w);
}

SJ_FN int sj_match(const sj_val *v, const char *want, const char *wend)
{
    static char dec[8192];
    const char *w;
    size_t      wl;

    if (*v->val == '"') {
        return !sj_decode(v->val, dec, sizeof dec) && strlen(dec) == (size_t)(wend - want) &&
                !memcmp(dec, want, (size_t)(wend - want));
    }
    if (*v->val == '[') {
        const char *p = sj_ws(v->val + 1);
        while (*p != ']') {
            const char *e = sj_skip(p);
            if (!e || *p == '"' || *p == '[' || *p == '{')
                return 0;
            wl = sj_word(&want, wend, &w);
            if (wl != (size_t)(e - p) || memcmp(w, p, wl))
                return 0;
            p = sj_ws(e);
            if (*p == ',')
                p = sj_ws(p + 1);
        }
        return sj_word(&want, wend, &w) == 0;
    }
    if (*v->val == '{' || *v->val == 't' || *v->val == 'f' || *v->val == 'n')
        return 0;
    wl = sj_word(&want, wend, &w);
    if (wl != (size_t)(v->end - v->val) || memcmp(w, v->val, wl))
        return 0;
    return sj_word(&want, wend, &w) == 0;
}

SJ_FN int sj_has(const char *reply, const char *want)
{
    const char *p = want;
    sj_val      prev;
    int         have_prev = 0;

    for (;;) {
        char        key[SJ_KEY_CAP];
        const char *kb, *vb, *ve;
        sj_val      v;

        while (*p == ' ')
            p++;
        if (!*p)
            return have_prev;
        kb = p;
        while (*p && *p != '[')
            p++;
        if (!*p || p == kb || (size_t)(p - kb) >= sizeof key)
            return 0;
        snprintf(key, sizeof key, "%.*s", (int)(p - kb), kb);
        vb = ++p;
        ve = NULL;
        for (; *p; p++)
            if (*p == ']' && (p[1] == ' ' || !p[1])) {
                ve = p;
                break;
            }
        if (!sj_find(reply, key, &v))
            return 0;
        if (ve && !sj_match(&v, vb, ve))
            return 0;
        if (have_prev) {
            const char *q  = sj_ws(prev.end);
            size_t      kl = strlen(key);
            if (strcmp(prev.block, v.block) || *q != ',')
                return 0;
            q = sj_ws(q + 1);
            if (q[0] != '"' || strncmp(q + 1, key, kl) || q[kl + 1] != '"')
                return 0;
        }
        prev      = v;
        have_prev = 1;
        if (!ve)
            return !*p;
        p = ve + 1;
    }
}

SJ_FN int sj_keys(const char *reply, const char *block, char keys[][SJ_KEY_CAP], int max)
{
    const char *obj = sj_block_obj(reply, block);

    return obj ? sj_members(obj, keys, NULL, max) : -1;
}

SJ_FN int sj_index(const char *reply, const char *key)
{
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    sj_val      v;
    int         n;

    if (!sj_find(reply, key, &v))
        return -1;
    n = sj_members(sj_block_obj(reply, v.block), keys, NULL, SJ_KEYS_MAX);
    for (int i = 0; i < n && i < SJ_KEYS_MAX; i++)
        if (!strcmp(keys[i], key))
            return i;
    return -1;
}

SJ_FN int sj_has_prefix(const char *reply, const char *prefix)
{
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    char        block[SJ_KEY_CAP];

    for (int i = 0; i < 3 + SJ_HB_MAX; i++) {
        int n;

        if (i < 3)
            snprintf(block, sizeof block, "%s", i == 0 ? "" : i == 1 ? "Sysinfo" : "Hashinfo");
        else
            snprintf(block, sizeof block, "HB_%d_Info", i - 3);
        n = sj_keys(reply, block, keys, SJ_KEYS_MAX);
        for (int k = 0; k < n && k < SJ_KEYS_MAX; k++)
            if (!strncmp(keys[k], prefix, strlen(prefix)))
                return 1;
    }
    return 0;
}

SJ_FN int sj_ok(const char *reply)
{
    json_value       *root = json_parse(reply);
    const json_value *rec  = json_array_get(json_object_get(root, "STATS"), 0);
    int               ok   = rec && json_is(json_object_get(rec, "MMID0Summary"), JSON_OBJECT) &&
            !json_object_get(rec, "MM ID0");

    json_free(root);
    return ok && sj_skip(reply) && !*sj_ws(sj_skip(reply));
}

SJ_FN int sj_same_as_text(const char *json, const char *text)
{
    static char blocks[SJ_HB_MAX + 3][SJ_KEY_CAP];
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    const char *p = strstr(text, "MM ID0=");
    const char *end;
    int         n_text = 0, n_json = 0, nb = 0;

    if (!p || !(end = strstr(p, ",MM Count=")))
        return 0;
    p += strlen("MM ID0=");
    while (p < end) {
        char        key[SJ_KEY_CAP];
        const char *kb = p, *vb, *ve = NULL;
        sj_val      v;

        while (p < end && *p != '[')
            p++;
        if (p >= end || (size_t)(p - kb) >= sizeof key)
            return 0;
        snprintf(key, sizeof key, "%.*s", (int)(p - kb), kb);
        vb = ++p;
        for (; p < end; p++)
            if (*p == ']' && (p + 1 == end || p[1] == ' ')) {
                ve = p;
                break;
            }
        if (!ve || !sj_find(json, key, &v) || !sj_match(&v, vb, ve))
            return 0;
        n_text++;
        p = ve + 1;
        while (p < end && *p == ' ')
            p++;
    }

    snprintf(blocks[nb++], SJ_KEY_CAP, "%s", "");
    snprintf(blocks[nb++], SJ_KEY_CAP, "%s", "Sysinfo");
    snprintf(blocks[nb++], SJ_KEY_CAP, "%s", "Hashinfo");
    for (int b = 0; b < SJ_HB_MAX; b++)
        snprintf(blocks[nb++], SJ_KEY_CAP, "HB_%d_Info", b);
    for (int i = 0; i < nb; i++) {
        const char *obj = sj_block_obj(json, blocks[i]);
        const char *vals[SJ_KEYS_MAX];
        int         n = obj ? sj_members(obj, keys, vals, SJ_KEYS_MAX) : 0;

        for (int k = 0; k < n && k < SJ_KEYS_MAX; k++)
            if (*blocks[i] || *vals[k] != '{')
                n_json++;
    }
    return n_text > 0 && n_text == n_json;
}

#endif /* CORE_TST_STATS_JSON_H */
