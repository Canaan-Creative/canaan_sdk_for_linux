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

#include "driver_avalon.h"
#include "hal.h"
#include <string.h>

void avalon_build_work(const avalon_job_in *in, struct toast_work *w)
{
    /*
     * Wire layout (see toast_work.h): tail = header words 16..18 (merkle[7],
     * ntime, nbits), hdr_rev = header words 15..1 reversed. Every word,
     * version included, is byte-swapped: without the swap the chip hashes a
     * non-standard header and its nonces never verify.
     */
    for (int v = 0; v < TOAST_WORK_VERSION_WORDS; v++)
        w->version[v] = __builtin_bswap32(in->version[v]);
    memcpy(w->user_note, in->user_note, sizeof w->user_note);
    w->nonce   = 0;
    w->tail[0] = __builtin_bswap32(in->merkle_root[7]);
    w->tail[1] = __builtin_bswap32(in->ntime);
    w->tail[2] = __builtin_bswap32(in->nbits);
    for (int i = 0; i < 7; i++)
        w->hdr_rev[i] = __builtin_bswap32(in->merkle_root[6 - i]);
    for (int i = 0; i < 8; i++)
        w->hdr_rev[7 + i] = __builtin_bswap32(in->prev_hash[7 - i]);
}

void avalon_decode_nonce(const struct toast_nonce *n, avalon_nonce_out *out)
{
    out->job_id          = n->job_id;
    out->nonce2          = n->nonce2;
    out->nonce           = n->nonce;
    out->version         = n->version;
    out->asic_id         = (uint16_t)n->asic_id;
    out->miner_id        = (uint16_t)n->miner_id;
    out->ntime           = (uint8_t)n->ntime;
    out->mid_id          = (uint8_t)n->mid_id;
    out->valid           = (uint8_t)n->valid;
    out->last_job_nonce2 = 0;
}

int avalon_send_work(uint8_t channel, struct toast_work *w)
{
    const hal_asic_ops_t *ops = hal_asic();
    if (!ops || !ops->send_work)
        return HAL_ERR;
    return ops->send_work(channel, w, (uint32_t)sizeof *w);
}

int avalon_set_target(uint8_t channel, uint32_t target_h, uint32_t target_l)
{
    const hal_asic_ops_t *ops = hal_asic();
    if (!ops || !ops->set_target)
        return HAL_ERR;
    return ops->set_target(channel, target_h, target_l);
}

int avalon_collect_nonces(uint8_t channel, avalon_nonce_out *out, uint8_t max)
{
    const hal_asic_ops_t *ops = hal_asic();
    if (!ops || !ops->get_nonce)
        return -1;

    struct toast_nonce raw[TOAST_NONCE_COUNT];
    uint8_t            count = 0;
    int                rc    = ops->get_nonce(channel, raw, &count);
    if (rc != HAL_OK)
        return -1;
    if (count > TOAST_NONCE_COUNT)
        count = TOAST_NONCE_COUNT;
    int got = 0;
    for (int i = 0; i < count && got < max; i++)
        avalon_decode_nonce(&raw[i], &out[got++]);
    return got;
}
