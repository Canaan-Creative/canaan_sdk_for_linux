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

#include "jwrite.h"
#include <stdarg.h>
#include <stdio.h>

static void emit(jwriter *w, const char *s)
{
    for (; *s; s++) {
        if (w->len + 1 < w->cap)
            w->buf[w->len] = *s;
        w->len++;
    }
    if (w->cap)
        w->buf[w->len < w->cap ? w->len : w->cap - 1] = '\0';
}

static void emit_fmt(jwriter *w, const char *fmt, va_list ap)
{
    size_t room = w->len < w->cap ? w->cap - w->len : 0;
    int    n    = vsnprintf(room ? w->buf + w->len : NULL, room, fmt, ap);
    if (n > 0)
        w->len += (size_t)n;
}

static void emit_escaped(jwriter *w, const char *s)
{
    char tmp[2] = { 0, 0 };
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20) {
            char u[7];
            snprintf(u, sizeof u, "\\u%04x", c);
            emit(w, u);
            continue;
        }
        if (c == '"' || c == '\\')
            emit(w, "\\");
        tmp[0] = (char)c;
        emit(w, tmp);
    }
}

static void pre_member(jwriter *w)
{
    if (w->depth <= 0)
        return;
    if (w->first[w->depth - 1])
        w->first[w->depth - 1] = 0;
    else
        emit(w, ",");
}

static void push(jwriter *w)
{
    if (w->depth >= JW_MAX_DEPTH) {
        w->err = 1;
        return;
    }
    w->first[w->depth++] = 1;
}
static void pop(jwriter *w)
{
    if (w->depth <= 0) {
        w->err = 1;
        return;
    }
    w->depth--;
}

void jw_init(jwriter *w, char *buf, size_t cap)
{
    w->buf   = buf;
    w->cap   = cap;
    w->len   = 0;
    w->depth = 0;
    w->err   = 0;
    if (cap)
        buf[0] = '\0';
}

void jw_obj_open(jwriter *w)
{
    pre_member(w);
    emit(w, "{");
    push(w);
}
void jw_obj_close(jwriter *w)
{
    pop(w);
    emit(w, "}");
}

static void key(jwriter *w, const char *k)
{
    pre_member(w);
    emit(w, "\"");
    emit_escaped(w, k);
    emit(w, "\":");
}

void jw_kv_str(jwriter *w, const char *k, const char *value)
{
    key(w, k);
    emit(w, "\"");
    emit_escaped(w, value ? value : "");
    emit(w, "\"");
}

void jw_kv_raw(jwriter *w, const char *k, const char *raw)
{
    key(w, k);
    emit(w, raw ? raw : "null");
}

void jw_key_obj(jwriter *w, const char *k)
{
    key(w, k);
    emit(w, "{");
    push(w);
}

void jw_key_arr(jwriter *w, const char *k)
{
    key(w, k);
    emit(w, "[");
    push(w);
}

void jw_arr_close(jwriter *w)
{
    pop(w);
    emit(w, "]");
}

void jw_kv_fmt(jwriter *w, const char *k, const char *fmt, ...)
{
    va_list ap;
    key(w, k);
    va_start(ap, fmt);
    emit_fmt(w, fmt, ap);
    va_end(ap);
}

void jw_val_fmt(jwriter *w, const char *fmt, ...)
{
    va_list ap;
    pre_member(w);
    va_start(ap, fmt);
    emit_fmt(w, fmt, ap);
    va_end(ap);
}

size_t jw_len(const jwriter *w)
{
    return w->len;
}
int jw_ok(const jwriter *w)
{
    return !w->err && w->len < w->cap;
}
