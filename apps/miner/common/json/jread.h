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

#ifndef JREAD_H
#define JREAD_H

#include <stddef.h>

typedef enum {
    JSON_NULL,   /* literal null, no payload */
    JSON_BOOL,   /* true or false, payload 1 or 0 */
    JSON_NUMBER, /* numeric literal, payload double */
    JSON_STRING, /* quoted string, payload NUL-terminated text */
    JSON_ARRAY,
    JSON_OBJECT,
} json_type;

typedef struct json_value json_value;

json_value       *json_parse(const char *text);
void              json_free(json_value *v);
int               json_is(const json_value *v, json_type t);
const char       *json_string(const json_value *v);
double            json_number(const json_value *v);
long long         json_int(const json_value *v);
int               json_bool(const json_value *v);
size_t            json_array_size(const json_value *v);
const json_value *json_array_get(const json_value *v, size_t i);
const json_value *json_object_get(const json_value *v, const char *key);
const json_value *json_object_find(const json_value *v, const char *key);

#endif /* JREAD_H */
