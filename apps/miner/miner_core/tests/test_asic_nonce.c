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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"
#include "driver_avalon.h"

#include "../../tests/include/tst_check.h"

#define REC_MAX 256 /* Accepted MSG_ASIC_NONCE sends kept in the record */

#define ENUM_N         2     /* Chips enumerated on each board */
#define DRAIN_PASS_CAP 20000 /* Loop ticks after which the run stops */

#define NONCES_PER_BOARD 2 /* Nonces collect_script() returns per board, once per board */

/* Stub enumerate chip count; non-zero makes stub work generation succeed */
extern int      stub_enum_chips, stub_genwork_ok;
extern int      stub_work_n;       /* Stub avalon_send_work() calls */
extern uint8_t  stub_work_board[]; /* Board of each logged stub work send */
extern uint16_t stub_work_chip[];  /* Chip selected at each logged stub work send */
/* Version words the stub avalon_build_work() copies into every work */
extern uint32_t stub_work_version[];
extern int      stub_genwork_n;     /* Stub mc_gen_chip_work() calls */
extern int      stub_default_job_n; /* Stub mc_default_job() calls */
extern int      stub_target_n;      /* Stub avalon_set_target() calls */
/* Chip selected at the last stub avalon_set_target() per board */
extern uint16_t stub_target_chip[];
extern int      stub_collect_n; /* Stub avalon_collect_nonces() calls */
/* Supplies the stub avalon_collect_nonces() results when set */
extern int (*stub_collect_hook)(uint8_t ch, int call_n, avalon_nonce_out *out, int max);
extern int      stub_nonce_arm_n[]; /* Stub a3197s_enable_nonce_return() calls per board */
extern uint32_t stub_uart_errcnt[]; /* UART error count the stub reports per board */

static uint64_t tst_now_ms;  /* Simulated clock returned by osif_now_ms(), ms */
static int      s_loop_live; /* Non-zero once the main loop has started */
static int s_link_up = 1;    /* Non-zero lets MSG_ASIC_NONCE sends succeed; zero makes them fail */
/* MSG_ASIC_NONCE sends accepted, attempted, and refused */
static int              s_rec_n, s_send_n, s_send_fail_n;
static uint32_t         s_rec_type[REC_MAX];    /* Message type of each recorded send */
static int              s_rec_len[REC_MAX];     /* Payload length of each recorded send, bytes */
static avalon_nonce_out s_rec_item[REC_MAX][2]; /* First two nonces of each recorded send */
static int              s_rec_items[REC_MAX]; /* Nonces copied from each recorded send, at most 2 */
static asic_chip_telem  s_telem;              /* Last MSG_ASIC_TELEM payload sent */
static int              s_passes;             /* Loop ticks */
static int s_drain_by_board[8]; /* collect_script() calls that returned nonces, per board */
static int s_link_restored;     /* Non-zero once the link has been brought back up */
static int s_second_job;        /* Non-zero once the second job has been posted */

static int s_run_base = -1; /* stub_work_n when every board reached BS_RUN; -1 before */

uint64_t osif_now_ms(void)
{
    return tst_now_ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return 0;
}
osif_status_t osif_lock_create(osif_lock_t **l)
{
    if (l)
        *l = NULL;
    return OSIF_OK;
}
void osif_lock_acquire(osif_lock_t *l)
{
    (void)l;
}
void osif_lock_release(osif_lock_t *l)
{
    (void)l;
}

