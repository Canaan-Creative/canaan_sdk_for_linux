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

#ifndef MC_API_ADDPOOL_H
#define MC_API_ADDPOOL_H

#include "pool.h"

typedef enum {
    MC_ADDPOOL_OK = 0,  /* url,user,pass parsed                         */
    MC_ADDPOOL_MISSING, /* no payload at all -> "Missing addpool details" */
    MC_ADDPOOL_INVALID  /* fewer than two commas, empty URL or a field too long */
} mc_addpool_rc;

typedef struct {
    char url[POOL_URL_MAX];
    char user[POOL_USER_MAX];
    char pass[POOL_PASS_MAX];
} mc_addpool_req;

mc_addpool_rc mc_api_addpool_parse(const char *req, mc_addpool_req *out);

#endif /* MC_API_ADDPOOL_H */
