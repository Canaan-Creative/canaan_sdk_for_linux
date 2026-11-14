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

#include "fault_report.h"
#include "mm_fan.h"
#include "mm_internal.h"
#include "mm_telem.h"
#include "mm_vf.h"
#include "osif.h"
#include "wdt_dev.h"
#include "wdt_flags.h"

#include <signal.h>
#include <time.h>

#define WDT_THREAD_SLICE_MS 1000u   /* longest sigtimedwait() wait, ms; SIGUSR2 and stop latency */
#define WDT_CORE_GRACE_MS   30000u  /* core leg left out of the gate after a core_miner spawn, ms */
#define WDT_ASIC_GRACE_MS   120000u /* asic leg out of the gate after an asic_miner spawn, ms */
#define WDT_GRACE_TRIES_MAX 3u /* spawns per leg given grace until it reports live outside grace */

_Static_assert(TELEM_STALE_FAILSAFE_MS <= WDT_STARVE_MS,
        "the telemetry fail-safe must drive the fan to maximum no later "
        "than the tick the watchdog feed gate stops feeding the dog; "
        "raise WDT_STARVE_MS or lower TELEM_STALE_FAILSAFE_MS, or the "
        "board is reset while it is still cooling at PID duty");

_Static_assert(WDT_CORE_WEDGE_MS < WDT_STARVE_MS,
        "the wedged-core SIGKILL must fall due before the watchdog "
        "starve budget expires, or the dog has already reset the SoC "
        "when the kill is scheduled and the wedge detector is dead code");

/* Start-up grace of one gate leg, indexed by WDT_TID_*; used by mm_wdt_thread only */
typedef struct wdt_grace {
    uint64_t spawn_at; /* child spawn time last seen, monotonic ms, 0 before the first spawn */
    /* spawns given grace since the last live report outside grace, 0..WDT_GRACE_TRIES_MAX + 1 */
    unsigned tries;
    int      deliberate; /* 1: a softoff stopped the child, so its next spawn uses no try */
    int      spent;      /* 1: grace exhausted and logged once; cleared with tries */
} wdt_grace_t;

static void wdt_grace_earned(wdt_grace_t *g)
{
    g->tries = 0;
    g->spent = 0;
}

static int wdt_grace_open(wdt_grace_t *g, int tid, uint64_t spawn_at, uint64_t now,
        uint32_t window_ms, int stopped)
{
    if (stopped)
        g->deliberate = 1;
    if (spawn_at != g->spawn_at) {
        if (!g->deliberate && g->tries <= WDT_GRACE_TRIES_MAX)
            g->tries++;
        g->spawn_at   = spawn_at;
        g->deliberate = 0;
    }
    if (g->tries > WDT_GRACE_TRIES_MAX && !g->spent) {
        g->spent = 1;
        osif_log(OSIF_LOG_WARN,
                "%s spawned %u times without a live report — "
                "no more start-up grace; the WDT now allows %u s of silence "
                "instead of %u s",
                wdt_flags_tid_name(tid), WDT_GRACE_TRIES_MAX + 1u, WDT_STARVE_MS / 1000u,
                (window_ms + WDT_STARVE_MS) / 1000u);
    }
    return spawn_at != 0 && g->tries <= WDT_GRACE_TRIES_MAX &&
            (int64_t)(now - spawn_at) < (int64_t)window_ms;
}

