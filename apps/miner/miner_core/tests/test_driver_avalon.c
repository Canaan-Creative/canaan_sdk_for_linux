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
#include "avalon_gen.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int m_send_work(uint8_t ch, struct toast_work *w, uint32_t len);
static int m_get_nonce(uint8_t ch, struct toast_nonce *n, uint8_t *count);
static int m_set_target(uint8_t ch, uint32_t th, uint32_t tl);

static int fails;

static uint32_t last_send_len; /* Length passed to the last send_work, bytes */
static uint8_t  last_send_ch;  /* Channel of the last send_work */
static int      send_calls;

static struct toast_nonce mock_q[8]; /* Nonces m_get_nonce() hands out */
static int      mock_n, mock_i;   /* Nonces queued in mock_q; index of the next one to hand out */
static int      mock_rc = HAL_OK; /* Return code of m_get_nonce() */
static uint32_t last_target_h, last_target_l; /* Target words of the last set_target */
static uint8_t  last_target_ch;               /* Channel of the last set_target */
static int      target_calls;
/* ASIC ops returned by hal_asic() */
static hal_asic_ops_t mock_ops = { .send_work = m_send_work,
    .get_nonce                                = m_get_nonce,
    .set_target                               = m_set_target };

static int m_send_work(uint8_t ch, struct toast_work *w, uint32_t len)
{
    (void)w;
    send_calls++;
    last_send_ch  = ch;
    last_send_len = len;
    return HAL_OK;
}
static int m_get_nonce(uint8_t ch, struct toast_nonce *n, uint8_t *count)
{
    (void)ch;
    int k = 0;
    for (; mock_i < mock_n; mock_i++)
        n[k++] = mock_q[mock_i];
    *count = (uint8_t)k;
    return mock_rc;
}
static int m_set_target(uint8_t ch, uint32_t th, uint32_t tl)
{
    target_calls++;
    last_target_ch = ch;
    last_target_h  = th;
    last_target_l  = tl;
    return HAL_OK;
}

const hal_asic_ops_t *hal_asic(void)
{
    return &mock_ops;
}

static void test_build_work(void)
{
    avalon_job_in in;
    memset(&in, 0, sizeof in);
    for (int i = 0; i < TOAST_WORK_VERSION_WORDS; i++)
        in.version[i] = 0x20000000u + (uint32_t)i;
    for (int i = 0; i < TOAST_WORK_HASH_WORDS; i++) {
        in.prev_hash[i]   = 0xAA00u + (uint32_t)i;
        in.merkle_root[i] = 0xBB00u + (uint32_t)i;
    }
    in.ntime        = 0x6a4ae548;
    in.nbits        = 0x17021a42;
    in.user_note[0] = 0xC0DE;

    struct toast_work w;
    memset(&w, 0xFF, sizeof w);
    avalon_build_work(&in, &w);

    CHECK(w.nonce == 0, "nonce search base is 0 (REG 0x20)");
    CHECK(w.tail[0] == 0x07BB0000, "tail[0] = bswap(merkle[7]) (REG 0x24)");
    CHECK(w.tail[1] == 0x48e54a6a, "tail[1] = bswap(ntime) (REG 0x28)");
    CHECK(w.tail[2] == 0x421a0217, "tail[2] = bswap(nbits) (REG 0x2c)");
    CHECK(w.version[0] == 0x00000020u && w.version[7] == 0x07000020u, "version words byteswapped");
    CHECK(w.hdr_rev[0] == 0x06BB0000 && w.hdr_rev[6] == 0x00BB0000,
            "hdr_rev[0..6] = bswap(merkle[6..0])");
    CHECK(w.hdr_rev[7] == 0x07AA0000 && w.hdr_rev[14] == 0x00AA0000,
            "hdr_rev[7..14] = bswap(prev[7..0])");
    CHECK(w.user_note[0] == 0xC0DE, "user note copied");
    CHECK(sizeof(struct toast_work) == 0x7c, "work image is 124 bytes (0x00..0x78)");
}

