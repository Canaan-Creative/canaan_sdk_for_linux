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

#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "jread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            fails++; \
        } else { \
            printf("  ok: " __VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int fails;

/* Saved fixture row: copies of its 16 tab-separated fields; non-zero once filled */
static struct {
    char *f[16];
    int   have;
} saved;

static int split_row(char *line, char **c)
{
    int   i = 0;
    char *sp, *t = strtok_r(line, "\t", &sp);
    while (t && i < 16) {
        c[i++] = t;
        t      = strtok_r(NULL, "\t", &sp);
    }
    return i;
}
static void save_row(char **c)
{
    for (int i = 0; i < 16; i++)
        saved.f[i] = strdup(c[i]);
    saved.have = 1;
}
static void free_row(void)
{
    if (saved.have)
        for (int i = 0; i < 16; i++)
            free(saved.f[i]);
}

int main(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.shares.tsv", "rb");
    if (!f) {
        fprintf(stderr, "  FAIL: happy.shares.tsv not found\n");
        return 2;
    }

    mc_pipeline pipe;
    mc_pipeline_init(&pipe);
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     acc = 0, submits = 0, badparams = 0;

    while ((n = getline(&buf, &cap, f)) > 0) {
        char work[8192];
        if ((size_t)n >= sizeof work)
            continue;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        memcpy(work, buf, (size_t)n + 1);
        char *c[16] = { 0 };
        if (split_row(work, c) < 16 || strcmp(c[15], "1") != 0)
            continue;
        acc++;
        if (!saved.have)
            save_row(c);

        mc_pipeline_set_job(&pipe, c[0], "poolworker.001");

        char            submit[512];
        mc_share_result r =
                mc_pipeline_on_nonce(&pipe, c[9], c[10], c[11], c[12], submit, sizeof submit);
        if (r == MC_SHARE_SUBMIT) {
            submits++;
            json_value       *v = json_parse(submit);
            const json_value *p = v ? json_object_get(v, "params") : NULL;
            if (json_array_size(p) != 6 || strcmp(json_string(json_array_get(p, 1)), c[0]) != 0 ||
                    strcmp(json_string(json_array_get(p, 2)), c[9]) != 0)
                badparams++;
            json_free(v);
            mc_pipeline_on_result(&pipe, 1);
        }
    }
    free(buf);
    fclose(f);

    CHECK(acc >= 1, "accepted shares in fixture (%d)", acc);
    CHECK(submits == acc, "every accepted share -> SUBMIT (%d/%d)", submits, acc);
    CHECK(badparams == 0, "every submit is a 6-param mining.submit bound to its job/xnonce2");
    CHECK(pipe.tally.accepted == (uint32_t)acc, "a/r tally records all accepts (%u)",
            pipe.tally.accepted);

    if (saved.have) {
        char      **c = saved.f;
        mc_pipeline p2;
        mc_pipeline_init(&p2);
        mc_pipeline_set_job(&p2, c[0], "w");
        char            s[512];
        mc_share_result a = mc_pipeline_on_nonce(&p2, c[9], c[10], c[11], c[12], s, sizeof s);
        mc_share_result b = mc_pipeline_on_nonce(&p2, c[9], c[10], c[11], c[12], s, sizeof s);
        CHECK(a == MC_SHARE_SUBMIT, "saved real share -> SUBMIT");
        CHECK(b == MC_SHARE_DUP && p2.dups == 1, "identical resubmit -> DUP");

        mc_pipeline_on_result(&p2, 1);
        mc_pipeline_on_result(&p2, 0);
        p2.sv1.stale = 3;
        int next_id  = p2.sv1.next_id;
        int dedup_n  = p2.dedup.count;
        mc_pipeline_zero_stats(&p2);
        CHECK(p2.tally.accepted == 0 && p2.tally.rejected == 0 && p2.dups == 0 && p2.sv1.stale == 0,
                "zero_stats clears accepted/rejected/dups/stale (%u/%u/%u/%u)", p2.tally.accepted,
                p2.tally.rejected, p2.dups, p2.sv1.stale);
        CHECK(p2.sv1.next_id == next_id && p2.dedup.count == dedup_n && !strcmp(p2.job_id, c[0]) &&
                        !strcmp(p2.user, "w"),
                "...and keeps the submit id sequence, dedup window and active job");
        b = mc_pipeline_on_nonce(&p2, c[9], c[10], c[11], c[12], s, sizeof s);
        CHECK(b == MC_SHARE_DUP && p2.dups == 1,
                "...so a share submitted before the zero is still a DUP after it");
    }
    free_row();

    {
        mc_pipeline p3;
        mc_pipeline_init(&p3);
        mc_pipeline_set_job(&p3, "J", "w");
        CHECK(!strcmp(p3.job_id, "J") && !strcmp(p3.user, "w"), "set_job stores job_id and user");
        mc_pipeline_set_job(&p3, NULL, NULL);
        CHECK(!strcmp(p3.job_id, "") && !strcmp(p3.user, ""),
                "...and a NULL job_id or user is stored as \"\"");
    }

    if (fails) {
        fprintf(stderr, "test_pipeline: %d FAIL\n", fails);
        return 1;
    }
    printf("test_pipeline: all pass\n");
    return 0;
}
