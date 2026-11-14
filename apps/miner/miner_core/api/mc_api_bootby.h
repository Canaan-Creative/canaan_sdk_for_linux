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

#ifndef MC_API_BOOTBY_H
#define MC_API_BOOTBY_H

#include <stdint.h>

#define MC_BOOTBY_SUB_WDT   0xF1u /* sub-reason: hardware watchdog reset */
#define MC_BOOTBY_SUB_PANIC 0xF3u /* sub-reason: kernel panic */
/* Watchdog bootstatus bit the K230 dw_wdt driver sets after a kernel panic (WDIOF_EXTERN1) */
#define MC_BOOTBY_BS_PANIC   0x0004
#define MC_BOOTBY_INI_SECT   "syscfg"
#define MC_BOOTBY_INI_REASON "reason"    /* ini key: boot reason */
#define MC_BOOTBY_INI_SUB    "subreason" /* ini key: boot sub-reason */

enum mc_bootby {
    MC_BOOTBY_POWRON            = 0x01,
    MC_BOOTBY_WEB               = 0x02, /* reboot from the web page */
    MC_BOOTBY_API               = 0x03, /* reboot from the API */
    MC_BOOTBY_WDT               = 0x04, /* watchdog pre-timeout */
    MC_BOOTBY_SYS               = 0x06, /* system fault */
    MC_BOOTBY_NUCLEAR_COMM_FAIL = 0x07,
    MC_BOOTBY_POOL_INACTIVE     = 0x08,
    MC_BOOTBY_FAC_RESTORE       = 0x09,
    MC_BOOTBY_RTSYS_FAIL        = 0x0A,
    MC_BOOTBY_NETDEV_OUT        = 0x0B,
    MC_BOOTBY_PERR              = 0x0C,
    MC_BOOTBY_UPGRADE           = 0x0D,
    MC_BOOTBY_NOSHARE           = 0x11,
    MC_BOOTBY_LOW_HASHRATE      = 0x12,
    MC_BOOTBY_HASH_LOST         = 0x13,
};

void        mc_bootby_resolve(const char *reason_text, const char *sub_text, int wdt_bootstatus,
               uint8_t *out_reason, uint32_t *out_sub);
const char *mc_bootby_string(uint8_t reason);

#endif /* MC_API_BOOTBY_H */
