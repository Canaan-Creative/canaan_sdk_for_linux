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

#ifndef PIPELINE_H
#define PIPELINE_H

#include <stddef.h>

#include "share.h"
#include "sv1.h"

typedef enum {
    MC_SHARE_SUBMIT = 0,   /* valid and new: submit line written */
    MC_SHARE_DUP,          /* same (job, xnonce2, ntime, nonce, version_bits): dropped */
    MC_SHARE_BAD,          /* fails validation or cannot be built: HW error */
    MC_SHARE_ABOVE_TARGET, /* valid nonce under pool difficulty: dropped, not HW */
} mc_share_result;

typedef struct {
    const char *job_id; /* submit and dedup key */
    const char *user;
    sv1_client  sv1;
    mc_dedup    dedup; /* recently seen shares */
    mc_tally    tally; /* pool accept/reject counts */
    uint32_t    dups;  /* duplicate shares dropped, count */
} mc_pipeline;

void            mc_pipeline_init(mc_pipeline *p);
void            mc_pipeline_set_job(mc_pipeline *p, const char *job_id, const char *user);
mc_share_result mc_pipeline_on_nonce(mc_pipeline *p, const char *xnonce2, const char *ntime,
        const char *nonce, const char *version_bits, char *submit_out, size_t cap);
void            mc_pipeline_on_result(mc_pipeline *p, int accepted);
void            mc_pipeline_zero_stats(mc_pipeline *p);

#endif /* PIPELINE_H */
