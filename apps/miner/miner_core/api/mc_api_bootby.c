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

#include "mc_api_bootby.h"

#include <stddef.h>
#include <stdlib.h>

struct bootby_str {
    uint8_t     reason; /* enum mc_bootby */
    const char *s;      /* display name */
};

static const struct bootby_str bootby[] = {
    { MC_BOOTBY_POWRON, "PowerOn" },
    { MC_BOOTBY_WEB, "WEB" },
    { MC_BOOTBY_API, "API" },
    { MC_BOOTBY_WDT, "WDT" },
    { MC_BOOTBY_SYS, "System" },
    { MC_BOOTBY_NUCLEAR_COMM_FAIL, "CommFail" },
    { MC_BOOTBY_NOSHARE, "NOShare" },
    { MC_BOOTBY_FAC_RESTORE, "FacRestore" },
    { MC_BOOTBY_RTSYS_FAIL, "RTSysFail" },
    { MC_BOOTBY_POOL_INACTIVE, "PoolInactive" },
    { MC_BOOTBY_LOW_HASHRATE, "LowHashrate" },
    { MC_BOOTBY_PERR, "PowErr" },
    { MC_BOOTBY_UPGRADE, "Upgrade" },
    { MC_BOOTBY_NETDEV_OUT, "NetdevOut" },
    { MC_BOOTBY_HASH_LOST, "HashLost" },
};

void mc_bootby_resolve(const char *reason_text, const char *sub_text, int wdt_bootstatus,
        uint8_t *out_reason, uint32_t *out_sub)
{
    uint8_t  reason = MC_BOOTBY_POWRON;
    uint32_t sub    = 0;

    if (reason_text)
        reason = (uint8_t)strtoul(reason_text, NULL, 0);
    if (sub_text)
        sub = (uint32_t)strtoul(sub_text, NULL, 0);

    if (reason == 0 || reason == MC_BOOTBY_POWRON) {
        if (wdt_bootstatus & MC_BOOTBY_BS_PANIC) {
            reason = MC_BOOTBY_SYS;
            sub    = MC_BOOTBY_SUB_PANIC;
        } else if (wdt_bootstatus != 0) {
            reason = MC_BOOTBY_SYS;
            sub    = MC_BOOTBY_SUB_WDT;
        } else {
            reason = MC_BOOTBY_POWRON;
            sub    = 0;
        }
    }

    if (out_reason)
        *out_reason = reason;
    if (out_sub)
        *out_sub = sub;
}

const char *mc_bootby_string(uint8_t reason)
{
    for (size_t i = 0; i < sizeof bootby / sizeof bootby[0]; i++)
        if (reason == bootby[i].reason)
            return bootby[i].s;
    return "Unknown";
}
