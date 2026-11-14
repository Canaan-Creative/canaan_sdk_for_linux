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

#ifndef POOL_H
#define POOL_H

#define POOL_VALID_NUM 3
#define POOL_URL_MAX   257 /* capacities include the NUL */
#define POOL_USER_MAX  129 /* worker name capacity, bytes */
#define POOL_PASS_MAX  129 /* same as user; sv1 sends user+pass in one SV1_LINE_CAP line */

typedef struct {
    char url[POOL_URL_MAX];
    char user[POOL_USER_MAX]; /* worker name */
    char pass[POOL_PASS_MAX];
    int  alive; /* 0 = marked dead, 1 = not marked dead */
    int  enabled;
    int  slot; /* config slot index, 0..POOL_VALID_NUM-1 */
} pool_t;

typedef struct {
    pool_t pools[POOL_VALID_NUM]; /* pools, first count entries valid */
    int    count;
    int    active; /* index of the selected pool, -1 = none */
    int    backoff_base_ms;
    int    backoff_max_ms;
    int    backoff_ms; /* current backoff delay, ms; 0 = reset */
} pool_set;

void pool_init(pool_set *ps, int backoff_base_ms, int backoff_max_ms);
int  pool_add(pool_set *ps, const char *url, const char *user, const char *pass);
void pool_mark_dead(pool_set *ps, int idx);
void pool_mark_alive(pool_set *ps, int idx);
int  pool_backoff_next(pool_set *ps);
void pool_backoff_reset(pool_set *ps);

#endif /* POOL_H */
