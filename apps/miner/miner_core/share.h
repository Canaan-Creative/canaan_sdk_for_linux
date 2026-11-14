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

#ifndef SHARE_H
#define SHARE_H

#include <stdint.h>

#define MC_DEDUP_MAX 256 /* remembered shares, count */
#define MC_DEDUP_KEY 160 /* share key buffer, bytes */

typedef struct {
    char keys[MC_DEDUP_MAX][MC_DEDUP_KEY]; /* job|xnonce2|ntime|nonce|version_bits */
    int  count;                            /* stored keys, <= MC_DEDUP_MAX */
    int  head;                             /* next slot to overwrite once full */
} mc_dedup;

/* Pool share results: accepted and rejected counts. */
typedef struct {
    uint32_t accepted;
    uint32_t rejected;
} mc_tally;

void mc_dedup_init(mc_dedup *d);
int  mc_dedup_check(mc_dedup *d, const char *job_id, const char *xnonce2, const char *ntime,
         const char *nonce, const char *version_bits);
void mc_tally_init(mc_tally *t);
void mc_tally_record(mc_tally *t, int accepted);

#endif /* SHARE_H */
