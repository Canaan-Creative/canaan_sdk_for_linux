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

#ifndef WEBAUTH_H
#define WEBAUTH_H

#include "mc_paths.h"

#define MC_WEBUSER_MAX 64 /* web user name buffer size, bytes including NUL */
#define MC_WEBPASS_MAX 80 /* web password buffer size, bytes including NUL */

/* Web login credentials from systemcfg.ini */
typedef struct {
    char user[MC_WEBUSER_MAX]; /* [syscfg] webuser */
    char pass[MC_WEBPASS_MAX]; /* [syscfg] webpass, hex text */
    int  loaded;               /* 1 when user and pass were loaded */
} mc_webcred;

int mc_api_load_creds(mc_webcred *c, const char *systemcfg_ini_path);
int mc_api_check(const mc_webcred *c, const char *user, const char *pass);
int mc_api_webpass_unset(const mc_webcred *c);
int mc_api_check_cookie(const mc_webcred *c, const char *cookie_auth);

#endif /* WEBAUTH_H */
