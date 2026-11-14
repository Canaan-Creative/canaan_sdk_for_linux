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

#include "wdt_dev.h"
#include "osif.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

/* watchdog device descriptor, -1 when closed */
static int s_fd = -1;
static int s_bootstatus; /* WDIOC_GETBOOTSTATUS flags */
static int s_pretimeout; /* granted pretimeout, s; 0 when not armed */
static int s_timeout;    /* granted timeout, s; 0 when unknown */
/* time from a feed to the first WDT action, s; 0 when unknown */
static int s_deadline;
static int s_feed_period_ms = WDT_FEED_PERIOD_MS_MAX;

static void wdt_set_cadence(int deadline_s)
{
    long ms = (long)deadline_s * 1000 / WDT_FEED_MARGIN;
    if (ms > WDT_FEED_PERIOD_MS_MAX)
        ms = WDT_FEED_PERIOD_MS_MAX;
    if (ms < WDT_FEED_PERIOD_MS_MIN)
        ms = WDT_FEED_PERIOD_MS_MIN;
    s_feed_period_ms = (int)ms;

    if ((long)deadline_s * 1000 < (long)WDT_FEED_MARGIN * s_feed_period_ms)
        osif_log(OSIF_LOG_ERR,
                "WDT: hardware deadline is only %d s, too short for "
                "%d feeds every %d ms; the WDT can fire between two feeds",
                deadline_s, WDT_FEED_MARGIN, s_feed_period_ms);
}

int wdt_dev_init(void)
{
    s_pretimeout     = 0;
    s_bootstatus     = 0;
    s_timeout        = 0;
    s_deadline       = 0;
    s_feed_period_ms = WDT_FEED_PERIOD_MS_MAX;

    s_fd = open(WDT_DEV_DEFAULT, O_WRONLY | O_CLOEXEC);
    if (s_fd < 0) {
        osif_log(OSIF_LOG_ERR, "WDT: cannot open %s; running without a hardware WDT",
                WDT_DEV_DEFAULT);
        return -1;
    }

    int requested = WDT_TIMEOUT_S;
    if (ioctl(s_fd, WDIOC_SETTIMEOUT, &requested) != 0)
        osif_log(OSIF_LOG_WARN,
                "WDT: driver refused a %d s timeout; keeping the hardware "
                "default, so the timings below are the driver's",
                WDT_TIMEOUT_S);

    int two_stage_on = WDT_PRETIMEOUT_S;
    if (ioctl(s_fd, WDIOC_SETPRETIMEOUT, &two_stage_on) == 0) {
        int granted_pt = 0;
        if (ioctl(s_fd, WDIOC_GETPRETIMEOUT, &granted_pt) == 0 && granted_pt > 0)
            s_pretimeout = granted_pt;
    } else {
        osif_log(OSIF_LOG_WARN,
                "WDT: early warning (pretimeout) not supported; "
                "no clean reboot before a WDT reset");
    }

    int granted = 0;
    if (ioctl(s_fd, WDIOC_GETTIMEOUT, &granted) != 0 || granted <= 0) {
        s_feed_period_ms = WDT_FEED_PERIOD_MS_MIN;
        osif_log(OSIF_LOG_ERR,
                "WDT started (requested %d s) but its timeout could not "
                "be read back; "
                "feeding every %d ms",
                requested, s_feed_period_ms);
    } else {
        int deadline = granted;
        if (s_pretimeout > 0 && s_pretimeout < granted)
            deadline = granted - s_pretimeout;
        s_timeout  = granted;
        s_deadline = deadline;
        osif_log(OSIF_LOG_INFO, "WDT set %d s, actual is %d s", requested, granted);
        wdt_set_cadence(deadline);
        osif_log(OSIF_LOG_DBG, "WDT: feeding every %d ms", s_feed_period_ms);
    }

    if (ioctl(s_fd, WDIOC_GETBOOTSTATUS, &s_bootstatus) == 0 && s_bootstatus != 0)
        osif_log(OSIF_LOG_INFO,
                "WDT: the previous boot ended in %s "
                "(status 0x%x)",
                (s_bootstatus & WDIOF_EXTERN1) ? "a kernel panic" : "a WDT reset", s_bootstatus);
    return 0;
}

void wdt_dev_feed(void)
{
    if (s_fd < 0)
        return;
    ioctl(s_fd, WDIOC_KEEPALIVE, 0);
}

void wdt_dev_disarm(void)
{
    if (s_fd < 0)
        return;
    int opt = WDIOS_DISABLECARD;
    ioctl(s_fd, WDIOC_SETOPTIONS, &opt);
    if (write(s_fd, "V", 1) != 1)
        osif_log(OSIF_LOG_WARN, "WDT: stop request not written; it may still reset the board");
    close(s_fd);
    s_fd = -1;
}

int wdt_dev_bootstatus(void)
{
    return s_bootstatus;
}

int wdt_dev_timeout_s(void)
{
    return s_timeout;
}

int wdt_dev_deadline_s(void)
{
    return s_deadline;
}

int wdt_dev_feed_period_ms(void)
{
    return s_feed_period_ms;
}
