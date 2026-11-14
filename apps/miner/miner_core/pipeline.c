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

#include "pipeline.h"
#include <stdlib.h>
#include <string.h>

void mc_pipeline_init(mc_pipeline *p)
{
    p->job_id = "";
    p->user   = "";
    sv1_init(&p->sv1);
    mc_dedup_init(&p->dedup);
    mc_tally_init(&p->tally);
    p->dups = 0;
}

void mc_pipeline_set_job(mc_pipeline *p, const char *job_id, const char *user)
{
    p->job_id = job_id ? job_id : "";
    p->user   = user ? user : "";
}

mc_share_result mc_pipeline_on_nonce(mc_pipeline *p, const char *xnonce2, const char *ntime,
        const char *nonce, const char *version_bits, char *submit_out, size_t cap)
{
    if (mc_dedup_check(&p->dedup, p->job_id, xnonce2, ntime, nonce, version_bits)) {
        p->dups++;
        return MC_SHARE_DUP;
    }
    char *line = sv1_build_submit(&p->sv1, p->user, p->job_id, xnonce2, ntime, nonce, version_bits);
    if (!line)
        return MC_SHARE_BAD; /* OOM: submit_out is not written */
    if (submit_out && cap) {
        strncpy(submit_out, line, cap - 1);
        submit_out[cap - 1] = '\0';
    }
    free(line);
    return MC_SHARE_SUBMIT;
}

void mc_pipeline_on_result(mc_pipeline *p, int accepted)
{
    mc_tally_record(&p->tally, accepted);
}

void mc_pipeline_zero_stats(mc_pipeline *p)
{
    mc_tally_init(&p->tally);
    p->dups      = 0;
    p->sv1.stale = 0;
}
