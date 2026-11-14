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

#ifndef MC_API_SETPOOL_H
#define MC_API_SETPOOL_H

#include "pool.h"
#include <stddef.h>

#define MC_SETPOOL_CODE_ERR 132 /* API Code: setpool failed */
#define MC_SETPOOL_CODE_OK  133 /* API Code: setpool accepted */

typedef enum {
    MC_SETPOOL_OK = 0,
    MC_SETPOOL_NO_PARAMS, /* no payload                                    */
    MC_SETPOOL_URL_LONG,
    MC_SETPOOL_USER_LONG, /* pool worker name too long                     */
    MC_SETPOOL_PASS_LONG, /* pool worker password too long                 */
    MC_SETPOOL_ARGC,      /* not 1 and not 4 arguments                     */
    MC_SETPOOL_BAD_CHAR,  /* a control, '"', '|' or '\\' byte               */
    MC_SETPOOL_INDEX,     /* pool outside 0..POOL_VALID_NUM-1              */
    MC_SETPOOL_WRITE      /* pools.conf write failed (caller's)            */
} mc_setpool_rc;

typedef struct {
    int  index;
    char url[POOL_URL_MAX];
    char user[POOL_USER_MAX];
    char pass[POOL_PASS_MAX];
    int  clear; /* 1-argument form, no pool fields */
    int  argc;
} mc_setpool_req;

mc_setpool_rc mc_api_setpool_parse(const char *req, mc_setpool_req *out);
mc_setpool_rc mc_api_setpool_check(const mc_setpool_req *r);
void          mc_api_setpool_msg(mc_setpool_rc rc, const mc_setpool_req *r, char *buf, size_t cap);

#endif /* MC_API_SETPOOL_H */
