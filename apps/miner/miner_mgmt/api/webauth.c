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

#include "webauth.h"
#include "ini.h"
#include <string.h>

#define MC_AUTH_NONCE_HEX 8u  /* auth cookie nonce prefix length, hex chars */
#define MC_AUTH_PROOF_HEX 24u /* auth cookie proof length after the nonce, hex chars */

int mc_api_load_creds(mc_webcred *c, const char *path)
{
    c->loaded  = 0;
    c->user[0] = c->pass[0] = '\0';
    ini_t *ini              = ini_load(path);
    if (!ini)
        return -1;
    const char *u = ini_get(ini, "syscfg", "webuser");
    const char *p = ini_get(ini, "syscfg", "webpass");
    if (u && p && strlen(u) < MC_WEBUSER_MAX && strlen(p) < MC_WEBPASS_MAX) {
        strcpy(c->user, u);
        strcpy(c->pass, p);
        c->loaded = 1;
    }
    ini_free(ini);
    return c->loaded ? 0 : -1;
}

int mc_api_check(const mc_webcred *c, const char *user, const char *pass)
{
    if (!c->loaded || !user || !pass)
        return 0;
    if (strcmp(user, c->user) != 0)
        return 0;
    size_t la = strlen(pass), lb = strlen(c->pass);
    if (la != lb)
        return 0;
    unsigned diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned)(pass[i] ^ c->pass[i]);
    return diff == 0;
}

int mc_api_webpass_unset(const mc_webcred *c)
{
    if (!c->loaded || c->pass[0] == '\0')
        return 1;
    for (const char *p = c->pass; *p; p++)
        if (*p != '0')
            return 0;
    return 1;
}

static char lc(char x)
{
    return (x >= 'A' && x <= 'Z') ? (char)(x + 32) : x;
}

int mc_api_check_cookie(const mc_webcred *c, const char *cookie_auth)
{
    if (mc_api_webpass_unset(c))
        return 0;
    if (!cookie_auth)
        return 0;
    if (strlen(cookie_auth) < MC_AUTH_NONCE_HEX + MC_AUTH_PROOF_HEX)
        return 0;
    if (strlen(c->pass) < MC_AUTH_PROOF_HEX)
        return 0;

    const char *proof = cookie_auth + MC_AUTH_NONCE_HEX;
    unsigned    diff  = 0;
    for (unsigned i = 0; i < MC_AUTH_PROOF_HEX; i++)
        diff |= (unsigned)(lc(proof[i]) ^ lc(c->pass[i]));
    return diff == 0;
}
