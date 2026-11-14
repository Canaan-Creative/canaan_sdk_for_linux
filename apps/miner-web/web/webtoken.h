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

#ifndef WEBTOKEN_H
#define WEBTOKEN_H

#include <stddef.h>

#include "webauth.h"

#define WEB_TOKEN_TTL_S   (24 * 60 * 60)                     /* token lifetime, s */
#define WEB_TOKEN_MAX     1024                               /* token buffer, bytes including NUL */
#define WEB_TOKEN_KEY_LEN 32                                 /* signing key length, bytes */
#define WEB_TOKEN_KEY     "/data/factory/miner-web.tokenkey" /* kept across reboots and OTA */
#define WEB_TOKEN_KEY_TMP "/tmp/miner-web.tokenkey"          /* used when /data cannot hold it */
#define WEB_TOKEN_BOOT_ID "/proc/sys/kernel/random/boot_id"

typedef enum {
    WEB_TOKEN_OK,      /* signed for the current credentials and not expired */
    WEB_TOKEN_INVALID, /* malformed, forged, expired or for other credentials */
    WEB_TOKEN_CLOCK    /* issued before this boot, and the wall clock is not synchronized */
} web_token_result;

int              web_token_issue(const mc_webcred *c, char *out, size_t cap);
web_token_result web_token_check(const mc_webcred *c, const char *token);

#endif /* WEBTOKEN_H */
