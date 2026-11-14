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

#include "mm_vf_harness.h"

static uint16_t vol_clamp(uint16_t v)
{
    if (v < VOL_MIN)
        return VOL_MIN;
    return v > VOL_MAX ? (uint16_t)VOL_MAX : v;
}

static uint16_t vreg_budget(uint8_t mode)
{
    cali_param cp;
    uint8_t    aged = 0;
    cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, mode, 0, &cp, &aged);
    return pout_budget(cp.max_pout);
}

static void cali_store(uint8_t mode)
{
    cali_param cp;
    uint8_t    aged = 0;
    char       buf[CALI_STR_MAX];
    cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, mode, 0, &cp, &aged);
    if (cali_format(&cp, buf, sizeof buf) <= 0 || cali_set(CALI_INI_PATH, mode, 0, buf) != 0)
        printf("  (cali_store: could not persist mode%u)\n", (unsigned)mode);
}

static void cali_clear(void)
{
    for (uint8_t m = 0; m < CALI_MODE_MAX; m++)
        cali_set(CALI_INI_PATH, m, 0, "");
}

static void vreg_restart(uint8_t mode)
{
    cali_param cp;
    cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, mode, 0, &cp, NULL);
    memset(&g_vreg, 0, sizeof g_vreg);
    g_vreg.mode = MODE_UNSET;
    g_workmode  = mode;
    g_rail_mv   = cp.volt > RAIL_MID ? (uint16_t)cp.volt : RAIL_MID;
}

#define POUT_BELOW(b) ((uint16_t)((b) + POUT_DOWN_LIMIT - 1))
#define POUT_ABOVE(b) ((uint16_t)((b) + POUT_UP_LIMIT + 1))

static int vreg_tick_at(uint16_t pout)
{
    psu_set(pout, g_rail_mv);
    g_writes = 0;
    g_now_ms += 6000;
    volt_reg_tick(&g_psu);
    return g_writes;
}

static void aging_prime(uint8_t mode)
{
    short n = ADG_STAGE_COUNT_UP + ADG_STAGE_COUNT_DOWN + 1;

    memset(&g_aging, 0, sizeof g_aging);
    g_workmode     = mode;
    g_aging.mode   = mode;
    g_aging.inited = 1;
    cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, mode, 0, &g_aging.cp, NULL);
    aging_reset(&g_aging.st, (uint16_t)INIT_FREQ);
    g_aging.st.aging_direction = -1;
    g_aging.st.enum_count      = n;
}

static void best_at(uint8_t mode, uint16_t freq, uint16_t rail_at_best)
{
    aging_prime(mode);
    g_aging.st.best_rate.hashrate_Mhz = INT_MAX;
    g_aging.st.best_rate.freq[0]      = freq;
    g_aging.st.best_rate.volt         = (short)rail_at_best;
}

static int vreg_first_move(uint16_t budget)
{
    for (int n = 1; n <= 200; n++)
        if (vreg_tick_at((uint16_t)POUT_BELOW(budget)))
            return n;
    return -1;
}

