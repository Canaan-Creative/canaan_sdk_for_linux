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

#ifndef WDT_DEV_H
#define WDT_DEV_H

#define WDT_DEV_DEFAULT "/dev/watchdog"
#define WDT_TIMEOUT_S   89 /* requested watchdog timeout, s */
/* non-zero pretimeout request that enables the two-stage mode */
#define WDT_PRETIMEOUT_S       1
#define WDT_FEED_MARGIN        4 /* feeds per granted deadline */
#define WDT_FEED_PERIOD_MS_MIN 1000
#define WDT_FEED_PERIOD_MS_MAX 10000

int  wdt_dev_init(void);
void wdt_dev_feed(void);
void wdt_dev_disarm(void);
int  wdt_dev_bootstatus(void);
int  wdt_dev_timeout_s(void);
int  wdt_dev_deadline_s(void);
int  wdt_dev_feed_period_ms(void);

#endif /* WDT_DEV_H */
