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

#ifndef MC_API_POOLLIST_H
#define MC_API_POOLLIST_H

#include "pool.h"

#define MC_POOLLIST_MAX (POOL_VALID_NUM + 1) /* ids kept, count */

typedef enum {
    MC_POOLLIST_OK = 0,  /* one or more ids parsed                       */
    MC_POOLLIST_MISSING, /* no payload / no ids -> "Missing pool id ..." */
} mc_poollist_rc;

typedef struct {
    int ids[MC_POOLLIST_MAX];
    int n; /* ids parsed, count */
} mc_poollist_req;

mc_poollist_rc mc_api_poollist_parse(const char *req, int is_json, mc_poollist_req *out);

#endif /* MC_API_POOLLIST_H */