int main(void)
{
    const board_desc_t seed = {
        .hboard_count  = HBOARD_COUNT,
        .max_asic      = MAX_ASIC_ON_SINGLE_HASH,
        .fan_count     = FAN_COUNT,
        .freq_interval = FREQ_INTERVAL,
    };
    board_desc_init(&seed);
    unlink(CALI_INI_PATH); /* aging DONE persists here */
    tst_psu.set_volt = tst_set_volt;

    printf("descriptor %s: %d board(s), PSU backend INA226 + DC-DC\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT);

    g_workmode = 2;
    g_rail_mv  = RAIL_MID;
    psu_set(POUT_ABOVE(vreg_budget(2)), RAIL_MID);
    g_reads = g_writes   = 0;
    uint16_t rail_before = g_rail_mv;

    volt_reg_tick(&g_psu);

    CHECK(g_reads == 0,
            "volt_reg_tick acts on the tick's PSU sample and reads none "
            "itself (got %d)",
            g_reads);
    CHECK(g_rail_mv == (uint16_t)(rail_before - RAIL_VOLT_STEP) && g_writes == 1,
            "...and steps the rail down one volt_step (%u -> %u, want %u; "
            "%d write)",
            (unsigned)rail_before, (unsigned)g_rail_mv, (unsigned)(rail_before - RAIL_VOLT_STEP),
            g_writes);
    g_reads = g_writes = 0;
    volt_reg_tick(&g_psu);
    CHECK(g_reads == 0 && g_writes == 0, "a second tick inside 5 s is refused");
    g_now_ms += 6000;
    volt_reg_tick(NULL);
    CHECK(g_writes == 0 && g_rail_mv == (uint16_t)(rail_before - RAIL_VOLT_STEP),
            "a tick without a PSU sample does not move the rail (rail %u, "
            "%d write(s))",
            (unsigned)g_rail_mv, g_writes);

    g_rail_mv   = RAIL_MID;
    g_work_freq = WORK_FREQ;
    g_vf_sends  = 0;
    g_writes    = 0;

    {
        cali_param age_cp;
        uint8_t    age_aged = 0;
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, g_workmode, 0, &age_cp, &age_aged);
        psu_set(age_cp.max_pout, RAIL_MID);
    }
    g_rail_mv = 20;
    for (int i = 0; i < ADG_STAGE_COUNT_UP + ADG_STAGE_COUNT_DOWN + 1; i++)
        aging_tick(6000.0, &g_psu);
    CHECK(g_work_freq == WORK_FREQ && g_writes == 0,
            "the sweep itself commits neither freq nor rail (freq %u)", (unsigned)g_work_freq);
    CHECK(g_aging_bar > 0, "...but it DOES progress, so the sweep really ran (BAR %u)",
            (unsigned)g_aging_bar);

    uint16_t freq_before = g_work_freq;
    aging_tick(6000.0, &g_psu);
    CHECK(g_work_freq != freq_before,
            "the commit tick applies the champion FREQ (%u -> %u), so the "
            "branch was entered",
            (unsigned)freq_before, (unsigned)g_work_freq);
    CHECK(g_rail_mv == 20 && g_writes == 0,
            "...and the `volt_out > 0` gate refuses the negative rail that "
            "came with it: the best point recorded the rail shadow 20, not the "
            "read-back %u (rail %u, writes %d)",
            (unsigned)RAIL_MID, (unsigned)g_rail_mv, g_writes);
    CHECK((uint16_t)(short)-6 > VOL_MAX,
            "...which as a uint16_t would have clamped to VOL_MAX (%d)", (int)VOL_MAX);
    g_rail_mv = RAIL_MID;

    aging_tick(6000.0, &g_psu);
    {
        mm_periph ap;
        tst_mode = 2;
        mm_vf_snapshot(&ap);
        CHECK(ap.aging_finished == 1,
                "aging that finished in mode 2 reports mode 2 as finished "
                "(got %u)",
                (unsigned)ap.aging_finished);
        tst_mode = 0;
        mm_vf_snapshot(&ap);
        CHECK(ap.aging_finished == 0,
                "...and not mode 0, which never aged: the flag belongs to "
                "the running mode, so CALIALL/ADJ/BAR/ATA cannot credit mode 0 with "
                "mode 2's calibration (got %u)",
                (unsigned)ap.aging_finished);
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        cali_param mcp, chk;
        uint8_t    ma = 0, ca = 0;
        uint8_t    saved_mode = g_workmode;
        uint16_t   saved_freq = g_work_freq;
        uint8_t    saved_aged = g_aged_mode;

        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &mcp, &ma);
        g_workmode  = 1;
        g_work_freq = mcp.pll_start;
        g_aged_mode = MODE_UNSET;
        psu_set((uint16_t)(mcp.max_pout + POUT_UP_LIMIT + 1), 20);
        g_writes        = 0;
        g_vf_zero_sends = 0;
        for (int i = 0; i < ADG_STAGE_COUNT_UP + ADG_STAGE_COUNT_DOWN + 2; i++)
            aging_tick(6000.0, &g_psu);
        CHECK(g_vf_zero_sends == 0 && g_work_freq == mcp.pll_start && g_writes == 0,
                "a sweep with every pout above the window sends no "
                "empty best point and the work freq stays %u (got %u, %d "
                "zero send(s), %d write(s))",
                (unsigned)mcp.pll_start, (unsigned)g_work_freq, g_vf_zero_sends, g_writes);
        CHECK(g_vf_last == mcp.pll_start,
                "...and the chips are sent back to the work freq %u "
                "instead of staying on the last sweep point (last sent %u)",
                (unsigned)mcp.pll_start, (unsigned)g_vf_last);
        uint8_t bar = g_aging_bar;

        aging_tick(6000.0, &g_psu);
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &chk, &ca);
        CHECK(g_aged_mode == MODE_UNSET && g_aging_bar == bar && bar < 100 && !ca &&
                        g_work_freq == mcp.pll_start,
                "...and its DONE neither marks the mode aged, nor sets "
                "BAR 100, nor persists a record (aged %u, BAR %u, record %u, "
                "freq %u)",
                (unsigned)g_aged_mode, (unsigned)g_aging_bar, (unsigned)ca, (unsigned)g_work_freq);

        int s0 = g_vf_sends;
        for (int i = 0; i < 3; i++)
            aging_tick(6000.0, &g_psu);
        CHECK(g_vf_sends == s0,
                "...and the failed sweep is not restarted in this mode "
                "(%d send(s))",
                g_vf_sends - s0);

        int req1 = mm_vf_take_aging_failed();
        int req2 = mm_vf_take_aging_failed();
        CHECK(req1 == 1 && req2 == 0,
                "...and the failure asks the supervisor for a soft-off "
                "exactly once (%d then %d)",
                req1, req2);

        mm_vf_aging_rearm();
        s0 = g_vf_sends;
        aging_tick(6000.0, &g_psu);
        CHECK(g_vf_sends > s0, "...and a soft-on restarts the sweep (%d send(s))", g_vf_sends - s0);

        g_workmode  = saved_mode;
        g_work_freq = saved_freq;
        g_aged_mode = saved_aged;
    }

    {
        int step_ok = 1;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            if (g_vf_pts[i] != (uint16_t)(g_vf_pts[0] + i * FREQ_INTERVAL))
                step_ok = 0;
        CHECK(g_vf_sends > 0 && step_ok,
                "every MSG_ASIC_VF carries %d PLL points spaced by this "
                "descriptor's FREQ_INTERVAL=%d (got %u %u %u %u after %d send(s))",
                ASIC_PLL_POINTS, (int)FREQ_INTERVAL, (unsigned)g_vf_pts[0], (unsigned)g_vf_pts[1],
                (unsigned)g_vf_pts[2], (unsigned)g_vf_pts[3], g_vf_sends);
    }

    {
        const uint16_t base  = (uint16_t)(WORK_FREQ + 1);
        int            exact = 1;
        (void)asic_send_freq(base);
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            if (g_vf_pts[i] != (uint16_t)(base + i * FREQ_INTERVAL))
                exact = 0;
        CHECK(exact && g_vf_pts[ASIC_PLL_POINTS - 1] != g_vf_pts[0],
                "...and the ramp is pinned ABSOLUTELY against the same macro: "
                "asic_send_freq(%u) puts the base itself in freq[0] and "
                "base + i*FREQ_INTERVAL(%d) above it (got %u %u %u %u, want "
                "%u %u %u %u). The check above is relative, so it survives an "
                "offset base AND a freq_interval of 0 — the whole i=0 term carries "
                "no multiplier",
                (unsigned)base, (int)FREQ_INTERVAL, (unsigned)g_vf_pts[0], (unsigned)g_vf_pts[1],
                (unsigned)g_vf_pts[2], (unsigned)g_vf_pts[3], (unsigned)base,
                (unsigned)(base + FREQ_INTERVAL), (unsigned)(base + 2 * FREQ_INTERVAL),
                (unsigned)(base + 3 * FREQ_INTERVAL));
    }

    {
        {
            cali_param fcp;
            uint8_t    fa = 0;
            cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &fcp, &fa);
            g_aged_mode = MODE_UNSET;
            vreg_restart(1);

            g_reads = g_writes = 0;
            for (int i = 0; i < 10; i++) {
                psu_set(fcp.max_pout, g_rail_mv);
                g_now_ms += 6000;
                volt_reg_tick(&g_psu);
            }
            CHECK(g_reads == 0 && g_writes == 0 && g_rail_mv == RAIL_MID,
                    "%u in-band ticks at %u W stabilise the servo without "
                    "moving the rail (%d reads, %d writes, rail %u)",
                    10u, (unsigned)fcp.max_pout, g_reads, g_writes, (unsigned)g_rail_mv);

            psu_set(POUT_BELOW(fcp.max_pout), g_rail_mv);
            g_reads = g_writes = 0;
            g_now_ms += 6000;
            volt_reg_tick(&g_psu);
            CHECK(g_writes == 1 && g_rail_mv == (uint16_t)(RAIL_MID + RAIL_VOLT_STEP),
                    "...and the servo is now in FIT_POUT — a below-band %u W "
                    "raises the rail one volt_step (%u -> %u, %d write)",
                    (unsigned)POUT_BELOW(fcp.max_pout), (unsigned)RAIL_MID, (unsigned)g_rail_mv,
                    g_writes);

            uint16_t held = g_rail_mv;
            psu_set((uint16_t)(MAX_POUT_LIMIT_ABNORMAL + 50), held);
            g_reads = g_writes = 0;
            g_now_ms += 6000;
            volt_reg_tick(&g_psu);
            CHECK(g_reads == 0 && g_writes == 0 && g_rail_mv == held,
                    "an implausible pout reaches the guard and HOLDS the rail "
                    "(%d reads, %d writes, rail %u want %u) — without the guard the -1 "
                    "clamps to vol_min %d and is written",
                    g_reads, g_writes, (unsigned)g_rail_mv, (unsigned)held, (int)VOL_MIN);
        }
        g_workmode = 2;
    }

    mm_periph   pp;
    board_power pw_poll;

    memset(&pp, 0xAA, sizeof pp);
    mm_vf_snapshot(&pp);
    CHECK(pp.ps_valid == 0, "before any rail read the snapshot says PS[] is not publishable");

    g_psu.vout_mv    = 1908;
    g_psu.iout_ma    = 86;
    g_psu.pout_w     = 1636;
    g_psu.voutcmd_mv = 1910;
    g_psu.err        = 0x0050;
    g_psu_rc         = 0;
    (void)power_poll(&pw_poll);
    mm_vf_snapshot(&pp);
    CHECK(pp.ps_valid == 1 && pp.ps_err == 0x0050 && pp.ps_vout_mv == 1908 && pp.ps_iout_ma == 86 &&
                    pp.ps_pout_w == 1636 && pp.ps_voutcmd_mv == 1910,
            "one read copies the whole lay4 struct into the snapshot, PSU fault "
            "word included (valid=%u err=0x%04x %u/%u/%u/%u)",
            pp.ps_valid, pp.ps_err, pp.ps_vout_mv, pp.ps_iout_ma, pp.ps_pout_w, pp.ps_voutcmd_mv);

    g_psu.err = 0x0001;
    g_psu_rc  = -1;
    (void)power_poll(&pw_poll);
    mm_vf_snapshot(&pp);
    CHECK(pp.ps_err == 0x0050,
            "a failed rail read keeps the last good cache, fault word included "
            "(got 0x%04x)",
            pp.ps_err);

    g_psu.err = 0;
    g_psu_rc  = 0;
    (void)power_poll(&pw_poll);
    mm_vf_snapshot(&pp);
    CHECK(pp.ps_err == 0, "a fault-free PSU clears the word (got 0x%04x)", pp.ps_err);

    memset(&g_tel, 0, sizeof g_tel);
    g_tel.asic_live  = 1;
    g_tel.asic_chips = MAX_ASIC_ON_SINGLE_HASH;
    g_tel.in_work    = 1;
    int was_live = 1, pre_work = 0;
    tst_idle  = 1;
    int sends = g_vf_sends, writes = g_writes;
    vf_tick(&was_live, &pre_work);
    CHECK(!was_live && g_vf_sends == sends && g_writes == writes,
            "a soft-off stops the V/F policy at once although the last ASIC "
            "telemetry is still fresh (live %d, %d send(s), %d write(s))",
            was_live, g_vf_sends - sends, g_writes - writes);
    tst_idle = 0;
    vf_tick(&was_live, &pre_work);
    CHECK(was_live,
            "the same telemetry runs the policy again once the ASIC is "
            "no longer stopped");

    CHECK(pout_budget(133) == 133, "no adapter limit: the cali row is the budget");
    tst_pout = 100;
    CHECK(pout_budget(133) == 100 - PD_POWER_RESERVE_W && pout_budget(62) == 62,
            "a 100W adapter caps a 133W row at 100W less the %uW admission reserve "
            "but leaves a 62W row alone (got %u)",
            (unsigned)PD_POWER_RESERVE_W, (unsigned)pout_budget(133));
    if (PD_POWER_RESERVE_W) {
        tst_pout = PD_POWER_RESERVE_W;
        CHECK(pout_budget(62) == 0,
                "an adapter no larger than the reserve leaves a budget of 0 (got %u)",
                (unsigned)pout_budget(62));
    }
    tst_pout = 0;

    {
        mm_periph ap;
        uint8_t   saved_mode = g_workmode;
        uint16_t  saved_freq = g_work_freq;

        set_aged_mode(2);
        tst_mode = 2;
        mm_vf_snapshot(&ap);
        int on_high = ap.aging_finished;
        tst_mode    = 0;
        mm_vf_snapshot(&ap);
        CHECK(on_high == 1 && ap.aging_finished == 0,
                "a mode switch moves the flag with it, with no policy tick in "
                "between: mode 2 aged reads %d running mode 2 and %u running mode 0",
                on_high, (unsigned)ap.aging_finished);

        mm_telem_t none;
        memset(&none, 0, sizeof none);
        tst_mode = 2;
        policy_seed(&none);
        mm_vf_snapshot(&ap);
        CHECK(ap.aging_finished == 1,
                "a V/F re-seed after a telemetry gap keeps this run's aging (got %u)",
                (unsigned)ap.aging_finished);
        g_seed_respawn = 1;
        policy_seed(&none);
        mm_vf_snapshot(&ap);
        CHECK(ap.aging_finished == 1, "...and so does one after an asic_miner spawn (got %u)",
                (unsigned)ap.aging_finished);
        set_aged_mode(MODE_UNSET);
        tst_mode    = CALI_MODE_DEFAULT;
        g_workmode  = saved_mode;
        g_work_freq = saved_freq;
    }

    {
        int n;

        cali_clear();
        g_aged_mode = MODE_UNSET;
        vreg_restart(2);
        n = vreg_first_move(vreg_budget(2));
        CHECK(n == 11,
                "an uncalibrated mode holds MAX_LIMIT_ONLY for "
                "stab_thresh_normal (10) ticks, the rail first moving on tick 11 "
                "(moved on %d)",
                n);

        cali_store(2);
        vreg_restart(2);
        n = vreg_first_move(vreg_budget(2));
        CHECK(n == 121,
                "a mode with a persisted record holds it for "
                "stab_thresh_aged (120): aging_finished comes from the record, "
                "not from aging this run (moved on %d)",
                n);

        cali_clear();
        g_aged_mode = 2;
        vreg_restart(2);
        n = vreg_first_move(vreg_budget(2));
        CHECK(n == 121,
                "so does a mode that aged this run but has no record "
                "(moved on %d)",
                n);

        {
            cali_param c2;
            int        w = 0;

            cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &c2, NULL);
            vreg_restart(2);
            g_rail_mv = (uint16_t)(c2.volt - 2 * RAIL_VOLT_STEP);
            for (int i = 0; i < 5; i++)
                w += vreg_tick_at(POUT_BELOW(vreg_budget(2)));
            CHECK(w == 2 && g_rail_mv == c2.volt && g_vreg.st.stage == POUT_STAGE_MAX_LIMIT_ONLY,
                    "during MAX_LIMIT_ONLY a below-band pout raises a rail "
                    "below the mode's cali volt back to %u and no higher (rail %u, "
                    "%d write(s), stage %d)",
                    (unsigned)c2.volt, (unsigned)g_rail_mv, w, g_vreg.st.stage);
        }

        {
            cali_param c2;
            uint16_t   hb = vreg_budget(2);
#if defined(BOARD_X3_H)
            uint16_t start = 2980;
#elif defined(BOARD_NANO3S_H)
            uint16_t start = 3736;
#else
#error "no MAX_LIMIT_ONLY start rail for this board"
#endif
            int cut = 0, w = 0;

            cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &c2, NULL);
            vreg_restart(2);
            g_rail_mv = start;
            for (int i = 0; i < 2; i++)
                cut += vreg_tick_at(POUT_ABOVE(hb));
            CHECK(c2.volt < start && cut == 2 &&
                            g_rail_mv == (uint16_t)(start - 2 * RAIL_VOLT_STEP),
                    "above the window MAX_LIMIT_ONLY cuts a %u rail twice "
                    "to %u (cali volt %u, %d write(s))",
                    (unsigned)start, (unsigned)g_rail_mv, (unsigned)c2.volt, cut);
            w += vreg_tick_at(POUT_BELOW(hb));
            CHECK(w == 1 && g_rail_mv == (uint16_t)(start - RAIL_VOLT_STEP),
                    "a below-band pout then raises the rail above the cali "
                    "volt %u to %u (rail %u)",
                    (unsigned)c2.volt, (unsigned)(start - RAIL_VOLT_STEP), (unsigned)g_rail_mv);
            for (int i = 0; i < 5; i++)
                w += vreg_tick_at(POUT_BELOW(hb));
            CHECK(w == 2 && g_rail_mv == start && g_vreg.st.stage == POUT_STAGE_MAX_LIMIT_ONLY,
                    "...and back to the %u rail regulation started at, "
                    "no higher (rail %u, %d write(s), stage %d)",
                    (unsigned)start, (unsigned)g_rail_mv, w, g_vreg.st.stage);
        }

        {
            cali_param c2;
            uint16_t   hb = vreg_budget(2);
            uint16_t   start;
            int        cut, w = 0;

            cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &c2, NULL);
            start = (uint16_t)(c2.volt - 2 * RAIL_VOLT_STEP);
            vreg_restart(2);
            g_rail_mv = start;
            cut       = vreg_tick_at(POUT_ABOVE(hb));
            for (int i = 0; i < 6; i++)
                w += vreg_tick_at(POUT_BELOW(hb));
            CHECK(cut == 1 && w == 3 && g_rail_mv == c2.volt &&
                            g_vreg.st.stage == POUT_STAGE_MAX_LIMIT_ONLY,
                    "with the cali volt %u above the %u start rail, a cut "
                    "rail is raised up to the cali volt and no higher (rail %u, "
                    "%d write(s), stage %d)",
                    (unsigned)c2.volt, (unsigned)start, (unsigned)g_rail_mv, w, g_vreg.st.stage);
        }

        vreg_restart(0);
        n = vreg_first_move(vreg_budget(0));
        CHECK(n == 11,
                "...but mode 2's aging does not lengthen the dwell of mode 0, "
                "which has neither (moved on %d)",
                n);

        /* The servo is in FIT_POUT on mode 0's budget. */
        uint16_t hb = vreg_budget(2), before = g_rail_mv;
        g_workmode = 2;
        int w      = vreg_tick_at(POUT_BELOW(hb));
        CHECK(w == 1 && g_rail_mv == (uint16_t)(before + RAIL_VOLT_STEP) &&
                        g_vreg.st.max_pout == hb,
                "a runtime switch keeps the stage and takes the new "
                "mode's budget: the first below-band tick on mode 2 raises the rail "
                "(%d write, %u -> %u, budget %u want %u)",
                w, (unsigned)before, (unsigned)g_rail_mv, (unsigned)g_vreg.st.max_pout,
                (unsigned)hb);
        CHECK(g_vreg.st.start_volt == (short)before,
                "...and records the rail mode 2's regulation started at "
                "(%d want %u)",
                g_vreg.st.start_volt, (unsigned)before);

        unlink(CALI_INI_PATH);
        g_aged_mode = MODE_UNSET;
    }

    {
        const uint16_t tf = (uint16_t)WORK_FREQ;
        const uint16_t tv = (uint16_t)(RAIL_MID - RAIL_VOLT_STEP);
        int            s0;

        tst_psu.set_volt = tst_set_volt;
        mm_vf_set_ramp_target(0, 0);
        g_rail_mv   = RAIL_MID;
        g_work_freq = (uint16_t)(tf + MODE_FREQ_STEP);
        g_writes    = 0;
        g_send_fail = 1;
        s0          = g_vf_sends;
        for (int i = 0; i < 3; i++)
            (void)vf_step_toward(tf, tv);
        CHECK(g_work_freq == tf + MODE_FREQ_STEP && g_rail_mv == RAIL_MID && g_writes == 0 &&
                        g_vf_sends == s0,
                "a refused decrease leaves the work freq where it was and the "
                "rail is not lowered on any later tick (freq %u, rail %u, "
                "%d write(s))",
                (unsigned)g_work_freq, (unsigned)g_rail_mv, g_writes);

        g_send_fail = 0;
        int r       = vf_step_toward(tf, tv);
        CHECK(r == 0 && g_work_freq == tf && g_vf_last == tf && g_vf_sends == s0 + 1 &&
                        g_rail_mv == RAIL_MID && g_writes == 0,
                "...the next tick re-sends the decrease and still holds the "
                "rail (freq %u, sent %u, rail %u)",
                (unsigned)g_work_freq, (unsigned)g_vf_last, (unsigned)g_rail_mv);
        r = vf_step_toward(tf, tv);
        CHECK(r == 0 && g_rail_mv == tv && g_writes == 1,
                "...and only the tick after it lowers the rail (rail %u want "
                "%u, %d write)",
                (unsigned)g_rail_mv, (unsigned)tv, g_writes);
        CHECK(vf_step_toward(tf, tv) == 1, "...after which the pair is settled");

        g_rail_mv   = RAIL_MID;
        g_work_freq = tf;
        g_send_fail = 1;
        s0          = g_vf_sends;
        r           = vf_step_toward((uint16_t)(tf + MODE_FREQ_STEP), RAIL_MID);
        CHECK(r == 0 && g_work_freq == tf && g_vf_sends == s0,
                "a refused increase leaves the work freq unchanged (got %u)",
                (unsigned)g_work_freq);
        g_send_fail = 0;
        r           = vf_step_toward((uint16_t)(tf + MODE_FREQ_STEP), RAIL_MID);
        CHECK(r == 0 && g_work_freq == tf + MODE_FREQ_STEP && g_vf_last == tf + MODE_FREQ_STEP,
                "...and is re-sent on the next tick (got %u)", (unsigned)g_work_freq);
        tst_psu.set_volt = tst_set_volt;
    }

    {
        uint16_t b;
        uint8_t  a;

        mm_vf_set_ramp_target(0xFFFF, 1);
        ramp_get(&b, &a);
        CHECK(b == 0xFFFF && a == 1, "the ramp pair round-trips (%u, %u)", (unsigned)b,
                (unsigned)a);
        mm_vf_set_ramp_target(INIT_FREQ, 0);
        ramp_get(&b, &a);
        CHECK(b == INIT_FREQ && a == 0, "...in both fields (%u, %u)", (unsigned)b, (unsigned)a);

        const uint16_t base     = (uint16_t)WORK_FREQ;
        int            was_live = 1, pre_work = 0, s0;

        tst_idle = 0;
        memset(&g_tel, 0, sizeof g_tel);
        g_tel.asic_live  = 1;
        g_tel.asic_chips = MAX_ASIC_ON_SINGLE_HASH;
        g_tel.in_work    = 0;
        g_revert_pending = 0;
        g_ramp_at_sent   = 0;
        g_work_freq      = (uint16_t)(base - 10);
        mm_vf_set_ramp_target(base, 1);

        g_send_fail = 1;
        s0          = g_vf_sends;
        vf_tick(&was_live, &pre_work);
        CHECK(g_work_freq == base - 10 && g_vf_sends == s0,
                "pre-work: a refused final ramp step leaves the work freq "
                "(got %u)",
                (unsigned)g_work_freq);
        g_send_fail = 0;
        vf_tick(&was_live, &pre_work);
        CHECK(g_work_freq == base && g_vf_last == base && g_vf_at == 1 && g_vf_sends == s0 + 1,
                "...the next tick sends it with at_target=1 (freq %u, at %u)", (unsigned)g_vf_last,
                (unsigned)g_vf_at);
        vf_tick(&was_live, &pre_work);
        CHECK(g_vf_sends == s0 + 1, "...and once delivered it is not re-sent");

        mm_vf_set_ramp_target(base, 0);
        vf_tick(&was_live, &pre_work);
        mm_vf_set_ramp_target(base, 1);
        g_send_fail = 1;
        vf_tick(&was_live, &pre_work);
        g_send_fail = 0;
        vf_tick(&was_live, &pre_work);
        CHECK(g_vf_sends == s0 + 2 && g_vf_last == base && g_vf_at == 1,
                "at_target=1 on an unchanged base is sent, and re-sent after a "
                "refusal (%d send(s), at %u)",
                g_vf_sends - s0, (unsigned)g_vf_at);
        mm_vf_set_ramp_target(0, 0);
    }

    {
        cali_param mid, hi;
        uint8_t    ma = 0, ha = 0;
        mm_telem_t t;

        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &mid, &ma);
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &hi, &ha);
        tst_psu.set_volt = tst_set_volt;
        tst_mode         = 1;

        memset(&t, 0, sizeof t);
        for (int k = 0; k < MM_PLL_POINTS; k++) {
            t.pll_mhz[k]   = mid.pll_start + k * FREQ_INTERVAL;
            t.pll_cores[k] = 10;
        }
        int      real  = mm_telem_real_freq(&t);
        uint16_t top_v = 0;
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
            cali_param mp;

            if (!(CALI_MODE_BOARD_MASK & (1u << m)))
                continue;
            cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, m, 0, &mp, NULL);
            if (mp.volt > top_v)
                top_v = mp.volt;
        }

        g_rail_known = 0;
        g_rail_mv    = VOL_MAX;
        policy_seed(&t);
        CHECK(g_work_freq == mid.pll_start,
                "the seed is PLL point 0 (%u), not the core-weighted mean (%d), "
                "so the next send lifts no PLL point",
                (unsigned)g_work_freq, real);
        CHECK(g_rail_mv == vol_clamp(mid.volt) && g_rail_known,
                "chips running exactly on mode 1's four points seed mode 1's volt, "
                "not mode 2's %u, and a shadow no write has set (%u) is ignored "
                "(rail %u want %u)",
                (unsigned)hi.volt, (unsigned)VOL_MAX, (unsigned)g_rail_mv,
                (unsigned)vol_clamp(mid.volt));

        for (int k = 0; k < MM_PLL_POINTS; k++)
            t.pll_mhz[k] = mid.pll_start + 10 + k * FREQ_INTERVAL;
        policy_seed(&t);
        CHECK(g_rail_mv == vol_clamp(top_v),
                "chips above mode 1's top point seed the highest volt of any mode "
                "(rail %u want %u)",
                (unsigned)g_rail_mv, (unsigned)vol_clamp(top_v));

        for (int k = 0; k < MM_PLL_POINTS; k++)
            t.pll_mhz[k] = mid.pll_start + k * FREQ_INTERVAL;
        g_rail_mv = VOL_MAX;
        policy_seed(&t);
        CHECK(g_rail_mv == VOL_MAX,
                "a written rail above the seed is kept: the seed never lowers the "
                "rail (rail %u want %u)",
                (unsigned)g_rail_mv, (unsigned)VOL_MAX);

        memset(&t, 0, sizeof t);
        policy_seed(&t);
        CHECK(g_work_freq == INIT_FREQ, "with no PLL read-back the seed is INIT_FREQ (got %u)",
                (unsigned)g_work_freq);
        tst_mode         = CALI_MODE_DEFAULT;
        tst_psu.set_volt = tst_set_volt;
    }

    {
        cali_param cp;
        uint8_t    aged = 0;
        mm_telem_t none;

        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &cp, &aged);
        uint16_t tv = cp.volt;
        if (tv < VOL_MIN)
            tv = VOL_MIN;
        if (tv > VOL_MAX)
            tv = VOL_MAX;
        const uint16_t rail0 = (tv == RAIL_MID) ? (uint16_t)(RAIL_MID - RAIL_VOLT_STEP) : RAIL_MID;

        tst_psu.set_volt = NULL;
        tst_mode         = 2;
        memset(&none, 0, sizeof none);
        g_rail_mv = rail0;
        g_writes  = 0;
        policy_seed(&none);
        CHECK(g_rail_mv == rail0 && g_writes == 0,
                "set_volt==NULL: the seed writes neither the rail nor its "
                "shadow (rail %u, %d write(s))",
                (unsigned)g_rail_mv, g_writes);

        mm_vf_set_ramp_target(0, 0);
        g_work_freq = (uint16_t)(cp.pll_start - 35);
        int n       = 0;
        while (n < 50 && !(n && g_mode_settled)) {
            (void)mode_tick();
            n++;
        }
        CHECK(g_mode_settled && g_work_freq == cp.pll_start && g_writes == 0 && g_rail_mv == rail0,
                "set_volt==NULL: the mode settles on frequency alone (%d "
                "tick(s), freq %u want %u, rail %u, %d write(s))",
                n, (unsigned)g_work_freq, (unsigned)cp.pll_start, (unsigned)g_rail_mv, g_writes);

        mm_vf_set_rail((uint16_t)(rail0 + 3));
        CHECK(g_rail_mv == rail0 + 3 && g_writes == 0,
                "set_volt==NULL: a reconcile records the read-back in the "
                "shadow without a write (rail %u)",
                (unsigned)g_rail_mv);
        tst_mode         = CALI_MODE_DEFAULT;
        tst_psu.set_volt = tst_set_volt;
    }

    {
        const uint16_t base     = (uint16_t)WORK_FREQ;
        int            was_live = 1, pre_work = 0;
        uint32_t       tick;
        uint8_t        at[3];
        uint16_t       sent[3];

        tst_mode = 2;
        tst_idle = 0;
        memset(&g_tel, 0, sizeof g_tel);
        g_tel.asic_live  = 1;
        g_tel.asic_chips = MAX_ASIC_ON_SINGLE_HASH;
        g_tel.in_work    = 1;
        psu_set(80, RAIL_MID);
        g_rail_mv      = RAIL_MID;
        g_workmode     = 2;
        g_freq_held    = 0;
        g_tgt_volt     = RAIL_MID;
        g_tgt_freq     = base;
        g_work_freq    = (uint16_t)(base - 25);
        g_mode_settled = 0;
        mm_vf_set_ramp_target(base, 1);
        tick    = vf_tick(&was_live, &pre_work);
        sent[0] = g_vf_last;
        at[0]   = g_vf_at;
        CHECK(tick == MODE_STEP_TICK_MS,
                "in work, a mode change steps once a second (next tick %u ms)", (unsigned)tick);
        vf_tick(&was_live, &pre_work);
        sent[1] = g_vf_last;
        at[1]   = g_vf_at;
        vf_tick(&was_live, &pre_work);
        sent[2] = g_vf_last;
        at[2]   = g_vf_at;
        CHECK(sent[0] == base - 15 && sent[1] == base - 5 && sent[2] == base,
                "...by MODE_FREQ_STEP toward the ramp target (%u, %u, %u)", (unsigned)sent[0],
                (unsigned)sent[1], (unsigned)sent[2]);
        CHECK(at[0] == 0 && at[1] == 0 && at[2] == 1,
                "...and at_target is sent only with the target frequency itself "
                "(%u, %u, %u)",
                at[0], at[1], at[2]);
        tick = vf_tick(&was_live, &pre_work);
        CHECK(g_mode_settled && tick == POLICY_TICK_MS,
                "...then the policy settles back to its normal tick (%u ms)", (unsigned)tick);

        mm_vf_set_ramp_target(0, 0);
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        mm_telem_t none;
        uint16_t   lb;
        int        w = 0;

        vreg_restart(0);
        lb = vreg_budget(0);
        mm_vf_set_volt_tune(0);
        for (int i = 0; i < 200; i++)
            w += vreg_tick_at(POUT_BELOW(lb));
        CHECK(w == 0,
                "volt-tune off holds the rail through 200 "
                "below-band ticks (%d write(s))",
                w);
        mm_vf_set_volt_tune(1);
        CHECK(vreg_first_move(lb) > 0, "...and volt-tune on lets VOLT-REG move it again");

        mm_vf_set_volt_tune(0);
        tst_mode = 2;
        memset(&none, 0, sizeof none);
        policy_seed(&none);
        CHECK(atomic_load(&g_volt_tune) == 0, "an ASIC bring-up leaves volt-tune as it was set");
        mm_vf_set_volt_tune(1);
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        mm_periph fp;
        int       was_live = 0, pre_work = 0;

        memset(&g_tel, 0, sizeof g_tel);
        psu_set(80, RAIL_MID);
        vf_tick(&was_live, &pre_work);
        g_now_ms += VF_SENSOR_FRESH_MS;
        mm_vf_snapshot(&fp);
        CHECK(fp.ps_valid == 1 && fp.board_temp == 30,
                "a reading %u ms old is still published (valid %u, temp %d)", VF_SENSOR_FRESH_MS,
                fp.ps_valid, (int)fp.board_temp);
        g_now_ms += 1;
        mm_vf_snapshot(&fp);
        CHECK(fp.ps_valid == 0 && fp.board_temp == ASIC_TEMP_NONE,
                "...and withdrawn 1 ms later (valid %u, temp %d)", fp.ps_valid, (int)fp.board_temp);
    }

    {
        int was_live = 1, pre_work = 0, s0;

        memset(&g_tel, 0, sizeof g_tel);
        g_tel.asic_live  = 1;
        g_tel.asic_chips = MAX_ASIC_ON_SINGLE_HASH;
        g_tel.in_work    = 1;
        g_tel.pll_mhz[0] = (uint16_t)WORK_FREQ;
        g_work_freq      = (uint16_t)WORK_FREQ;
        g_ramp_at_sent   = 1;
        vf_tick(&was_live, &pre_work);

        /* asic_miner respawned: its first report is back at the start freq. */
        mm_vf_set_ramp_target(0, 0);
        mm_vf_asic_spawned();
        g_tel.in_work    = 0;
        g_tel.pll_mhz[0] = (uint16_t)INIT_FREQ;
        vf_tick(&was_live, &pre_work);
        CHECK(was_live && g_work_freq == INIT_FREQ && g_ramp_at_sent == 0,
                "a respawned asic_miner is seeded again even though no tick saw "
                "it absent: work freq %u (want %u), at_target sent %u",
                (unsigned)g_work_freq, (unsigned)INIT_FREQ, (unsigned)g_ramp_at_sent);

        s0 = g_vf_sends;
        mm_vf_set_ramp_target((uint16_t)(INIT_FREQ + 10), 0);
        vf_tick(&was_live, &pre_work);
        CHECK(g_vf_sends == s0 + 1 && g_vf_last == INIT_FREQ + 10,
                "...so the first ramp step reaches it (%d send(s), freq %u)", g_vf_sends - s0,
                (unsigned)g_vf_last);

        g_tel.pll_mhz[0] = (uint16_t)(INIT_FREQ + 10);
        vf_tick(&was_live, &pre_work);
        CHECK(g_work_freq == INIT_FREQ + 10,
                "...and without a new spawn the policy is not seeded again "
                "(work freq %u)",
                (unsigned)g_work_freq);
        mm_vf_set_ramp_target(0, 0);
    }

    {
        cali_param cp;
        uint16_t   tv, sent[3];

        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &cp, NULL);
        tv = cp.volt;
        if (tv < VOL_MIN)
            tv = VOL_MIN;
        if (tv > VOL_MAX)
            tv = VOL_MAX;
        tst_mode           = 2;
        g_send_fail        = 0;
        g_workmode         = 0;
        g_mode_settled     = 1;
        g_freq_held        = 0;
        g_switch_ramp_base = 0;
        g_rail_mv          = tv;
        g_tgt_freq         = (uint16_t)(cp.pll_start + 30);
        g_work_freq        = (uint16_t)(cp.pll_start + 30);
        mm_vf_set_ramp_target((uint16_t)(cp.pll_start + 30), 1);
        for (int i = 0; i < 3; i++) {
            (void)mode_tick();
            sent[i] = g_work_freq;
        }
        CHECK(sent[0] == cp.pll_start + 20 && sent[1] == cp.pll_start + 10 &&
                        sent[2] == cp.pll_start && g_tgt_freq == cp.pll_start,
                "a mode switch steps straight to the new mode while the ramp target "
                "still names the old one, with no step back (%u, %u, %u, want %u, "
                "%u, %u)",
                (unsigned)sent[0], (unsigned)sent[1], (unsigned)sent[2],
                (unsigned)(cp.pll_start + 20), (unsigned)(cp.pll_start + 10),
                (unsigned)cp.pll_start);

        mm_vf_set_ramp_target(cp.pll_start, 1);
        (void)mode_tick();
        CHECK(g_mode_settled && !g_switch_ramp_base && g_work_freq == cp.pll_start,
                "...the ramp catching up settles the mode (work %u)", (unsigned)g_work_freq);
        mm_vf_set_ramp_target((uint16_t)(cp.pll_start + 10), 1);
        (void)mode_tick();
        CHECK(g_tgt_freq == cp.pll_start + 10 && !g_mode_settled,
                "...and a later ramp target is followed again (target %u)", (unsigned)g_tgt_freq);

        mm_vf_set_ramp_target(0, 0);
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        cali_param cp;
        mm_telem_t none;
        int        s0;

        cali_clear();
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &cp, NULL);
        memset(&none, 0, sizeof none);
        tst_mode    = 1;
        g_send_fail = 0;
        g_aged_mode = MODE_UNSET;
        g_workmode  = 0;
        aging_tick(6000.0, &g_psu);
        g_workmode  = 1;
        g_work_freq = cp.pll_start;
        psu_set(cp.max_pout, RAIL_MID);
        for (int i = 0; i < 3; i++)
            aging_tick(6000.0, &g_psu);

        policy_seed(&none);
        g_workmode  = 1;
        g_work_freq = cp.pll_start;
        s0          = g_vf_sends;
        aging_tick(6000.0, &g_psu);
        CHECK(g_vf_sends == s0 + 2 && g_vf_last == (uint16_t)(cp.pll_start + ADJ_FREQ_INTERVAL),
                "an ASIC re-seed restarts the running mode's aging sweep from "
                "pll_start %u (%d send(s), last %u)",
                (unsigned)cp.pll_start, g_vf_sends - s0, (unsigned)g_vf_last);

        policy_seed(&none);
        g_workmode     = 1;
        g_work_freq    = cp.pll_start;
        g_mode_settled = 1;
        aging_cadence(6000.0, &g_psu);
        g_now_ms += AGING_FIRST_RUN_MS;
        s0 = g_vf_sends;
        aging_cadence(6000.0, &g_psu);
        CHECK(g_vf_sends > s0,
                "a settled mode that needs aging steps after "
                "AGING_FIRST_RUN_MS (%d send(s))",
                g_vf_sends - s0);

        policy_seed(&none);
        g_workmode     = 1;
        g_work_freq    = cp.pll_start;
        g_mode_settled = 1;
        g_now_ms += AGING_FIRST_RUN_MS;
        s0 = g_vf_sends;
        aging_cadence(6000.0, &g_psu);
        CHECK(g_vf_sends == s0,
                "...and after a re-seed it waits "
                "AGING_FIRST_RUN_MS again (%d send(s))",
                g_vf_sends - s0);
        g_now_ms += AGING_FIRST_RUN_MS;
        aging_cadence(6000.0, &g_psu);
        CHECK(g_vf_sends > s0, "...before the restarted sweep steps (%d send(s))", g_vf_sends - s0);

        cali_clear();
        g_aged_mode = MODE_UNSET;
        tst_mode    = CALI_MODE_DEFAULT;
    }

    {
        cali_param cp;
        mm_telem_t none;

        cali_clear();
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &cp, NULL);
        memset(&none, 0, sizeof none);
        tst_mode       = 1;
        g_send_fail    = 0;
        g_seed_respawn = 0;
        mm_vf_set_rail(VOL_MAX);
        policy_seed(&none);
        CHECK(vol_clamp(cp.volt) < VOL_MAX && g_rail_mv == VOL_MAX,
                "a seed after a telemetry gap keeps the written rail as its floor "
                "(rail %u, cali %u)",
                (unsigned)g_rail_mv, (unsigned)vol_clamp(cp.volt));

        mm_vf_set_rail(VOL_MAX);
        g_seed_respawn = 1;
        policy_seed(&none);
        CHECK(g_rail_mv == vol_clamp(cp.volt),
                "a seed after an asic_miner spawn takes the cali volt only (rail %u "
                "want %u)",
                (unsigned)g_rail_mv, (unsigned)vol_clamp(cp.volt));

        mm_vf_set_rail(VOL_MAX);
        mm_vf_rail_unknown();
        policy_seed(&none);
        CHECK(g_rail_mv == vol_clamp(cp.volt),
                "after a rail power cycle the next seed takes the cali volt only "
                "(rail %u want %u)",
                (unsigned)g_rail_mv, (unsigned)vol_clamp(cp.volt));
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        cali_param cp;
        mm_telem_t none;
        mm_periph  ap;
        int        s0;

        cali_clear();
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 1, 0, &cp, NULL);
        memset(&none, 0, sizeof none);
        memset(&g_aging, 0, sizeof g_aging);
        psu_set(cp.max_pout, RAIL_MID);
        tst_mode    = 1;
        g_send_fail = 0;
        set_aged_mode(1);
        policy_seed(&none);
        g_workmode     = 1;
        g_work_freq    = cp.pll_start;
        g_mode_settled = 1;
        s0             = g_vf_sends;
        for (int i = 0; i < 3; i++) {
            aging_cadence(6000.0, &g_psu);
            g_now_ms += AGING_FIRST_RUN_MS;
        }
        mm_vf_snapshot(&ap);
        CHECK(g_vf_sends == s0 && !g_aging.inited && ap.aging_finished == 1,
                "a mode aged this run but not persisted is not swept again after a "
                "re-seed and still reports aged (%d send(s), aged %u)",
                g_vf_sends - s0, (unsigned)ap.aging_finished);

        set_aged_mode(2);
        policy_seed(&none);
        g_workmode     = 1;
        g_work_freq    = cp.pll_start;
        g_mode_settled = 1;
        s0             = g_vf_sends;
        for (int i = 0; i < 3; i++) {
            aging_cadence(6000.0, &g_psu);
            g_now_ms += AGING_FIRST_RUN_MS;
        }
        mm_vf_snapshot(&ap);
        CHECK(g_vf_sends > s0 && g_aging.inited && ap.aging_finished == 0,
                "...while a mode not aged this run is swept again after a re-seed "
                "(%d send(s), aged %u)",
                g_vf_sends - s0, (unsigned)ap.aging_finished);

        g_aging.aborted = 1;
        policy_seed(&none);
        g_workmode = 1;
        s0         = g_vf_sends;
        aging_tick(6000.0, &g_psu);
        CHECK(g_aging.aborted && g_aging.mode == 1 && g_vf_sends == s0,
                "a sweep stopped at the frequency ceiling is not run again in this mode "
                "after a re-seed (%d send(s))",
                g_vf_sends - s0);
        g_workmode  = 2;
        g_work_freq = WORK_FREQ;
        aging_tick(6000.0, &g_psu);
        CHECK(g_aging.mode == 2 && g_aging.inited && g_vf_sends > s0,
                "...and another mode still sweeps (%d send(s))", g_vf_sends - s0);

        set_aged_mode(MODE_UNSET);
        memset(&g_aging, 0, sizeof g_aging);
        cali_clear();
        tst_mode = CALI_MODE_DEFAULT;
    }

    {
        cali_param     cp;
        const uint16_t f0 = WORK_FREQ;
        const uint16_t lo = (uint16_t)(RAIL_MID - 2 * RAIL_VOLT_STEP);

        cali_clear();
        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &cp, NULL);
        psu_set(cp.max_pout, RAIL_MID);
        g_send_fail = 0;

        best_at(2, (uint16_t)(f0 - 20), RAIL_MID);
        g_work_freq = f0;
        g_rail_mv   = RAIL_MID;
        ord_reset();
        aging_tick(6000.0, &g_psu);
        CHECK(!strcmp(g_ord, "FV") && g_work_freq == f0 - 20 &&
                        g_rail_mv == RAIL_MID - RAIL_VOLT_STEP,
                "a best point below the running rail lowers the frequency before the "
                "rail (order %s, freq %u, rail %u)",
                g_ord, (unsigned)g_work_freq, (unsigned)g_rail_mv);

        best_at(2, (uint16_t)(f0 + 10), RAIL_MID);
        g_work_freq = f0;
        g_rail_mv   = lo;
        ord_reset();
        aging_tick(6000.0, &g_psu);
        CHECK(!strcmp(g_ord, "VF") && g_work_freq == f0 + 10 &&
                        g_rail_mv == RAIL_MID - RAIL_VOLT_STEP,
                "a best point above the running rail raises the rail before the "
                "frequency (order %s, freq %u, rail %u)",
                g_ord, (unsigned)g_work_freq, (unsigned)g_rail_mv);

        best_at(2, (uint16_t)(f0 - 20), RAIL_MID);
        g_work_freq = f0;
        g_rail_mv   = RAIL_MID;
        g_send_fail = 1;
        ord_reset();
        aging_tick(6000.0, &g_psu);
        g_send_fail = 0;
        CHECK(g_ord[0] == '\0' && g_work_freq == f0 && g_rail_mv == RAIL_MID,
                "a best frequency that cannot be sent leaves the rail where it was "
                "(order %s, freq %u, rail %u)",
                g_ord, (unsigned)g_work_freq, (unsigned)g_rail_mv);

        uint16_t budget = (uint16_t)(cp.max_pout - POUT_UP_LIMIT - 10);
        g_work_freq     = f0;
        g_rail_mv       = RAIL_MID;
        tst_pout        = (uint16_t)(budget + PD_POWER_RESERVE_W);
        aging_prime(2);
        g_aging.st.aging_direction = 1;
        g_aging.st.enum_count      = 1;
        aging_tick(6000.0, &g_psu);
        int over_budget = g_aging.st.best_rate.hashrate_Mhz;
        tst_pout        = 0;
        aging_prime(2);
        g_aging.st.aging_direction = 1;
        g_aging.st.enum_count      = 1;
        aging_tick(6000.0, &g_psu);
        CHECK(pout_budget(cp.max_pout) == cp.max_pout && over_budget == 0 &&
                        g_aging.st.best_rate.hashrate_Mhz > 0,
                "aging takes the power adapter budget %u W, not the cali row's %u W, as "
                "its best-point limit: a %u W sample is refused under the budget and "
                "taken without one",
                (unsigned)budget, (unsigned)cp.max_pout, (unsigned)cp.max_pout);

        memset(&g_aging, 0, sizeof g_aging);
        g_workmode = 2;
        g_rail_mv  = RAIL_MID;
        cali_clear();
    }

    {
        cali_clear();
        mm_vf_set_ramp_target(0, 0);
        g_aged_mode = 2;
        g_workmode  = 2;
        g_aging_bar = 100;
        tst_mode    = 1;
        (void)mode_tick();
        CHECK(g_aging_bar == 0,
                "a switch from an aged mode to one not aged sets BAR 0 at once (%u)",
                (unsigned)g_aging_bar);
        tst_mode = 2;
        (void)mode_tick();
        CHECK(g_aging_bar == 100, "...and back to the aged mode sets BAR 100 (%u)",
                (unsigned)g_aging_bar);
        g_aged_mode = MODE_UNSET;
        tst_mode    = CALI_MODE_DEFAULT;
    }

    {
        int was_live = 1, pre_work = 0;

        tst_psu.set_volt = NULL;
        CHECK(mm_vf_set_volt(RAIL_MID) == -1 && atomic_load(&g_volt_req) == 0,
                "set_volt==NULL: mm_vf_set_volt refuses and keeps no request");
        tst_psu.set_volt = tst_set_volt;
        CHECK(mm_vf_set_volt((uint16_t)(VOL_MIN - 1)) == -1 &&
                        mm_vf_set_volt((uint16_t)(VOL_MAX + 1)) == -1 &&
                        atomic_load(&g_volt_req) == 0,
                "mm_vf_set_volt refuses %u and %u, outside [%u %u]", (unsigned)(VOL_MIN - 1),
                (unsigned)(VOL_MAX + 1), (unsigned)VOL_MIN, (unsigned)VOL_MAX);

        tst_mode = 2;
        tst_idle = 0;
        memset(&g_tel, 0, sizeof g_tel);
        g_tel.asic_live  = 1;
        g_tel.asic_chips = MAX_ASIC_ON_SINGLE_HASH;
        g_tel.in_work    = 0;
        g_psu_rc         = -1;
        g_workmode       = 2;
        g_mode_settled   = 1;
        g_aged_mode      = 2;
        g_tgt_volt       = RAIL_MID;
        g_rail_mv        = RAIL_MID;
        mm_vf_set_ramp_target(0, 0);

        CHECK(mm_vf_set_volt((uint16_t)VOL_MIN) == 0 && atomic_load(&g_volt_req) == VOL_MIN,
                "mm_vf_set_volt(%u) is accepted and kept pending", (unsigned)VOL_MIN);
        g_writes = 0;
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 0 && g_rail_mv == RAIL_MID && atomic_load(&g_volt_req) == VOL_MIN,
                "...not applied before work (rail %u, %d write(s))", (unsigned)g_rail_mv, g_writes);
        g_tel.in_work = 1;
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 1 && g_rail_mv == VOL_MIN && g_tgt_volt == VOL_MIN &&
                        atomic_load(&g_volt_req) == 0,
                "...applied on the first V/F tick in work (rail %u, %d write(s))",
                (unsigned)g_rail_mv, g_writes);
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 1, "...and only once (%d write(s))", g_writes);

        g_rail_mv = RAIL_MID;
        g_writes  = 0;
        (void)mm_vf_set_volt((uint16_t)VOL_MAX);
        tst_idle = 1;
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 0 && g_rail_mv == RAIL_MID,
                "a pending request writes nothing while the ASIC is stopped (%d write(s))",
                g_writes);
        mm_vf_rail_unknown();
        tst_idle = 0;
        was_live = 1;
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 0 && g_rail_mv == RAIL_MID && atomic_load(&g_volt_req) == 0,
                "...and a rail power change drops it (%d write(s))", g_writes);

        (void)mm_vf_set_volt((uint16_t)VOL_MAX);
        mm_vf_asic_spawned();
        vf_tick(&was_live, &pre_work);
        CHECK(atomic_load(&g_volt_req) == 0, "an asic_miner spawn drops a pending request");

        aging_prime(2);
        was_live       = 1;
        g_mode_settled = 1;
        g_rail_mv      = RAIL_MID;
        vf_tick(&was_live, &pre_work);
        CHECK(mm_vf_set_volt((uint16_t)VOL_MIN) == -2 && atomic_load(&g_volt_req) == 0,
                "mm_vf_set_volt answers -2 while aging runs and keeps no request");

        g_aging.inited = 0;
        vf_tick(&was_live, &pre_work);
        CHECK(mm_vf_set_volt((uint16_t)VOL_MIN) == 0 && atomic_load(&g_volt_req) == VOL_MIN,
                "mm_vf_set_volt is accepted before aging starts");
        g_aging.inited = 1;
        g_writes       = 0;
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 0 && g_rail_mv == RAIL_MID && atomic_load(&g_volt_req) == 0,
                "...and aging started before the next V/F tick drops it (rail %u, %d write(s))",
                (unsigned)g_rail_mv, g_writes);

        g_aging.finished = 1;
        vf_tick(&was_live, &pre_work);
        CHECK(mm_vf_set_volt((uint16_t)VOL_MIN) == 0, "mm_vf_set_volt is accepted after aging");
        vf_tick(&was_live, &pre_work);
        CHECK(g_writes == 1 && g_rail_mv == VOL_MIN && atomic_load(&g_volt_req) == 0,
                "...and the next V/F tick in work applies it (rail %u, %d write(s))",
                (unsigned)g_rail_mv, g_writes);
        memset(&g_aging, 0, sizeof g_aging);
        atomic_store(&g_aging_on, 0);

        memset(&g_tel, 0, sizeof g_tel);
        g_psu_rc         = 0;
        g_aged_mode      = MODE_UNSET;
        tst_mode         = CALI_MODE_DEFAULT;
        tst_psu.set_volt = tst_set_volt;
    }

    return tst_done();
}
