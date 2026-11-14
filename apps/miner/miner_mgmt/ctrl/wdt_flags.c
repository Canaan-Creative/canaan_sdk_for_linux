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

#include "wdt_flags.h"

#include <string.h>

#define WDT_FLAGS_MASK_ALL (((uint32_t)1u << WDT_TID_TOTAL) - 1u) /* every WDT_TID_* bit */

/* log name of each WDT_TID_* */
static const char *const WDT_TID_NAMES[] = {
    "supervisor",
    "asic",
    "core",
};

_Static_assert(sizeof WDT_TID_NAMES / sizeof WDT_TID_NAMES[0] == WDT_TID_TOTAL,
        "WDT_TID_NAMES must name every WDT_TID");

_Static_assert(WDT_TID_TOTAL <= WDT_FLAGS_ENABLED_SHIFT,
        "the packed gate carries the mask in the low half and the enable "
        "set in the high half of one word: the fatal-signal handler reads "
        "it with a single load, so the two halves cannot be allowed to "
        "overlap — widen WDT_FLAGS_ENABLED_SHIFT before adding a TID");

void wdt_flags_reset(wdt_flags_t *f)
{
    if (f == NULL)
        return;
    f->mask    = 0;
    f->enabled = WDT_FLAGS_MASK_ALL;
}

uint32_t wdt_flags_pack(const wdt_flags_t *f)
{
    if (f == NULL)
        return 0;
    return (f->mask & WDT_FLAGS_MASK_ALL) |
            ((f->enabled & WDT_FLAGS_MASK_ALL) << WDT_FLAGS_ENABLED_SHIFT);
}

void wdt_flags_unpack(wdt_flags_t *f, uint32_t packed)
{
    if (f == NULL)
        return;
    f->mask    = packed & WDT_FLAGS_MASK_ALL;
    f->enabled = (packed >> WDT_FLAGS_ENABLED_SHIFT) & WDT_FLAGS_MASK_ALL;
}

int wdt_flags_enable(wdt_flags_t *f, int tid, int on)
{
    if (f == NULL || tid < 0 || tid >= WDT_TID_TOTAL)
        return -1;
    if (on)
        f->enabled |= (uint32_t)1u << tid;
    else {
        f->enabled &= ~((uint32_t)1u << tid);
        f->mask &= ~((uint32_t)1u << tid);
    }
    return 0;
}

int wdt_flags_enabled(const wdt_flags_t *f, int tid)
{
    if (f == NULL || tid < 0 || tid >= WDT_TID_TOTAL)
        return 0;
    return (f->enabled & ((uint32_t)1u << tid)) != 0;
}

const char *wdt_flags_tid_name(int tid)
{
    if (tid < 0 || tid >= WDT_TID_TOTAL || WDT_TID_NAMES[tid] == NULL)
        return "?";
    return WDT_TID_NAMES[tid];
}

int wdt_flags_mark(wdt_flags_t *f, int tid)
{
    if (f == NULL || tid < 0 || tid > WDT_TID_TOTAL)
        return -1;

    if (tid < WDT_TID_TOTAL)
        f->mask |= ((uint32_t)1u << tid) & f->enabled;
    else
        f->mask |= WDT_FLAGS_MASK_ALL & f->enabled;

    if (f->enabled != 0 && (f->mask & f->enabled) == f->enabled) {
        f->mask = 0;
        return 1;
    }
    return 0;
}

uint32_t wdt_flags_enabled_mask(const wdt_flags_t *f)
{
    return f != NULL ? f->enabled : 0;
}

uint32_t wdt_flags_raw(const wdt_flags_t *f)
{
    return f != NULL ? f->mask : 0;
}

void wdt_flags_missing(const wdt_flags_t *f, char *buf, size_t cap)
{
    size_t len = 0;
    int    tid;

    if (buf == NULL || cap == 0)
        return;
    buf[0] = '\0';
    if (f == NULL)
        return;

    for (tid = 0; tid < WDT_TID_TOTAL; tid++) {
        size_t n;
        size_t need;

        if (!(f->enabled & ((uint32_t)1u << tid)))
            continue;
        if (f->mask & ((uint32_t)1u << tid))
            continue;

        const char *name = WDT_TID_NAMES[tid] ? WDT_TID_NAMES[tid] : "?";
        n                = strlen(name);
        need             = n + (len > 0 ? 1 : 0);
        if (len + need >= cap)
            break;

        if (len > 0)
            buf[len++] = ',';
        memcpy(buf + len, name, n);
        len += n;
        buf[len] = '\0';
    }
}
