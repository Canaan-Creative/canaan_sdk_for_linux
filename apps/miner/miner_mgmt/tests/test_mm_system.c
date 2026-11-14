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
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "mc_api_bootby.h"
#include "osif.h"
#include "../../tests/include/tst_check.h"

#define TST_EV_MAX 16 /* Capacity of tst_ev[], events; later events are counted but not stored */

int  tst_reboot(int how);
void tst_sync(void);

#define reboot(h) tst_reboot(h)
#define sync()    tst_sync()

#include "mm_system.c"

#undef sync
#undef reboot

_Atomic int g_rebooting;
_Atomic int g_reboot_failed; /* reboot() returned flag */
_Atomic int g_ota_reboot;
static char tst_ev[TST_EV_MAX][32];    /* Event log, one formatted string per stub call */
static int  tst_ev_n;                  /* Events logged so far; may exceed TST_EV_MAX */
static char tst_log[160];              /* First OP log line since reset() */
static char tst_last_op[160];          /* Latest OP log line */
static int  tst_log_n;                 /* OP log lines since reset() */
static int  tst_log_at_reboot;         /* tst_log_n when reboot() was called */
static int  tst_reboot_rc;             /* Non-zero makes reboot() fail with EPERM */
static int  tst_reboot_how;            /* Command passed to reboot(), -1 before the call */
static int  tst_flag_at_rail_off = -1; /* g_rebooting at the rail cut, -1 before it */
static int  tst_flag_at_reboot   = -1; /* g_rebooting at reboot(), -1 before the call */
static int  tst_ota_at_reboot    = -1; /* g_ota_reboot at reboot(), -1 before the call */

static void ev(const char *fmt, ...)
{
    va_list ap;
    if (tst_ev_n >= TST_EV_MAX) {
        tst_ev_n++;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(tst_ev[tst_ev_n++], sizeof tst_ev[0], fmt, ap);
    va_end(ap);
}
static const char *ev_str(void)
{
    static char b[512];
    size_t      n = 0;
    b[0]          = '\0';
    for (int i = 0; i < tst_ev_n && i < TST_EV_MAX && n + 1 < sizeof b; i++)
        n += (size_t)snprintf(b + n, sizeof b - n, "%s%s", i ? "," : "", tst_ev[i]);
    return b;
}
static int ev_starts(const char *const *want, int n)
{
    if (tst_ev_n < n)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(tst_ev[i], want[i]))
            return 0;
    return 1;
}
static int ev_is(const char *const *want, int n)
{
    return tst_ev_n == n && ev_starts(want, n);
}

int tst_reboot(int how)
{
    tst_reboot_how     = how;
    tst_flag_at_reboot = atomic_load(&g_rebooting);
    tst_ota_at_reboot  = atomic_load(&g_ota_reboot);
    tst_log_at_reboot  = tst_log_n;
    ev(how == RB_POWER_OFF ? "poweroff" : "reboot");
    if (tst_reboot_rc) {
        errno = EPERM;
        return -1;
    }
    return 0;
}
void tst_sync(void)
{
    ev("sync");
}

int rail_power_off_for_reboot(void)
{
    tst_flag_at_rail_off = atomic_load(&g_rebooting);
    ev("rail-off");
    return 0;
}
int mm_bootby_stage(uint8_t reason, uint32_t sub)
{
    ev("stage=%u,%u", (unsigned)reason, (unsigned)sub);
    return 0;
}
void mm_cfg_flush(void)
{
    ev("cfgflush");
}
int mm_bootby_persist(uint8_t reason, uint32_t sub)
{
    ev("persist=%u,%u", (unsigned)reason, (unsigned)sub);
    return 0;
}
void osif_sleep_ms(uint32_t ms)
{
    ev("sleep=%u", (unsigned)ms);
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl != OSIF_LOG_OP)
        return;
    va_start(ap, fmt);
    vsnprintf(tst_last_op, sizeof tst_last_op, fmt, ap);
    va_end(ap);
    if (!tst_log_n++)
        snprintf(tst_log, sizeof tst_log, "%s", tst_last_op);
}

static void reset(int reboot_rc)
{
    tst_ev_n             = 0;
    tst_log_n            = 0;
    tst_log[0]           = '\0';
    tst_reboot_rc        = reboot_rc;
    tst_reboot_how       = -1;
    tst_flag_at_rail_off = tst_flag_at_reboot = -1;
    atomic_store(&g_rebooting, 0);
    atomic_store(&g_reboot_failed, 0);
}

