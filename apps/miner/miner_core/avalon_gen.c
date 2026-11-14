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

#include "avalon_gen.h"
#include "hex.h"
#include "toast_work.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void mc_xnonce2_hex(uint64_t v, int size_bytes, char *out, size_t cap)
{
    if (size_bytes < 1)
        size_bytes = 4;
    if ((size_t)size_bytes * 2 + 1 > cap)
        size_bytes = (int)(cap - 1) / 2;
    int     lead = size_bytes > 8 ? size_bytes - 8 : 0;
    int     n    = size_bytes - lead;
    uint8_t be[8];
    for (int i = 0; i < n; i++)
        be[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    memset(out, '0', (size_t)lead * 2);
    hex_encode(out + lead * 2, be, (size_t)n);
}

int mc_build_asic_job(const mc_job *j, avalon_job_in *out)
{
    uint8_t hdr[80], mroot[32];
    if (mc_build_header(j, hdr, mroot) != 0)
        return -1;
    memset(out, 0, sizeof *out);
    /*
     * Each big core has 8 AB cores, one per VERSION register, all hashing the
     * same header. Give them distinct versions or the chip's same-nonce filter
     * drops seven of them (1/8 throughput): 0, the full mask, then each mask
     * bit from 15 up. The mask is the job's own, not the live one; mask 0
     * makes all slots equal. Every returned nonce is re-verified anyway.
     */
    uint32_t mask = (j->mask && *j->mask) ? (uint32_t)strtoul(j->mask, NULL, 16) : 0;
    uint32_t base = get_le32(hdr + 0) & ~mask;
    uint32_t rolls[TOAST_WORK_VERSION_WORDS];
    int      nr = 0;
    rolls[nr++] = 0;
    if (mask) {
        rolls[nr++] = mask;
        for (int b = 15; b <= 28 && nr < TOAST_WORK_VERSION_WORDS; b++)
            if (mask & (1u << b))
                rolls[nr++] = (1u << b);
    }
    while (nr < TOAST_WORK_VERSION_WORDS)
        rolls[nr++] = 0;
    for (int i = 0; i < TOAST_WORK_VERSION_WORDS; i++)
        out->version[i] = base | rolls[i];
    for (int i = 0; i < 8; i++)
        out->prev_hash[i] = get_le32(hdr + 4 + i * 4);
    for (int i = 0; i < 8; i++)
        out->merkle_root[i] = get_le32(hdr + 36 + i * 4);
    out->ntime = get_le32(hdr + 68);
    out->nbits = get_le32(hdr + 72);
    return 0;
}

int mc_gen_chip_work(const mc_asic_job *tpl, uint32_t n2, avalon_job_in *out)
{
    char x2[132];
    mc_xnonce2_hex((uint64_t)n2, tpl->xnonce2_size, x2, sizeof x2);
    mc_job j = {
        .version      = tpl->version,
        .prevhash     = tpl->prevhash,
        .coinb1       = tpl->coinb1,
        .coinb2       = tpl->coinb2,
        .merkle       = tpl->merkle,
        .nbits        = tpl->nbits,
        .ntime        = tpl->ntime,
        .xnonce1      = tpl->xnonce1,
        .xnonce2      = x2,
        .nonce        = "00000000",
        .version_bits = "",
        .mask         = tpl->mask,
    };
    return mc_build_asic_job(&j, out);
}

void mc_default_job(mc_asic_job *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->seq          = STATIC_WORK_JOB_ID;
    out->n2_base      = 0;
    out->xnonce2_size = 4;
    out->target_h     = 0x000FFFF0u;
    out->target_l     = 0x00000000u;
    snprintf(out->version, sizeof out->version, "20000000");
    snprintf(out->prevhash, sizeof out->prevhash, "%064x", 0u);
    snprintf(out->ntime, sizeof out->ntime, "5f000000");
    snprintf(out->nbits, sizeof out->nbits, "1d00ffff");
    snprintf(out->mask, sizeof out->mask, "1fffe000");
    snprintf(out->coinb1, sizeof out->coinb1, "00");
    snprintf(out->coinb2, sizeof out->coinb2, "00");
}
