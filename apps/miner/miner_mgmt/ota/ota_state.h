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

#ifndef OTA_STATE_H
#define OTA_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "aup.h"

/* Destination of a streamed package; any callback may be NULL */
typedef struct ota_sink {
    int (*begin)(void *ctx, const struct aup_bin *b); /* starts a bin, < 0 refuses it */
    int (*write)(void *ctx, const struct aup_bin *b, long long off, const void *data,
            size_t len);                            /* writes bin bytes at off, < 0 on failure */
    int (*end)(void *ctx, const struct aup_bin *b); /* commits a verified bin, < 0 on failure */
    void *ctx;                                      /* first argument of every callback */
    /* SW/HW gate, < 0 is an UPGRADE_* error */
    int (*check_hdr)(void *ctx, const struct aup_hdr *h);
    /* partition gate, < 0 is an UPGRADE_* error */
    int (*check_part)(void *ctx, const struct aup_hdr *h);
} ota_sink;

typedef struct ota_state ota_state_t;

size_t ota_state_size(void);
void   ota_reset(ota_state_t *st, const ota_sink *sink);
int    ota_feed(ota_state_t *st, const void *buf, int len, int8_t *percent);

#endif /* OTA_STATE_H */