int main(void)
{
    char stage[32];
    int  rc;

    snprintf(stage, sizeof stage, "stage=%u,%u", (unsigned)MC_BOOTBY_WDT, 0x1234u);
    {
        const char *const want[] = { stage, "cfgflush", "sync", "rail-off", "reboot" };
        reset(0);
        (void)mm_system_reboot(MC_BOOTBY_WDT, 0x1234u);
        CHECK(ev_starts(want, 5) && tst_reboot_how == RB_AUTOBOOT,
                "reboot stages the reason, saves settings and syncs, so the reason is on disk "
                "before the rail is cut, then reboots (saw %s)",
                ev_str());
        CHECK(tst_flag_at_rail_off == 1 && tst_flag_at_reboot == 1,
                "the rebooting flag is raised before the rail is cut and is still "
                "up at reboot(2) (rail-off %d, reboot %d)",
                tst_flag_at_rail_off, tst_flag_at_reboot);
        CHECK(tst_log_at_reboot == 1 &&
                        strcmp(tst_log, "[BOOT] Reboot reason 0x04 sub 0x00001234, WDT") == 0,
                "a reboot logs exactly one OP line naming the reason (saw %d: %s)",
                tst_log_at_reboot, tst_log);
    }
    {
        const char *const want[] = { "cfgflush", "sync", "rail-off", "reboot" };
        reset(0);
        (void)mm_system_reboot(0, 7);
        CHECK(ev_starts(want, 4), "a reboot with reason 0 stages no reason (saw %s)", ev_str());
        CHECK(strcmp(tst_log, "[BOOT] Reboot reason 0x00 sub 0x00000007, Unknown") == 0,
                "...and still logs the reason and sub (saw %s)", tst_log);
    }
    {
        const char *const want[] = { stage, "cfgflush", "sync", "rail-off", "reboot" };
        reset(1);
        rc = mm_system_reboot(MC_BOOTBY_WDT, 0x1234u);
        CHECK(rc == -1 && ev_is(want, 5),
                "a failed reboot(2) returns -1 and does not power the rail back "
                "on from the calling thread (rc %d, saw %s)",
                rc, ev_str());
        CHECK(atomic_load(&g_reboot_failed) == 1 && atomic_load(&g_rebooting) == 1,
                "...it requests the stopped-state log and keeps its rebooting "
                "count held, so the rail and spawn stay refused (request %d, "
                "count %d)",
                atomic_load(&g_reboot_failed), atomic_load(&g_rebooting));
        reset(1);
        atomic_store(&g_rebooting, 1);
        (void)mm_system_reboot(MC_BOOTBY_WDT, 0x1234u);
        CHECK(atomic_load(&g_rebooting) == 2,
                "a failed reboot adds its own count to a reboot already in "
                "progress (count %d)",
                atomic_load(&g_rebooting));
    }
    {
        const char *const want[] = { "cfgflush", "sync", "poweroff" };
        reset(0);
        (void)mm_system_poweroff();
        CHECK(ev_starts(want, 3) && tst_reboot_how == RB_POWER_OFF && tst_flag_at_reboot == 1,
                "power off saves settings, syncs "
                "and powers off with the rebooting flag up (saw %s, flag %d)",
                ev_str(), tst_flag_at_reboot);
        reset(1);
        rc = mm_system_poweroff();
        CHECK(rc == -1 && ev_is(want, 3) && atomic_load(&g_rebooting) == 1 &&
                        atomic_load(&g_reboot_failed) == 0,
                "a failed power off returns -1 and keeps its rebooting count "
                "held, so the rail and spawn stay refused (rc %d, saw %s, "
                "count %d)",
                rc, ev_str(), atomic_load(&g_rebooting));
        CHECK(strcmp(tst_last_op, "power off failed — mining stays stopped") == 0,
                "...and logs that mining stays stopped (saw %s)", tst_last_op);
    }
    {
        char              persist[32], delay[32], up[32];
        const char *const want[] = { persist, "cfgflush", delay, up, "cfgflush", "sync", "rail-off",
            "reboot" };

        snprintf(persist, sizeof persist, "persist=%u,0", (unsigned)MC_BOOTBY_UPGRADE);
        snprintf(delay, sizeof delay, "sleep=%u", MM_OTA_REBOOT_DELAY_MS);
        snprintf(up, sizeof up, "stage=%u,0", (unsigned)MC_BOOTBY_UPGRADE);
        reset(0);
        ota_arm_reboot();
        CHECK(ev_is(want, 8) && tst_ota_at_reboot == 1,
                "an armed OTA reboot persists MC_BOOTBY_UPGRADE and saves the settings "
                "before its %u ms delay, so a hang in the delay or in the reboot path "
                "still leaves the reason on disk, then reboots with that reason "
                "(saw %s, armed at reboot %d)",
                MM_OTA_REBOOT_DELAY_MS, ev_str(), tst_ota_at_reboot);
        CHECK(atomic_load(&g_ota_reboot) == 0,
                "...and a reboot(2) that returns disarms the OTA reboot (flag %d)",
                atomic_load(&g_ota_reboot));
        atomic_store(&g_ota_reboot, 0);
    }

    return tst_done();
}