static void tst_loop_tick(void);
void        osif_sleep_ms(uint32_t ms)
{
    tst_now_ms += ms;
    if (s_loop_live)
        tst_loop_tick();
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    s_loop_live = 1;
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_ASIC_TELEM && data && len == (int)sizeof s_telem)
        memcpy(&s_telem, data, sizeof s_telem);
    if (type != MSG_ASIC_NONCE)
        return OSIF_OK;
    s_send_n++;
    if (!s_link_up) {
        s_send_fail_n++;
        return OSIF_ERR;
    }
    if (s_rec_n < REC_MAX) {
        s_rec_type[s_rec_n] = type;
        s_rec_len[s_rec_n]  = len;
        int items           = len / (int)sizeof(avalon_nonce_out);
        if (items > 2)
            items = 2;
        s_rec_items[s_rec_n] = items;
        if (data && items > 0)
            memcpy(s_rec_item[s_rec_n], data, (size_t)items * sizeof(avalon_nonce_out));
    }
    s_rec_n++;
    return OSIF_OK;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static avalon_nonce_out mk_nonce(uint32_t k)
{
    avalon_nonce_out n;
    memset(&n, 0, sizeof n);
    n.job_id          = 0x1000u + k;
    n.nonce2          = 0x2000u + k;
    n.last_job_nonce2 = 0x3000u + k;
    n.nonce           = 0xABCD0000u + k;
    n.version         = 0x20000000u + k;
    n.asic_id         = (uint16_t)(100u + k);
    n.miner_id        = (uint16_t)(k % 8u);
    n.ntime           = (uint8_t)k;
    n.mid_id          = (uint8_t)(k >> 8);
    n.valid           = 1;
    return n;
}