static void test_decode_nonce(void)
{
    struct toast_nonce n;
    memset(&n, 0, sizeof n);
    n.job_id   = 0xdeadbeef;
    n.nonce2   = 0x00000077;
    n.nonce    = 0x0f2ccd65;
    n.asic_id  = 0x155;
    n.miner_id = 0x2a;
    n.ntime    = 0x5b;
    n.mid_id   = 0x9;
    n.valid    = 0x1;

    avalon_nonce_out o;
    avalon_decode_nonce(&n, &o);
    CHECK(o.job_id == 0xdeadbeef && o.nonce == 0x0f2ccd65 && o.nonce2 == 0x77, "nonce words");
    CHECK(o.asic_id == 0x155 && o.miner_id == 0x2a, "asic/miner id");
    CHECK(o.ntime == 0x5b && o.mid_id == 0x9 && o.valid == 0x1, "ntime/mid/valid bitfields");
}

static void test_hal_seam(void)
{
    struct toast_work w;
    memset(&w, 0, sizeof w);
    send_calls = 0;
    int rc     = avalon_send_work(3, &w);
    CHECK(rc == HAL_OK, "send_work forwards to hal (HAL_OK)");
    CHECK(send_calls == 1 && last_send_ch == 3, "channel forwarded");
    CHECK(last_send_len == sizeof(struct toast_work), "sent length == work image size");

    memset(mock_q, 0, sizeof mock_q);
    mock_q[0].nonce = 0x11;
    mock_q[1].nonce = 0x22;
    mock_q[2].nonce = 0x33;
    mock_n          = 3;
    mock_i          = 0;
    avalon_nonce_out out[8];
    int              got = avalon_collect_nonces(0, out, 8);
    CHECK(got == 3, "collect_nonces drains the FIFO (3)");
    CHECK(out[0].nonce == 0x11 && out[2].nonce == 0x33, "collected nonces decoded in order");

    target_calls = 0;
    rc           = avalon_set_target(3, 0x00040000, 0x00000000);
    CHECK(rc == HAL_OK, "set_target forwards to hal (HAL_OK)");
    CHECK(target_calls == 1 && last_target_ch == 3, "set_target channel forwarded");
    CHECK(last_target_h == 0x00040000 && last_target_l == 0, "target words forwarded");
}

static void test_collect_full(void)
{
    memset(mock_q, 0, sizeof mock_q);
    for (int i = 0; i < 8; i++)
        mock_q[i].nonce = 0xA0u + (uint32_t)i;
    avalon_nonce_out out[8];

    mock_n  = 8;
    mock_i  = 0;
    mock_rc = HAL_OK;
    int got = avalon_collect_nonces(0, out, 8);
    CHECK(got == 8, "a full read keeps all TOAST_NONCE_COUNT records");
    CHECK(out[0].nonce == 0xA0 && out[7].nonce == 0xA7, "full-read records decoded in order");

    mock_n  = 3;
    mock_i  = 0;
    mock_rc = HAL_ERR;
    got     = avalon_collect_nonces(0, out, 8);
    CHECK(got == -1, "a hard read failure still discards the batch");

    mock_rc = HAL_OK;
}

static void test_static_versions(void)
{
    static const uint32_t ref_vmask[TOAST_WORK_VERSION_WORDS] = {
        0x00000020u,
        0x00e0ff3fu,
        0x00800020u,
        0x00000120u,
        0x00000220u,
        0x00000420u,
        0x00000820u,
        0x00001020u,
    };
    mc_asic_job       tpl;
    avalon_job_in     in;
    struct toast_work w;

    mc_default_job(&tpl);
    CHECK(tpl.seq == STATIC_WORK_JOB_ID && tpl.target_h == 0x000FFFF0u && tpl.target_l == 0,
            "the static job keeps job id 0x39240000 and target 0x000FFFF0");
    CHECK(mc_gen_chip_work(&tpl, 0, &in) == 0, "the static job builds a work");
    avalon_build_work(&in, &w);
    CHECK(memcmp(w.version, ref_vmask, sizeof ref_vmask) == 0,
            "the static work carries 8 distinct VERSION words, the static vmask set");
}

int main(void)
{
    test_build_work();
    test_static_versions();
    test_decode_nonce();
    test_hal_seam();
    test_collect_full();
    if (fails) {
        fprintf(stderr, "test_driver_avalon: %d FAIL\n", fails);
        return 1;
    }
    printf("test_driver_avalon: all pass\n");
    return 0;
}
