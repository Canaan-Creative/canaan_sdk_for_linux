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
#include "mc_api_bootby.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

static void resolve(const char *reason, const char *sub, int wdt, uint8_t *r, uint32_t *s)
{
    *r = 0xff;
    *s = 0xffffffffu;
    mc_bootby_resolve(reason, sub, wdt, r, s);
}

int main(void)
{
    uint8_t  r   = 0;
    uint32_t sub = 0;
    char     sig[16];

    CHECK(MC_BOOTBY_SUB_WDT == 0xF1u && MC_BOOTBY_SUB_PANIC == 0xF3u,
            "watchdog and panic sub codes are 0xF1 and 0xF3 (saw 0x%X/0x%X)",
            (unsigned)MC_BOOTBY_SUB_WDT, (unsigned)MC_BOOTBY_SUB_PANIC);
    CHECK(MC_BOOTBY_POWRON == 0x01 && MC_BOOTBY_WEB == 0x02 && MC_BOOTBY_API == 0x03 &&
                    MC_BOOTBY_WDT == 0x04 && MC_BOOTBY_SYS == 0x06 &&
                    MC_BOOTBY_NUCLEAR_COMM_FAIL == 0x07 && MC_BOOTBY_POOL_INACTIVE == 0x08 &&
                    MC_BOOTBY_FAC_RESTORE == 0x09 && MC_BOOTBY_RTSYS_FAIL == 0x0A &&
                    MC_BOOTBY_NETDEV_OUT == 0x0B && MC_BOOTBY_PERR == 0x0C &&
                    MC_BOOTBY_UPGRADE == 0x0D && MC_BOOTBY_NOSHARE == 0x11 &&
                    MC_BOOTBY_LOW_HASHRATE == 0x12 && MC_BOOTBY_HASH_LOST == 0x13,
            "boot reason codes keep their BOOTBY wire values");

    snprintf(sig, sizeof sig, "%d", SIGSEGV);
    resolve("6", sig, 0, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == (uint32_t)SIGSEGV,
            "a stored crash SYS/%u resolves to MC_BOOTBY_SYS / sub %u, so BOOTBY "
            "names the signal instead of reporting a power cut (saw %u/%u)",
            (unsigned)SIGSEGV, (unsigned)SIGSEGV, (unsigned)r, (unsigned)sub);

    resolve("1", "0", 0, &r, &sub);
    CHECK(r == MC_BOOTBY_POWRON && sub == 0,
            "a stored POWRON/0 resolves to a plain power-on (saw %u/%u)", (unsigned)r,
            (unsigned)sub);

    resolve("13", "0", 1, &r, &sub);
    CHECK(r == MC_BOOTBY_UPGRADE && sub == 0,
            "a stored reason outranks the watchdog bootstatus — the wdt arm is "
            "only reached from 0 or POWRON (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    snprintf(sig, sizeof sig, "%d", SIGABRT);
    resolve("6", sig, 1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == (uint32_t)SIGABRT && sub != MC_BOOTBY_SUB_WDT,
            "with BOTH a crash record and a watchdog bootstatus the SIGNAL wins: "
            "SYS/%u, not the generic SYS/0xF1 (saw %u/%u)",
            (unsigned)SIGABRT, (unsigned)r, (unsigned)sub);

    resolve(NULL, NULL, 0, &r, &sub);
    CHECK(r == MC_BOOTBY_POWRON && sub == 0, "no record, no watchdog: POWRON/0 (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    resolve(NULL, NULL, 1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == 0xF1u,
            "no record, watchdog bootstatus set: SYS/0xF1 (saw %u/%u)", (unsigned)r, (unsigned)sub);

    resolve(NULL, NULL, MC_BOOTBY_BS_PANIC, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == MC_BOOTBY_SUB_PANIC,
            "no record, the kernel-panic bootstatus bit set: SYS/0xF3, told apart "
            "from a watchdog reset (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    resolve("1", "0", 0x20 | MC_BOOTBY_BS_PANIC, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == MC_BOOTBY_SUB_PANIC,
            "a stored POWRON with the panic bit among others is a panic (saw %u/%u)", (unsigned)r,
            (unsigned)sub);

    resolve("13", "0", MC_BOOTBY_BS_PANIC, &r, &sub);
    CHECK(r == MC_BOOTBY_UPGRADE && sub == 0,
            "a stored reason outranks the panic bit too (saw %u/%u)", (unsigned)r, (unsigned)sub);

    resolve("0", "0", 1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == 0xF1u,
            "a stored reason of 0 is 'no record', not a reason: the watchdog arm "
            "still applies (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    resolve("1", "7", 1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == 0xF1u,
            "a stored POWRON with a watchdog bootstatus is the watchdog arm, its "
            "stale subreason dropped (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    resolve("1", "7", 0, &r, &sub);
    CHECK(r == MC_BOOTBY_POWRON && sub == 0,
            "a stored POWRON without a watchdog bootstatus drops a stale subreason "
            "(saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    resolve("6", NULL, 0, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == 0,
            "a reason without a stored subreason resolves with sub 0 (saw %u/%u)", (unsigned)r,
            (unsigned)sub);

    resolve("0x0D", "0x10", 0, &r, &sub);
    CHECK(r == MC_BOOTBY_UPGRADE && sub == 0x10u,
            "stored values are read with base prefixes, as strtoul base 0 does "
            "(saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    mc_bootby_resolve("6", "11", 0, NULL, NULL);
    CHECK(1, "NULL outputs are skipped");

    {
        static const struct {
            uint8_t     r;
            const char *want;
        } c[] = {
            { MC_BOOTBY_POWRON, "PowerOn" },
            { MC_BOOTBY_WEB, "WEB" },
            { MC_BOOTBY_API, "API" },
            { MC_BOOTBY_WDT, "WDT" },
            { MC_BOOTBY_SYS, "System" },
            { MC_BOOTBY_NUCLEAR_COMM_FAIL, "CommFail" },
            { MC_BOOTBY_POOL_INACTIVE, "PoolInactive" },
            { MC_BOOTBY_FAC_RESTORE, "FacRestore" },
            { MC_BOOTBY_RTSYS_FAIL, "RTSysFail" },
            { MC_BOOTBY_NETDEV_OUT, "NetdevOut" },
            { MC_BOOTBY_PERR, "PowErr" },
            { MC_BOOTBY_UPGRADE, "Upgrade" },
            { MC_BOOTBY_NOSHARE, "NOShare" },
            { MC_BOOTBY_LOW_HASHRATE, "LowHashrate" },
            { MC_BOOTBY_HASH_LOST, "HashLost" },
            { 0x05, "Unknown" },
            { 0x0E, "Unknown" },
            { 0x00, "Unknown" },
        };

        for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
            CHECK(strcmp(mc_bootby_string(c[i].r), c[i].want) == 0,
                    "boot reason 0x%02x reads \"%s\" (saw \"%s\")", (unsigned)c[i].r, c[i].want,
                    mc_bootby_string(c[i].r));
    }

    return tst_done();
}
