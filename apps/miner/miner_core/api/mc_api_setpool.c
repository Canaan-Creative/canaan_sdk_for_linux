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

#include "mc_api_setpool.h"
#include "jread.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETPOOL_FIELDS  4    /* request fields, max count */
#define SETPOOL_PAY_MAX 4096 /* payload buffer, bytes */

static int field_ok(int i, const char *f, long *idx)
{
    if (!*f)
        return 0;
    if (i != 0)
        return 1;
    char *end;
    errno  = 0;
    long v = strtol(f, &end, 10);
    if (*end || errno || v < INT_MIN || v > INT_MAX)
        return 0;
    *idx = v;
    return 1;
}

mc_setpool_rc mc_api_setpool_parse(const char *req, mc_setpool_req *out)
{
    if (!req || !out)
        return MC_SETPOOL_NO_PARAMS;
    memset(out, 0, sizeof *out);

    char        pay[SETPOOL_PAY_MAX];
    const char *p = req;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '{') {
        if (strstr(p, "\\u0000"))
            return MC_SETPOOL_BAD_CHAR;
        json_value       *root  = json_parse(p);
        const json_value *param = json_object_get(root, "parameter");
        const char       *v     = json_is(param, JSON_STRING) ? json_string(param) : NULL;
        mc_setpool_rc     rc    = !v || !*v    ? MC_SETPOOL_NO_PARAMS :
                       strlen(v) >= sizeof pay ? MC_SETPOOL_ARGC :
                                                 MC_SETPOOL_OK;
        if (rc == MC_SETPOOL_OK)
            strcpy(pay, v);
        json_free(root);
        if (rc != MC_SETPOOL_OK)
            return rc;
    } else {
        p = strchr(p, '|');
        if (!p)
            return MC_SETPOOL_NO_PARAMS;
        p++;
        size_t n = strlen(p);
        if (n && p[n - 1] == '\n')
            n--;
        if (n && p[n - 1] == '\r')
            n--;
        if (!n)
            return MC_SETPOOL_NO_PARAMS;
        if (n >= sizeof pay)
            return MC_SETPOOL_ARGC;
        memcpy(pay, p, n);
        pay[n] = 0;
    }
    for (p = pay; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '"' || *p == '|' || *p == '\\')
            return MC_SETPOOL_BAD_CHAR;

    char *f[SETPOOL_FIELDS];
    int   fc = 0;
    for (char *s = pay;;) {
        char *c = strchr(s, ',');
        if (fc < SETPOOL_FIELDS)
            f[fc] = s;
        fc++;
        if (!c)
            break;
        *c = 0;
        s  = c + 1;
    }
    long idx = 0;
    int  n   = 0;
    while (n < fc && n < SETPOOL_FIELDS && field_ok(n, f[n], &idx))
        n++;
    out->argc = n;

    static const struct {
        int           field;
        size_t        max; /* max length, bytes */
        mc_setpool_rc rc;  /* result when exceeded */
    } lim[] = {
        { 1, POOL_URL_MAX - 1, MC_SETPOOL_URL_LONG },
        { 2, POOL_USER_MAX - 1, MC_SETPOOL_USER_LONG },
        { 3, POOL_PASS_MAX - 1, MC_SETPOOL_PASS_LONG },
    };
    for (size_t i = 0; i < sizeof lim / sizeof *lim; i++)
        if (lim[i].field < n && strlen(f[lim[i].field]) > lim[i].max)
            return lim[i].rc;

    if (fc > SETPOOL_FIELDS) {
        out->argc = fc;
        return MC_SETPOOL_ARGC;
    }
    if (n != 1 && n != SETPOOL_FIELDS)
        return MC_SETPOOL_ARGC;

    out->index = (int)idx;
    out->clear = (n == 1);
    if (!out->clear) {
        snprintf(out->url, sizeof out->url, "%s", f[1]);
        snprintf(out->user, sizeof out->user, "%s", f[2]);
        snprintf(out->pass, sizeof out->pass, "%s", f[3]);
    }
    return MC_SETPOOL_OK;
}

mc_setpool_rc mc_api_setpool_check(const mc_setpool_req *r)
{
    if (r->index < 0 || r->index >= POOL_VALID_NUM)
        return MC_SETPOOL_INDEX;
    return MC_SETPOOL_OK;
}

void mc_api_setpool_msg(mc_setpool_rc rc, const mc_setpool_req *r, char *buf, size_t cap)
{
    const char *reason = "unknown error";
    switch (rc) {
    case MC_SETPOOL_OK:
        snprintf(buf, cap,
                "Pool set info: \npool %d success set to %s\nworker is %s\n"
                "workerpassword is %s\nPlease reboot miner to make config work.\n",
                r->index, r->url, r->user, r->pass);
        return;
    case MC_SETPOOL_ARGC:
        snprintf(buf, cap,
                "Pool set error: we need 1 or 4 arguments, "
                "your input is %d arguments.",
                r->argc);
        return;
    case MC_SETPOOL_INDEX:
        snprintf(buf, cap,
                "Pool set error: pool num must between 0 and %d , "
                "your input is %d",
                POOL_VALID_NUM - 1, r->index);
        return;
    case MC_SETPOOL_NO_PARAMS:
        reason = "no params";
        break;
    case MC_SETPOOL_URL_LONG:
        reason = "The pool url length is too long.";
        break;
    case MC_SETPOOL_USER_LONG:
        reason = "The pool user length is too long.";
        break;
    case MC_SETPOOL_PASS_LONG:
        reason = "The pool pass length is too long.";
        break;
    case MC_SETPOOL_BAD_CHAR:
        reason = "invalid character in arguments";
        break;
    case MC_SETPOOL_WRITE:
        reason = "config write error";
        break;
    }
    snprintf(buf, cap, "Pool set error: %s", reason);
}
