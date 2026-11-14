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

/*
 * web/webcred — mtime-cached web credential load.
 *
 * The HTTP handler needs the web user/pass on every request. Reading and
 * parsing systemcfg.ini each time is wasteful, so this wraps
 * mc_api_load_creds() with a one-slot cache keyed on the file's mtime: the
 * common path is a single stat(), and a full parse happens only when the file
 * is first seen or rewritten (e.g. a password change). Same return contract as
 * mc_api_load_creds().
 */
#ifndef WEBCRED_H
#define WEBCRED_H

#include "webauth.h"

int  web_creds_load(const char *systemcfg_ini_path, mc_webcred *out);
int  web_auth_password(const mc_webcred *c, const char *user, const char *digest,
         const char *remote_addr);
int  web_auth_login(const mc_webcred *c, const char *proof, const char *remote_addr);
int  web_proof_guard_open(const char *remote_addr);
void web_proof_guard_fail(const char *remote_addr);

#endif /* WEBCRED_H */
