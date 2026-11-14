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

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/watchdog.h>

#include "osif.h"
#include "wdt_dev.h"

#undef WDT_DEV_DEFAULT
#define WDT_DEV_DEFAULT  TST_WDT_DEV /* watchdog device path overridden to the test's TST_WDT_DEV */
#define WDT_TST_MAX_TOPS 16          /* capacity of the TOP period grid */

static int wdt_tst_ioctl(int fd, unsigned long req, void *arg);
#define ioctl wdt_tst_ioctl
#include "../platform/linux/wdt_dev.c"
#undef ioctl

/* Simulated watchdog device */
static struct {
    int use_dw;                 /* 1 models dw_wdt TOP rounding, 0 ignores set requests */
    int grid[WDT_TST_MAX_TOPS]; /* selectable TOP periods, s, ascending */
    int ngrid;
    int rmod;                 /* 2 while a pretimeout is armed, else 1 */
    int timeout;              /* value WDIOC_GETTIMEOUT returns, s */
    int pretimeout;           /* value WDIOC_GETPRETIMEOUT returns, s */
    int pretimeout_supported; /* 0 makes WDIOC_SETPRETIMEOUT fail */
    int settimeout_fails;     /* non-zero makes WDIOC_SETTIMEOUT fail */
    int gettimeout_fails;     /* non-zero makes WDIOC_GETTIMEOUT fail */
    int bootstatus;           /* value WDIOC_GETBOOTSTATUS returns */
    int n_keepalive;          /* WDIOC_KEEPALIVE calls */
} dev;
static int g_saw_err;
static int g_saw_warn;
static int g_saw_info;
/* dw_wdt TOP periods, s */
static const int k230_24mhz[] = { 1, 2, 5, 11, 22, 44, 89, 178 };

static int dev_max_hw_ms(void)
{
    return dev.grid[dev.ngrid - 1] * 1000;
}

static void dev_set_timeout(int top_s)
{
    int req = (top_s + dev.rmod - 1) / dev.rmod;
    int i, top = dev.grid[dev.ngrid - 1];

    for (i = 0; i < dev.ngrid; ++i) {
        if (dev.grid[i] >= req) {
            top = dev.grid[i];
            break;
        }
    }
    dev.pretimeout = (dev.rmod == 2) ? top : 0;
    dev.timeout    = (top_s * 1000 <= dev_max_hw_ms()) ? top * dev.rmod : top_s;
}