static int same_nonce(const avalon_nonce_out *a, const avalon_nonce_out *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static int collect_one(uint8_t channel, int call, avalon_nonce_out *out, int capacity)
{
    (void)channel;
    (void)call;
    if (capacity < 1)
        return 0;
    out[0] = mk_nonce(777);
    return 1;
}

static void reset_path(void)
{
    nonce_ring_init(&g_nring, g_nring_slots, sizeof g_nring_slots[0], NONCE_RING_CAP);
    s_rec_n = s_send_n = s_send_fail_n = 0;
    s_link_up                          = 1;
}

static int ring_depth(void)
{
    return (int)((g_nring.write_pos + g_nring.capacity - g_nring.read_pos) % g_nring.capacity);
}
_Static_assert(ENUM_N <= (int)MINER_MAX_CHIPS, "case 7 chain longer than the descriptor's");

static int collect_script(uint8_t ch, int call_n, avalon_nonce_out *out, int max)
{
    (void)call_n;
    if (ch >= 8 || max < NONCES_PER_BOARD)
        return 0;
    if (g_cur_job.seq != 2u)
        return 0;
    if (s_drain_by_board[ch] >= 1)
        return 0;
    s_drain_by_board[ch]++;
    for (int i = 0; i < NONCES_PER_BOARD; i++) {
        out[i]                 = mk_nonce((uint32_t)(ch * 16 + i));
        out[i].last_job_nonce2 = 0;
        out[i].miner_id        = ch;
    }
    return NONCES_PER_BOARD;
}

static void tst_loop_tick(void)
{
    if (s_run_base < 0) {
        int running = 0;
        for (int b = 0; b < (int)HBOARD_COUNT; b++)
            if (g_bd[b].stage == BS_RUN)
                running++;
        if (running == (int)HBOARD_COUNT)
            s_run_base = stub_work_n;
    }
    if (!s_second_job && s_run_base >= 0 && stub_work_n >= s_run_base + 2 * (int)HBOARD_COUNT) {
        g_job.seq     = 2;
        g_job.n2_base = 0x8800u;
        g_job_cnt++;
        s_second_job = 1;
    }
    int drained = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (s_drain_by_board[b])
            drained++;
    if (!s_link_restored && drained == (int)HBOARD_COUNT) {
        s_link_up       = 1;
        s_link_restored = 1;
    }
    if ((s_link_restored && ring_depth() == 0 && s_rec_n > 0) || ++s_passes >= DRAIN_PASS_CAP)
        g_run = 0;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, ring cap %d\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, NONCE_RING_CAP);

    reset_path();
    {
        avalon_nonce_out n = mk_nonce(1);
        nonce_ring_push(&g_nring, &n);
        asic_send_nonce();
        CHECK(s_rec_n == 1, "one nonce -> one datagram (got %d)", s_rec_n);
        CHECK(s_rec_type[0] == MSG_ASIC_NONCE, "...typed MSG_ASIC_NONCE (got %u)", s_rec_type[0]);
        CHECK(s_rec_len[0] == (int)sizeof(avalon_nonce_out),
                "...of exactly one avalon_nonce_out, %zu bytes (got %d) — the "
                "consumer parses this length",
                sizeof(avalon_nonce_out), s_rec_len[0]);
        CHECK(same_nonce(&s_rec_item[0][0], &n), "...carrying the nonce byte-for-byte");
        CHECK(ring_depth() == 0 && g_nring.discarded == 0,
                "...and nothing is left queued (%d held, %u discarded)", ring_depth(),
                g_nring.discarded);
    }

    reset_path();
    s_link_up = 0;
    {
        avalon_nonce_out n = mk_nonce(2);
        nonce_ring_push(&g_nring, &n);
        asic_send_nonce();
        CHECK(s_rec_n == 0 && s_send_fail_n == 1,
                "a nonce sent down a dead link reaches nobody (%d datagrams, %d "
                "refusals)",
                s_rec_n, s_send_fail_n);
        CHECK(ring_depth() == 1 && g_nring.discarded == 0,
                "...and is HELD for the next pass, not dropped (%d held, %u "
                "discarded)",
                ring_depth(), g_nring.discarded);
        const avalon_nonce_out *head = nonce_ring_peek(&g_nring);
        CHECK(head && same_nonce(head, &n), "...intact");
    }

    s_link_up = 1;
    {
        avalon_nonce_out held  = mk_nonce(2);
        avalon_nonce_out fresh = mk_nonce(3);
        nonce_ring_push(&g_nring, &fresh);
        asic_send_nonce();
        CHECK(s_rec_n == 2, "the recovery drains both, one datagram each (got %d)", s_rec_n);
        CHECK(s_rec_len[0] == (int)sizeof(avalon_nonce_out) &&
                        s_rec_len[1] == (int)sizeof(avalon_nonce_out),
                "...each of exactly one avalon_nonce_out, %zu bytes (got %d, %d)",
                sizeof(avalon_nonce_out), s_rec_len[0], s_rec_len[1]);
        CHECK(same_nonce(&s_rec_item[0][0], &held),
                "...the HELD one first — the backlog is older than the new find");
        CHECK(same_nonce(&s_rec_item[1][0], &fresh), "...then the new one");
        CHECK(ring_depth() == 0,
                "...and the held slot is consumed, so it cannot be submitted twice "
                "(%d still held)",
                ring_depth());
    }

    reset_path();
    s_link_up = 0;
    {
        avalon_nonce_out a = mk_nonce(10), b = mk_nonce(11);
        nonce_ring_push(&g_nring, &a);
        asic_send_nonce();
        nonce_ring_push(&g_nring, &b);
        asic_send_nonce();
        CHECK(ring_depth() == 2 && g_nring.discarded == 0,
                "two nonces down a dead link are both held (%d held, %u discarded)", ring_depth(),
                g_nring.discarded);
        const avalon_nonce_out *head = nonce_ring_peek(&g_nring);
        CHECK(head && same_nonce(head, &a), "...oldest first");
        s_link_up = 1;
        asic_send_nonce();
        CHECK(s_rec_n == 2, "...and the flush sends both, one datagram each (got %d)", s_rec_n);
        CHECK(s_rec_len[0] == (int)sizeof(avalon_nonce_out) && same_nonce(&s_rec_item[0][0], &a) &&
                        same_nonce(&s_rec_item[1][0], &b),
                "...in order, intact");
        CHECK(ring_depth() == 0, "...leaving the ring empty (%d)", ring_depth());
    }

    reset_path();
    s_link_up = 0;
    {
        int usable = NONCE_RING_CAP - 1;
        for (int i = 0; i < usable + 5; i++) {
            avalon_nonce_out n = mk_nonce((uint32_t)(200 + i));
            nonce_ring_push(&g_nring, &n);
            asic_send_nonce();
        }
        CHECK(ring_depth() == usable, "the ring fills to its %d usable slot(s) (got %d)", usable,
                ring_depth());
        CHECK(g_nring.discarded == 5u,
                "...and the 5 that did not fit are COUNTED, not silently dropped "
                "(got %u)",
                g_nring.discarded);
        avalon_nonce_out        first = mk_nonce(200);
        const avalon_nonce_out *head  = nonce_ring_peek(&g_nring);
        CHECK(head && same_nonce(head, &first), "...with the OLDEST held nonce still at the head");
        uint32_t         was  = g_nring.discarded;
        avalon_nonce_out more = mk_nonce(999);
        nonce_ring_push(&g_nring, &more);
        asic_send_nonce();
        CHECK(g_nring.discarded == was + 1u, "...and the counter keeps climbing (%u -> %u)", was,
                g_nring.discarded);
    }

    reset_path();
    s_link_up = 0;
    {
        for (int i = 0; i < 4; i++) {
            avalon_nonce_out n = mk_nonce((uint32_t)(300 + i));
            nonce_ring_push(&g_nring, &n);
            asic_send_nonce();
        }
        s_send_n = 0;
        asic_send_nonce();
        CHECK(ring_depth() == 4 && s_rec_n == 0,
                "a flush down a dead link keeps all %d (got %d held, %d sent)", 4, ring_depth(),
                s_rec_n);
        CHECK(s_send_n == 1,
                "...and stops at the FIRST refusal rather than retrying each (%d "
                "attempt(s))",
                s_send_n);
    }

    reset_path();
    s_link_up         = 0;
    stub_enum_chips   = ENUM_N;
    stub_genwork_ok   = 1;
    stub_collect_hook = collect_script;
    stub_collect_n    = 0;
    memset(s_drain_by_board, 0, sizeof s_drain_by_board);
    memset(stub_nonce_arm_n, 0, sizeof(int) * 8);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].ri_sta = g_bd[bd].stage = 0;
    g_pll_at_target = 1;
    memset(&g_job, 0, sizeof g_job);
    g_job.seq     = 1;
    g_job.n2_base = 0x4000u;
    g_job_cnt++;
    g_last_job_nonce2 = 0;
    s_passes          = 0;
    s_run_base        = -1;
    s_link_restored   = 0;
    s_second_job      = 0;
    s_loop_live       = 0;
    g_run             = 1;
    (void)asic_miner_main_unused();
    s_loop_live       = 0;
    stub_collect_hook = NULL;

    CHECK(s_passes < DRAIN_PASS_CAP, "the loop flushed the whole backlog (%d passes, cap %d)",
            s_passes, DRAIN_PASS_CAP);
    {
        int want = (int)HBOARD_COUNT * NONCES_PER_BOARD;
        int got  = 0;
        for (int i = 0; i < s_rec_n && i < REC_MAX; i++)
            got += s_rec_items[i];
        CHECK(got == want,
                "every nonce every board found reached core_miner (%d of %d) — "
                "held across the outage, flushed on idle passes afterwards",
                got, want);
        int per[8] = { 0 };
        for (int i = 0; i < s_rec_n && i < REC_MAX; i++)
            for (int k = 0; k < s_rec_items[i]; k++) {
                int b = s_rec_item[i][k].miner_id;
                if (b >= 0 && b < 8)
                    per[b]++;
            }
        int covered = 1;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (per[b] != NONCES_PER_BOARD)
                covered = 0;
        CHECK(covered,
                "...every board's share of them (board 0 %d, last board %d, want "
                "%d each)",
                per[0], per[HBOARD_COUNT - 1], NONCES_PER_BOARD);
    }
    {
        int stamped = 0, zero = 0;
        for (int i = 0; i < s_rec_n && i < REC_MAX; i++)
            for (int k = 0; k < s_rec_items[i]; k++) {
                if (s_rec_item[i][k].last_job_nonce2 == g_last_job_nonce2)
                    stamped++;
                if (s_rec_item[i][k].last_job_nonce2 == 0)
                    zero++;
            }
        CHECK(g_last_job_nonce2 != 0,
                "the engine has a live last_job_nonce2 to stamp with (got %u) — "
                "otherwise the next assertion cannot fail",
                g_last_job_nonce2);
        CHECK(zero == 0 && stamped == (int)HBOARD_COUNT * NONCES_PER_BOARD,
                "...and every returned nonce carries it (%d stamped, %d left at "
                "0)",
                stamped, zero);
    }
    CHECK(g_nring.discarded == 0,
            "nothing was discarded across the outage (%u) — %d nonce(s) is well "
            "inside the %d-slot ring",
            g_nring.discarded, (int)HBOARD_COUNT * NONCES_PER_BOARD, NONCE_RING_CAP);

    {
        stub_genwork_ok = 1;
        stub_work_n     = 0;
        memset(&g_job, 0, sizeof g_job);
        g_job.seq     = 7;
        g_job.n2_base = 0x9100u;
        g_job_cnt++;
        memset(&g_cur_job, 0, sizeof g_cur_job);
        g_work_nonce2     = 0;
        g_last_job_nonce2 = 0;
        for (int bd = 0; bd < HBOARD_COUNT; bd++) {
            memset(g_bd[bd].cur_ver, 0, sizeof g_bd[bd].cur_ver);
            g_bd[bd].chip = 0;
        }
        g_bd[0].chip = 1;
        for (int i = 0; i < TOAST_WORK_VERSION_WORDS; i++)
            stub_work_version[i] = 0x5A000000u + (uint32_t)i;

        uart_chip_select(0, A3197S_CHIP_BROADCAST);
        asic_send_work(0);

        CHECK(stub_work_n == 1 && stub_work_board[0] == 0,
                "one work image reached the chain, on board 0's channel (%d "
                "dispatch(es))",
                stub_work_n);
        CHECK(uart_get_chip_select(0) == (uint16_t)g_bd[0].chip,
                "...ADDRESSED AT THE CHIP THE CURSOR NAMES (%u), not left at the "
                "0x%x broadcast slot the version write selects — the wire is "
                "positional, so a broadcast work send is every chip hashing the "
                "same range",
                (unsigned)g_bd[0].chip, (unsigned)A3197S_CHIP_BROADCAST);
        CHECK(memcmp(g_bd[0].cur_ver, stub_work_version, sizeof g_bd[0].cur_ver) == 0,
                "...and the VERSION regs the chain now holds are cached, so the "
                "next pass does not rewrite them");
        CHECK(g_cur_job.seq == 7u && g_work_nonce2 == 0x9101u,
                "a new job seq rebases the extranonce2 to the template's base and "
                "the dispatch spends one (seq %u, n2 %#x)",
                g_cur_job.seq, g_work_nonce2);
        CHECK(g_last_job_nonce2 == 0xFFFFFFFFu,
                "...latching one below the n2 counter it rebased from (%#x)", g_last_job_nonce2);

        asic_send_work(0);

        CHECK(stub_work_n == 2 && g_work_nonce2 == 0x9102u,
                "EVERY DISPATCH UNDER ONE JOB CARRIES ITS OWN EXTRANONCE2 (n2 "
                "%#x) — one n2 for two dispatches is two chips hashing the same "
                "range and duplicate shares",
                g_work_nonce2);
        CHECK(g_last_job_nonce2 == 0xFFFFFFFFu,
                "...and last_job_nonce2 does NOT move within a job: it is the "
                "previous JOB's last n2, not the previous dispatch's (%#x)",
                g_last_job_nonce2);

        g_job.seq     = 8;
        g_job.n2_base = 0xA200u;
        g_job_cnt++;
        asic_send_work(0);

        CHECK(g_cur_job.seq == 8u && g_work_nonce2 == 0xA201u,
                "the next job rebases again (seq %u, n2 %#x)", g_cur_job.seq, g_work_nonce2);
        CHECK(g_last_job_nonce2 == 0x9101u,
                "...and its first dispatch latches the LAST n2 job 7 dispatched "
                "(%#x) — core_miner's extranonce2 skew repair reads this off "
                "every returned nonce",
                g_last_job_nonce2);

        {
            int      rec    = stub_work_n;
            uint32_t n2     = g_work_nonce2;
            uint32_t lw     = g_bd[0].local_work;
            stub_genwork_ok = 0;
            uart_chip_select(0, A3197S_CHIP_BROADCAST);
            asic_send_work(0);
            stub_genwork_ok = 1;
            CHECK(stub_work_n == rec && g_work_nonce2 == n2 &&
                            uart_get_chip_select(0) == (uint16_t)A3197S_CHIP_BROADCAST,
                    "a template that builds no work image dispatches nothing, "
                    "spends no extranonce2 and touches no chip select (%d "
                    "dispatch(es), n2 %#x)",
                    stub_work_n - rec, g_work_nonce2);
            CHECK(g_bd[0].local_work == lw,
                    "...and LW[] does not count the work it never sent (%u, "
                    "was %u)",
                    g_bd[0].local_work, lw);

            asic_send_work(0);
            CHECK(stub_work_n == rec + 1 && g_bd[0].local_work == lw + 1u,
                    "a dispatch that reaches the chain counts once in LW[] "
                    "(%u, was %u)",
                    g_bd[0].local_work, lw);
        }

        {
            uint32_t n2 = g_work_nonce2;

            asic_send_work(0);
            CHECK(g_work_nonce2 == n2 + 1u && g_last_job_nonce2 == 0x9101u,
                    "the same job re-sent unchanged (pump keepalive) keeps "
                    "walking its extranonce2 (n2 %#x, last_job_nonce2 %#x)",
                    g_work_nonce2, g_last_job_nonce2);

            uint32_t last = g_work_nonce2 - 1u;
            snprintf(g_job.prevhash, sizeof g_job.prevhash, "%064x", 1u);
            g_job.n2_base = 0xB300u;
            g_job_cnt++;
            asic_send_work(0);
            CHECK(g_cur_job.seq == 8u && g_work_nonce2 == 0xB301u && g_last_job_nonce2 == last,
                    "a different job under the SAME seq (core_miner restarted "
                    "and its seq counter came round to the old value) rebases "
                    "the extranonce2 and latches the old job's last n2 (n2 %#x, "
                    "last_job_nonce2 %#x, want %#x)",
                    g_work_nonce2, g_last_job_nonce2, last);

            n2            = g_work_nonce2;
            g_job.seq     = 9;
            g_job.n2_base = 0xC400u;
            asic_send_work(0);
            CHECK(g_cur_job.seq == 8u && g_work_nonce2 == n2 + 1u,
                    "a template change with no new generation is not read: the "
                    "dispatch reuses the copy it holds (seq %u, n2 %#x)",
                    g_cur_job.seq, g_work_nonce2);
            g_job_cnt++;
            asic_send_work(0);
            CHECK(g_cur_job.seq == 9u && g_work_nonce2 == 0xC401u,
                    "...and the next generation is copied and rebases (seq %u, "
                    "n2 %#x)",
                    g_cur_job.seq, g_work_nonce2);
        }
    }

    {
        const int n = 7;

        stub_genwork_ok     = 1;
        stub_work_n         = 0;
        stub_genwork_n      = 0;
        stub_default_job_n  = 0;
        stub_target_n       = 0;
        stub_target_chip[0] = 0;
        g_bd[0].chips_n     = n;
        g_bd[0].chip        = 3;
        g_bd[0].local_work  = 0;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            memset(g_bd[bd].cur_ver, 0, sizeof g_bd[bd].cur_ver);
        uart_chip_select(0, A3197S_CHIP_BROADCAST);

        asic_send_default_work(0);

        CHECK(stub_default_job_n == 1 && stub_work_n == 1 && stub_genwork_n == 1,
                "the default work is ONE image sent ONCE for all %d chips (%d "
                "template, %d build(s), %d dispatch(es))",
                n, stub_default_job_n, stub_genwork_n, stub_work_n);
        CHECK(stub_work_n >= 1 && stub_work_board[0] == 0 &&
                        stub_work_chip[0] == (uint16_t)A3197S_CHIP_BROADCAST,
                "...addressed at the 0x%x broadcast slot on board 0 (board %u, "
                "slot %u)",
                (unsigned)A3197S_CHIP_BROADCAST, (unsigned)stub_work_board[0],
                (unsigned)stub_work_chip[0]);
        CHECK(stub_target_n == 1 && stub_target_chip[0] == (uint16_t)A3197S_CHIP_BROADCAST,
                "...with the target/version prologue written once under the same "
                "slot (%d target write(s), slot %u)",
                stub_target_n, (unsigned)stub_target_chip[0]);
        CHECK(uart_get_chip_select(0) == (uint16_t)g_bd[0].chip,
                "...and the sweep RESTORES the scan cursor's select (%u) on the "
                "way out, so the BS_RUN pass that follows addresses the chip it "
                "names",
                (unsigned)uart_get_chip_select(0));
        CHECK(g_bd[0].local_work == 1u,
                "...and LW[] counts the one image sent (%u), so the board is not "
                "reported idle while it is hashing",
                g_bd[0].local_work);

        stub_work_n     = 0;
        stub_genwork_ok = 0;
        uart_chip_select(0, A3197S_CHIP_BROADCAST);
        asic_send_default_work(0);
        CHECK(stub_work_n == 0 && uart_get_chip_select(0) == (uint16_t)A3197S_CHIP_BROADCAST,
                "a template that builds no work image sweeps nothing and leaves "
                "the select alone (%d dispatch(es))",
                stub_work_n);

        stub_genwork_ok = 1;
        stub_work_n     = 0;
        g_bd[0].chips_n = 0;
        asic_send_default_work(0);
        CHECK(stub_work_n == 0,
                "and a board that enumerated no chip is swept not at all (%d "
                "dispatch(es)) — not once at a broadcast slot no chip answers",
                stub_work_n);
    }
    reset_path();
    s_link_up         = 0;
    stub_collect_hook = collect_one;
    for (int count = 0; count < NONCE_RING_CAP + 1; count++)
        asic_read_nonce(0);
    asic_send_info(0);
    uint32_t lost = s_telem.nonce_discarded;
    CHECK(lost > 0,
            "real nonce collection overflow is counted in nonce_discarded (%u); "
            "it sets no RBOVERFLOW bit",
            (unsigned)lost);
    s_link_up = 1;
    asic_send_nonce();
    asic_send_info(0);
    CHECK(ring_depth() == 0 && s_telem.nonce_discarded == lost,
            "successful drain cannot erase the count of lost nonces");
    CHECK(s_telem.asic_health == ASIC_HEALTH_OK,
            "nonce loss does not masquerade as a stalled process");
    stub_collect_hook = NULL;

    uint32_t com_before[HBOARD_COUNT];
    for (int board = 0; board < HBOARD_COUNT; board++) {
        com_before[board]       = g_board_com_crc[board];
        g_bd[board].chips_n     = 1;
        g_rs.board              = (uint8_t)board;
        g_rs.chip               = 0;
        g_rs.type               = RS_ECC;
        stub_uart_errcnt[board] = board == HBOARD_COUNT - 1 ? 1 : 0;
        asic_collect_status();
    }
    asic_send_info(0);
    for (int board = 0; board < HBOARD_COUNT; board++)
        CHECK(s_telem.com_crc[board] - com_before[board] == (board == HBOARD_COUNT - 1 ? 1u : 0u),
                "UART failures stay counted per board in COMCRC on board %d; "
                "there is no COMM bit",
                board);

    return tst_done();
}