static void mm_wdt_tick(wdt_flags_t *f, wdt_grace_t *grace, uint64_t *healthy_at,
        fault_report_t *missing_rep, uint64_t now)
{
    static fault_report_t vf_rep;
    mm_telem_t            t;
    mm_telem_get(&t);
    unsigned stopped = softonoff_children_stopped(&g_softonoff, g_fault_off);

    int core_starting = wdt_grace_open(&grace[WDT_TID_CORE], WDT_TID_CORE,
            atomic_load(&g_core_spawn_at), now, WDT_CORE_GRACE_MS, 0);
    int asic_starting = wdt_grace_open(&grace[WDT_TID_ASIC], WDT_TID_ASIC,
            atomic_load(&g_asic_spawn_at), now, WDT_ASIC_GRACE_MS, stopped & SOFTONOFF_CHILD_ASIC);

    wdt_flags_enable(f, WDT_TID_ASIC, !asic_starting);
    wdt_flags_enable(f, WDT_TID_CORE, !core_starting);

    uint64_t vf_at      = mm_vf_beat_ms();
    int      vf_stalled = vf_at && now > vf_at && now - vf_at > WDT_VF_STALL_MS;
    if (fault_report_step(&vf_rep, (uint32_t)vf_stalled) != FAULT_REPORT_NONE && vf_stalled)
        osif_log(OSIF_LOG_ERR, "WDT: V/F control has not run for %u s", WDT_VF_STALL_MS / 1000u);

    int complete = 0;
    if (atomic_exchange(&g_wdt_beat, 0) && !vf_stalled)
        complete |= wdt_flags_mark(f, WDT_TID_SUPERVISOR) == 1;
    if (t.asic_live || (stopped & SOFTONOFF_CHILD_ASIC)) {
        if (t.asic_live && !asic_starting)
            wdt_grace_earned(&grace[WDT_TID_ASIC]);
        complete |= wdt_flags_mark(f, WDT_TID_ASIC) == 1;
    }
    if (t.core_live) {
        if (!core_starting)
            wdt_grace_earned(&grace[WDT_TID_CORE]);
        complete |= wdt_flags_mark(f, WDT_TID_CORE) == 1;
    }

    if (complete)
        *healthy_at = now;

    uint32_t missing_mask = complete ? 0u : (~wdt_flags_raw(f) & wdt_flags_enabled_mask(f));
    if (fault_report_step(missing_rep, missing_mask) != FAULT_REPORT_NONE && missing_mask != 0) {
        char missing[64];
        wdt_flags_missing(f, missing, sizeof missing);
        osif_log(OSIF_LOG_OP, "WDT: no recent report from %s", missing);
    }

    wdt_flags_t pub = *f;
    if (complete)
        pub.mask = pub.enabled;
    mm_wdt_sig_publish(wdt_flags_pack(&pub));

    if (now - *healthy_at < WDT_STARVE_MS)
        wdt_dev_feed();
}

static void mm_wdt_wait(const sigset_t *usr2, uint32_t ms)
{
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };

    if (sigtimedwait(usr2, NULL, &ts) == SIGUSR2)
        mm_wdt_sig_pretimeout();
}

void *mm_wdt_thread(void *arg)
{
    (void)arg;
    wdt_flags_t f;
    wdt_flags_reset(&f);
    wdt_grace_t    grace[WDT_TID_TOTAL] = { 0 };
    uint64_t       healthy_at           = osif_now_ms();
    fault_report_t missing_rep          = { 0 };
    uint32_t       period_ms            = (uint32_t)wdt_dev_feed_period_ms();
    uint32_t       deadline_ms          = (uint32_t)wdt_dev_deadline_s() * 1000u;
    sigset_t       usr2;

    mm_wdt_sig_usr2_set(&usr2);
    wdt_dev_feed();

    /* Past the starve budget the hardware, not this thread, decides when an
     * unhealthy board resets; the WDT_STARVE_MS asserts no longer order it. */
    if (deadline_ms > WDT_STARVE_MS)
        osif_log(OSIF_LOG_ERR,
                "WDT hardware deadline %u ms is longer than "
                "the %u ms supervision limit",
                deadline_ms, WDT_STARVE_MS);

    while (g_run) {
        for (uint32_t slept = 0; slept < period_ms && g_run;) {
            uint32_t slice = period_ms - slept;
            if (slice > WDT_THREAD_SLICE_MS)
                slice = WDT_THREAD_SLICE_MS;
            mm_wdt_wait(&usr2, slice);
            slept += slice;
        }
        if (!g_run)
            break;
        mm_wdt_tick(&f, grace, &healthy_at, &missing_rep, osif_now_ms());
    }
    while (!atomic_load(&g_wdt_released))
        mm_wdt_wait(&usr2, WDT_THREAD_SLICE_MS);
    return NULL;
}