static int wdt_tst_ioctl(int fd, unsigned long req, void *arg)
{
    int *v = (int *)arg;

    (void)fd;
    switch (req) {
    case WDIOC_SETTIMEOUT:
        if (dev.settimeout_fails)
            return -1;
        if (dev.use_dw)
            dev_set_timeout(*v);
        return 0;
    case WDIOC_GETTIMEOUT:
        if (dev.gettimeout_fails)
            return -1;
        *v = dev.timeout;
        return 0;
    case WDIOC_SETPRETIMEOUT:
        if (!dev.pretimeout_supported)
            return -1;
        if (dev.use_dw) {
            dev.rmod = *v ? 2 : 1;
            dev_set_timeout(dev.timeout);
        }
        return 0;
    case WDIOC_GETPRETIMEOUT:
        *v = dev.pretimeout;
        return 0;
    case WDIOC_GETBOOTSTATUS:
        *v = dev.bootstatus;
        return 0;
    case WDIOC_KEEPALIVE:
        dev.n_keepalive++;
        return 0;
    case WDIOC_SETOPTIONS:
        return 0;
    default:
        return -1;
    }
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl == OSIF_LOG_ERR)
        g_saw_err = 1;
    if (lvl == OSIF_LOG_WARN)
        g_saw_warn = 1;
    if (lvl == OSIF_LOG_INFO)
        g_saw_info = 1;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void node_create(void)
{
    int fd = open(WDT_DEV_DEFAULT, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    close(fd);
}

static void dev_reset(void)
{
    memset(&dev, 0, sizeof dev);
    dev.rmod                 = 1;
    dev.pretimeout_supported = 1;
    g_saw_err                = 0;
    g_saw_warn               = 0;
    g_saw_info               = 0;
}

static void dev_dw(const int *grid, int n)
{
    assert(n <= WDT_TST_MAX_TOPS);
    dev.use_dw = 1;
    memcpy(dev.grid, grid, (size_t)n * sizeof(int));
    dev.ngrid = n;
}

static int run_init(void)
{
    return wdt_dev_init();
}

static void test_k230_grid(void)
{
    dev_reset();
    dev_dw(k230_24mhz, (int)(sizeof k230_24mhz / sizeof k230_24mhz[0]));
    assert(run_init() == 0);

    assert(dev.timeout == 178);
    assert(dev.pretimeout == 89);
    assert(wdt_dev_timeout_s() == 178);
    assert(wdt_dev_deadline_s() == 89);
    assert(wdt_dev_feed_period_ms() == WDT_FEED_PERIOD_MS_MAX);
    assert(!g_saw_err);
    assert(!g_saw_warn);
    wdt_dev_disarm();
}

static void test_refused_timeout_is_a_warning(void)
{
    dev_reset();
    dev_dw(k230_24mhz, (int)(sizeof k230_24mhz / sizeof k230_24mhz[0]));
    dev.settimeout_fails = 1;
    dev.timeout          = 44;
    assert(run_init() == 0);

    assert(g_saw_warn);
    assert(!g_saw_err);
    assert(wdt_dev_timeout_s() == 44);
    assert(wdt_dev_deadline_s() == 22);
    assert(wdt_dev_feed_period_ms() == 5500);
    wdt_dev_disarm();
}

static void test_k230_grid_without_pretimeout(void)
{
    dev_reset();
    dev_dw(k230_24mhz, (int)(sizeof k230_24mhz / sizeof k230_24mhz[0]));
    dev.pretimeout_supported = 0;
    assert(run_init() == 0);

    assert(dev.timeout == 89);
    assert(wdt_dev_timeout_s() == 89);
    assert(wdt_dev_deadline_s() == 89);
    assert(wdt_dev_feed_period_ms() == WDT_FEED_PERIOD_MS_MAX);
    assert(!g_saw_err);
    wdt_dev_disarm();
}

static void test_short_timeout_shortens_the_feed_period(void)
{
    dev_reset();
    dev.timeout = 20;
    assert(run_init() == 0);

    assert(wdt_dev_feed_period_ms() == 5000);
    assert(!g_saw_err);
    wdt_dev_disarm();
}

static void test_pretimeout_is_the_deadline(void)
{
    dev_reset();
    dev.timeout    = 20;
    dev.pretimeout = 10;
    assert(run_init() == 0);

    assert(wdt_dev_timeout_s() == 20);
    assert(wdt_dev_deadline_s() == 10);
    assert(wdt_dev_feed_period_ms() == 2500);
    assert(!g_saw_err);
    wdt_dev_disarm();
}

static void test_unusable_deadline_is_an_error(void)
{
    dev_reset();
    dev.timeout = 3;
    assert(run_init() == 0);

    assert(wdt_dev_feed_period_ms() == WDT_FEED_PERIOD_MS_MIN);
    assert(g_saw_err);
    wdt_dev_disarm();
}

static void test_unreadable_timeout_is_an_error(void)
{
    dev_reset();
    dev.gettimeout_fails = 1;
    assert(run_init() == 0);

    assert(wdt_dev_timeout_s() == 0);
    assert(wdt_dev_deadline_s() == 0);
    assert(wdt_dev_feed_period_ms() == WDT_FEED_PERIOD_MS_MIN);
    assert(g_saw_err);
    wdt_dev_disarm();
}

static void test_cardreset_is_the_bootstatus(void)
{
    dev_reset();
    dev_dw(k230_24mhz, (int)(sizeof k230_24mhz / sizeof k230_24mhz[0]));
    dev.bootstatus = WDIOF_CARDRESET;
    assert(run_init() == 0);

    assert(wdt_dev_bootstatus() == WDIOF_CARDRESET);
    assert(g_saw_info);
    wdt_dev_disarm();

    dev_reset();
    dev_dw(k230_24mhz, (int)(sizeof k230_24mhz / sizeof k230_24mhz[0]));
    assert(run_init() == 0);

    assert(wdt_dev_bootstatus() == 0);
    wdt_dev_disarm();
}

static void test_absent_device_keeps_the_default_period(void)
{
    dev_reset();
    int rc = unlink(WDT_DEV_DEFAULT);
    assert(rc == 0);
    assert(run_init() == -1);

    assert(wdt_dev_timeout_s() == 0);
    assert(wdt_dev_deadline_s() == 0);
    assert(wdt_dev_feed_period_ms() == WDT_FEED_PERIOD_MS_MAX);
    assert(g_saw_err);
}

int main(void)
{
    unlink(WDT_DEV_DEFAULT);
    node_create();

    test_k230_grid();
    test_refused_timeout_is_a_warning();
    test_k230_grid_without_pretimeout();
    test_short_timeout_shortens_the_feed_period();
    test_pretimeout_is_the_deadline();
    test_unusable_deadline_is_an_error();
    test_unreadable_timeout_is_an_error();
    test_cardreset_is_the_bootstatus();
    test_absent_device_keeps_the_default_period();

    printf("test_wdt_dev: all cases passed\n");
    return 0;
}
