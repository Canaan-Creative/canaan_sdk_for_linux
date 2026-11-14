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

#include "ini.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *section; /* section name, "" before the first section header */
    char *key;
    char *value;
} entry;

struct ini {
    entry *e; /* entries in insertion order */
    size_t n;
    size_t cap; /* allocated entries */
};

static char *dup_range(const char *s, size_t len)
{
    char *p = (char *)malloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static char *dup_str(const char *s)
{
    return dup_range(s, strlen(s));
}

static const char *trim(const char *s, size_t len, size_t *out_len)
{
    while (len && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) {
        s++;
        len--;
    }
    while (len &&
            (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;
    *out_len = len;
    return s;
}

int ini_set(ini_t *ini, const char *section, const char *key, const char *value)
{
    for (size_t i = 0; i < ini->n; i++) {
        if (!strcmp(ini->e[i].section, section) && !strcmp(ini->e[i].key, key)) {
            char *v = dup_str(value);
            if (!v)
                return -1;
            free(ini->e[i].value);
            ini->e[i].value = v;
            return 0;
        }
    }
    if (ini->n == ini->cap) {
        size_t ncap = ini->cap ? ini->cap * 2 : 16;
        entry *ne   = (entry *)realloc(ini->e, ncap * sizeof *ne);
        if (!ne)
            return -1;
        ini->e   = ne;
        ini->cap = ncap;
    }
    entry *en   = &ini->e[ini->n];
    en->section = dup_str(section);
    en->key     = dup_str(key);
    en->value   = dup_str(value);
    if (!en->section || !en->key || !en->value) {
        free(en->section);
        free(en->key);
        free(en->value);
        return -1;
    }
    ini->n++;
    return 0;
}

const char *ini_get(const ini_t *ini, const char *section, const char *key)
{
    for (size_t i = 0; i < ini->n; i++)
        if (!strcmp(ini->e[i].section, section) && !strcmp(ini->e[i].key, key))
            return ini->e[i].value;
    return NULL;
}

int ini_fill(ini_t *dst, const ini_t *src)
{
    int added = 0;

    for (size_t i = 0; i < src->n; i++) {
        const entry *s = &src->e[i];
        const char  *v = ini_get(dst, s->section, s->key);
        if (v && (v[0] || !s->value[0]))
            continue;
        if (ini_set(dst, s->section, s->key, s->value) != 0)
            return -1;
        added++;
    }
    return added;
}

ini_t *ini_parse(const char *text)
{
    ini_t *ini = (ini_t *)calloc(1, sizeof *ini);
    if (!ini)
        return NULL;

    char        cur_section[128] = "";
    const char *p                = text;

    while (*p) {
        const char *eol      = strchr(p, '\n');
        size_t      line_len = eol ? (size_t)(eol - p) : strlen(p);

        size_t      tlen;
        const char *line = trim(p, line_len, &tlen);

        if (tlen == 0 || line[0] == '#' || line[0] == ';') {
        } else if (line[0] == '[' && line[tlen - 1] == ']') {
            size_t      slen;
            const char *s = trim(line + 1, tlen - 2, &slen);
            if (slen >= sizeof cur_section)
                slen = sizeof cur_section - 1;
            memcpy(cur_section, s, slen);
            cur_section[slen] = '\0';
        } else {
            const char *eq = memchr(line, '=', tlen);
            if (eq) {
                size_t      klen, vlen;
                const char *k = trim(line, (size_t)(eq - line), &klen);
                const char *v = trim(eq + 1, tlen - (size_t)(eq - line) - 1, &vlen);
                if (klen) {
                    char *kk = dup_range(k, klen);
                    char *vv = dup_range(v, vlen);
                    if (!kk || !vv || ini_set(ini, cur_section, kk, vv) != 0) {
                        free(kk);
                        free(vv);
                        ini_free(ini);
                        return NULL;
                    }
                    free(kk);
                    free(vv);
                }
            }
        }

        if (!eol)
            break;
        p = eol + 1;
    }
    return ini;
}

ini_t *ini_load(const char *path)
{
    FILE *f = fopen(path, "rbe");
    if (!f)
        return NULL;
    struct stat st;
    int         err = 0;
    if (fstat(fileno(f), &st) != 0)
        err = errno;
    else if (!S_ISREG(st.st_mode))
        err = EINVAL;
    else if (st.st_size > INI_MAX_BYTES)
        err = EFBIG;
    if (err) {
        fclose(f);
        errno = err;
        return NULL;
    }
    size_t sz  = (size_t)st.st_size;
    char  *buf = (char *)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        errno = ENOMEM;
        return NULL;
    }
    size_t got = fread(buf, 1, sz, f);
    int    bad = got != sz || ferror(f);
    fclose(f);
    if (bad) {
        free(buf);
        errno = EIO;
        return NULL;
    }
    buf[got]   = '\0';
    ini_t *ini = ini_parse(buf);
    free(buf);
    if (!ini)
        errno = ENOMEM;
    return ini;
}

static void emit(char *out, size_t cap, size_t *pos, const char *s)
{
    size_t len = strlen(s);
    if (*pos + 1 < cap) {
        size_t room = cap - 1 - *pos;
        memcpy(out + *pos, s, len < room ? len : room);
    }
    *pos += len;
}

size_t ini_write(const ini_t *ini, char *out, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < ini->n; i++) {
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(ini->e[j].section, ini->e[i].section)) {
                seen = 1;
                break;
            }
        if (seen)
            continue;

        const char *sec = ini->e[i].section;
        if (sec[0]) {
            emit(out, cap, &pos, "[");
            emit(out, cap, &pos, sec);
            emit(out, cap, &pos, "]\n");
        }
        for (size_t j = i; j < ini->n; j++) {
            if (strcmp(ini->e[j].section, sec))
                continue;
            emit(out, cap, &pos, ini->e[j].key);
            emit(out, cap, &pos, " = ");
            emit(out, cap, &pos, ini->e[j].value);
            emit(out, cap, &pos, "\n");
        }
        emit(out, cap, &pos, "\n");
    }
    if (cap)
        out[pos < cap ? pos : cap - 1] = '\0';
    return pos;
}

void ini_free(ini_t *ini)
{
    if (!ini)
        return;
    for (size_t i = 0; i < ini->n; i++) {
        free(ini->e[i].section);
        free(ini->e[i].key);
        free(ini->e[i].value);
    }
    free(ini->e);
    free(ini);
}
