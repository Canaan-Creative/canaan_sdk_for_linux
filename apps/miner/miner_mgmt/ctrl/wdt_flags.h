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

#ifndef WDT_FLAGS_H
#define WDT_FLAGS_H

#include <stddef.h>
#include <stdint.h>

#define WDT_FLAGS_ENABLED_SHIFT 16 /* bit position of the enable set in the packed gate word */

/* watchdog feed gate leg */
enum {
    WDT_TID_SUPERVISOR = 0, /* mm_miner main loop */
    WDT_TID_ASIC,           /* asic_miner */
    WDT_TID_CORE,           /* core_miner */
    WDT_TID_TOTAL,
};

typedef struct wdt_flags {
    uint32_t mask;    /* legs reported since the last complete round, bit per WDT_TID_* */
    uint32_t enabled; /* legs the gate waits for, bit per WDT_TID_* */
} wdt_flags_t;

void        wdt_flags_reset(wdt_flags_t *f);
uint32_t    wdt_flags_pack(const wdt_flags_t *f);
void        wdt_flags_unpack(wdt_flags_t *f, uint32_t packed);
int         wdt_flags_enable(wdt_flags_t *f, int tid, int on);
int         wdt_flags_enabled(const wdt_flags_t *f, int tid);
const char *wdt_flags_tid_name(int tid);
uint32_t    wdt_flags_enabled_mask(const wdt_flags_t *f);
int         wdt_flags_mark(wdt_flags_t *f, int tid);
uint32_t    wdt_flags_raw(const wdt_flags_t *f);
void        wdt_flags_missing(const wdt_flags_t *f, char *buf, size_t cap);

#endif /* WDT_FLAGS_H */
