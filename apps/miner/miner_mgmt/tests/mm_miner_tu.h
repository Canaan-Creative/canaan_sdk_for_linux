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

#ifndef MM_MINER_TU_H
#define MM_MINER_TU_H

#define main mm_miner_main_unused
#include "mm_miner_main.c"
#undef main
#include "mm_ctrl.c"
#include "mm_periph.c"
#include "mm_softoff.c"
#include "mm_power.c"
#include "mm_spawn.c"
#ifdef sigtimedwait
#include "mm_wdt.c"
#else
#include <errno.h>
#include <signal.h>
#include <time.h>

#define TST_WDT_POLL_NS 10000000L /* watchdog thread wait slice, ns; bounds its stop latency */

static int tst_wdt_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *ts)
{
    long long left    = (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
    int       running = g_run;

    while (left > 0 && (running ? g_run : !atomic_load(&g_wdt_released))) {
        struct timespec step = { 0, left < TST_WDT_POLL_NS ? (long)left : TST_WDT_POLL_NS };
        int             sig  = sigtimedwait(set, info, &step);

        if (sig > 0 || errno != EAGAIN)
            return sig;
        left -= step.tv_nsec;
    }
    errno = EAGAIN;
    return -1;
}

#define sigtimedwait(s, i, t) \
    tst_wdt_sigtimedwait((s), (i), \
            (t)) /* ends a wait early once g_run clears or the dog is released */
#include "mm_wdt.c"
#undef sigtimedwait
#endif

#endif /* MM_MINER_TU_H */
