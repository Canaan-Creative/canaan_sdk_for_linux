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

#ifndef JWRITE_H
#define JWRITE_H

#include <stddef.h>

#define JW_MAX_DEPTH 8

typedef struct {
    char  *buf;
    size_t cap; /* buffer size, bytes */
    /* output length, may exceed cap on truncation, bytes, excluding NUL */
    size_t len;
    int    depth;
    int    first[JW_MAX_DEPTH]; /* per nesting level, 1 until the first member is written */
    int    err;                 /* 1 after a nesting overflow or underflow */
} jwriter;

void   jw_init(jwriter *w, char *buf, size_t cap);
void   jw_obj_open(jwriter *w);
void   jw_obj_close(jwriter *w);
void   jw_kv_str(jwriter *w, const char *key, const char *value);
void   jw_kv_raw(jwriter *w, const char *key, const char *raw);
void   jw_key_obj(jwriter *w, const char *key);
void   jw_key_arr(jwriter *w, const char *key);
void   jw_arr_close(jwriter *w);
size_t jw_len(const jwriter *w);
int    jw_ok(const jwriter *w);

void jw_kv_fmt(jwriter *w, const char *key, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
void jw_val_fmt(jwriter *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* JWRITE_H */
