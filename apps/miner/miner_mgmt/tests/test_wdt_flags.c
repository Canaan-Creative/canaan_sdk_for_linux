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

#include <stdio.h>
#include <string.h>
#include "wdt_flags.h"

#include "../../tests/include/tst_check.h"

int main(void)
{
    wdt_flags_t f;
    wdt_flags_t zeroed = { 0 };
    char        buf[128];

    wdt_flags_reset(&f);
    CHECK(wdt_flags_raw(&f) == 0, "a reset gate is empty (raw 0x%x)", wdt_flags_raw(&f));
    CHECK(wdt_flags_mark(&f, WDT_TID_ASIC) == 0 && wdt_flags_raw(&f) != 0,
            "one TID of three does not complete the gate (raw 0x%x)", wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_raw(&f) == (1u << WDT_TID_SUPERVISOR),
            "marking the SAME TID twice is idempotent and never completes the "
            "gate — one live thread cannot feed the dog on the other two's "
            "behalf (raw 0x%x)",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 && wdt_flags_mark(&f, WDT_TID_ASIC) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_CORE) == 1 && wdt_flags_raw(&f) == 0,
            "the LAST of the three returns 1 and the gate self-clears for the next "
            "round (raw 0x%x)",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_mark(&f, WDT_TID_TOTAL) == 1 && wdt_flags_raw(&f) == 0,
            "WDT_TID_TOTAL is the count, not a TID (raw 0x%x)", wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_mark(&f, -1) == -1 && wdt_flags_raw(&f) == 0,
            "a negative TID is refused and marks nothing (raw 0x%x)", wdt_flags_raw(&f));
    CHECK(wdt_flags_mark(&f, WDT_TID_TOTAL + 1) == -1 && wdt_flags_raw(&f) == 0,
            "and so is one past the end (raw 0x%x)", wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_enabled_mask(&f) == ((1u << WDT_TID_TOTAL) - 1u) &&
                    wdt_flags_enabled(&f, WDT_TID_SUPERVISOR) == 1 &&
                    wdt_flags_enabled(&f, WDT_TID_ASIC) == 1 &&
                    wdt_flags_enabled(&f, WDT_TID_CORE) == 1,
            "a reset gate supervises every TID (enabled 0x%x)", wdt_flags_enabled_mask(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_enable(&f, WDT_TID_ASIC, 0) == 0 && wdt_flags_enabled(&f, WDT_TID_ASIC) == 0 &&
                    wdt_flags_enabled_mask(&f) ==
                            ((1u << WDT_TID_SUPERVISOR) | (1u << WDT_TID_CORE)),
            "disabling a TID drops it from the supervised set (enabled 0x%x)",
            wdt_flags_enabled_mask(&f));
    CHECK(wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 && wdt_flags_mark(&f, WDT_TID_CORE) == 1 &&
                    wdt_flags_raw(&f) == 0,
            "completion counts the ENABLED TIDs only, so marking the remaining "
            "two completes the gate — a participant that is not yet supervised "
            "cannot hold the dog hungry (raw 0x%x)",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    wdt_flags_mark(&f, WDT_TID_ASIC);
    CHECK(wdt_flags_enable(&f, WDT_TID_ASIC, 0) == 0 && wdt_flags_raw(&f) == 0,
            "disabling a TID that had ALREADY reported clears its mark too "
            "(raw 0x%x)",
            wdt_flags_raw(&f));
    CHECK(wdt_flags_enable(&f, WDT_TID_ASIC, 1) == 0 && wdt_flags_raw(&f) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_CORE) == 0,
            "so re-enabling it carries no stale report into the new window — the "
            "other two do not complete the gate on their own (raw 0x%x)",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    wdt_flags_mark(&f, WDT_TID_SUPERVISOR);
    CHECK(wdt_flags_enable(&f, WDT_TID_SUPERVISOR, 1) == 0 &&
                    wdt_flags_raw(&f) == (1u << WDT_TID_SUPERVISOR),
            "re-enabling an ALREADY-supervised TID keeps its mark — the "
            "supervisor re-asserts the enable set every tick and must not erase "
            "this window's reports doing it (raw 0x%x)",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    CHECK(wdt_flags_enable(&f, -1, 0) == -1 && wdt_flags_enable(&f, WDT_TID_TOTAL, 0) == -1 &&
                    wdt_flags_enabled_mask(&f) == ((1u << WDT_TID_TOTAL) - 1u),
            "an out-of-range TID can be neither enabled nor disabled — unlike "
            "mark(), WDT_TID_TOTAL is no escape hatch here (enabled 0x%x)",
            wdt_flags_enabled_mask(&f));
    CHECK(wdt_flags_enabled(&f, -1) == 0 && wdt_flags_enabled(&f, WDT_TID_TOTAL) == 0,
            "and an out-of-range TID reads back as unsupervised");

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    wdt_flags_enable(&f, WDT_TID_CORE, 0);
    wdt_flags_reset(&f);
    CHECK(wdt_flags_enabled_mask(&f) == ((1u << WDT_TID_TOTAL) - 1u) &&
                    wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_ASIC) == 0 && wdt_flags_mark(&f, WDT_TID_CORE) == 1,
            "a reset re-supervises everything, so all three are required again "
            "(enabled 0x%x)",
            wdt_flags_enabled_mask(&f));

    wdt_flags_reset(&f);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "supervisor,asic,core") == 0,
            "an empty gate reports all three names, in TID order — this string is "
            "what the crash report and the pretimeout log carry (saw '%s')",
            buf);

    wdt_flags_reset(&f);
    wdt_flags_mark(&f, WDT_TID_SUPERVISOR);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "asic,core") == 0, "one marked leaves the other two named (saw '%s')", buf);

    wdt_flags_reset(&f);
    wdt_flags_mark(&f, WDT_TID_SUPERVISOR);
    wdt_flags_mark(&f, WDT_TID_ASIC);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "core") == 0, "two marked names exactly the culprit (saw '%s')", buf);

    CHECK(wdt_flags_mark(&f, WDT_TID_CORE) == 1 && wdt_flags_raw(&f) == 0,
            "completing that gate clears it (raw 0x%x)", wdt_flags_raw(&f));
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "supervisor,asic,core") == 0,
            "so the very next report reads all three missing again, not none — a "
            "completed round is indistinguishable from a fresh one (saw '%s')",
            buf);

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "supervisor,core") == 0,
            "a DISABLED TID is never named missing — it is not unaccounted for, "
            "it is not being supervised, and the diagnostic must not claim "
            "otherwise in either direction (saw '%s')",
            buf);

    wdt_flags_mark(&f, WDT_TID_SUPERVISOR);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "core") == 0,
            "one of the two supervised TIDs marked leaves the report naming "
            "exactly the culprit (saw '%s')",
            buf);

    wdt_flags_mark(&f, WDT_TID_ASIC);
    wdt_flags_missing(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "core") == 0,
            "and a mark arriving for the disabled TID changes nothing the report "
            "says (saw '%s')",
            buf);

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    CHECK(wdt_flags_mark(&f, WDT_TID_ASIC) == 0 && wdt_flags_raw(&f) == 0,
            "a report that arrives for a DISABLED TID is DISCARDED, not deferred: "
            "the bit never enters the mask at all (raw 0x%x). Drop the "
            "`& f->enabled` from wdt_flags_mark() and the mask reads 0x%x here — a "
            "graced child that is answering sets its bit on every tick of its "
            "window, and the window is the one place nothing clears it on the way "
            "out",
            wdt_flags_raw(&f), 1u << WDT_TID_ASIC);
    CHECK(wdt_flags_enable(&f, WDT_TID_ASIC, 1) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_CORE) == 0,
            "...so when the window closes, the first SUPERVISED cycle has to be "
            "earned: the other two do not complete the gate on their own (raw "
            "0x%x). With the bit deferred instead of discarded the second mark "
            "returns 1 on the stale bit, and the gate publishes \"all three "
            "reported\" for a cycle that carried no report from that TID",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    CHECK(wdt_flags_mark(&f, WDT_TID_TOTAL) == 1 && wdt_flags_raw(&f) == 0,
            "WDT_TID_TOTAL sets the ENABLED bits only and still completes the gate "
            "on them (raw 0x%x): mark() ANDs against the enable set, so a "
            "completion test written against all three TIDs instead of "
            "`f->enabled` could never be satisfied again once anything is "
            "disabled — the two halves of the gate have to agree on which set "
            "they count",
            wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    wdt_flags_enable(&f, WDT_TID_CORE, 0);
    CHECK(wdt_flags_mark(&f, WDT_TID_TOTAL) == 1 && wdt_flags_raw(&f) == 0 &&
                    wdt_flags_enabled_mask(&f) == (1u << WDT_TID_SUPERVISOR),
            "...and with two of the three disabled it completes on the one that "
            "remains (enabled 0x%x, raw 0x%x)",
            wdt_flags_enabled_mask(&f), wdt_flags_raw(&f));

    wdt_flags_reset(&f);
    wdt_flags_enable(&f, WDT_TID_SUPERVISOR, 0);
    wdt_flags_enable(&f, WDT_TID_ASIC, 0);
    wdt_flags_enable(&f, WDT_TID_CORE, 0);
    CHECK(wdt_flags_enabled_mask(&f) == 0 && wdt_flags_mark(&f, WDT_TID_SUPERVISOR) == 0 &&
                    wdt_flags_mark(&f, WDT_TID_ASIC) == 0 && wdt_flags_mark(&f, WDT_TID_CORE) == 0,
            "an EMPTY enable set NEVER completes (enabled 0x%x): supervising "
            "nobody is not the same as everybody having reported. Delete the "
            "`f->enabled != 0 &&` from the completion test and `mask & 0 == 0` "
            "holds, so every single mark completes the gate",
            wdt_flags_enabled_mask(&f));

    CHECK(wdt_flags_mark(&zeroed, WDT_TID_SUPERVISOR) == 0 && wdt_flags_raw(&zeroed) == 0 &&
                    wdt_flags_enabled_mask(&zeroed) == 0,
            "...and the zero-initialised gate is the reason that arm exists, "
            "because \"somebody forgot wdt_flags_reset()\" is a thing that "
            "happens (raw 0x%x, enabled 0x%x). Without it such a gate would hold "
            "/dev/watchdog, feed it on schedule, report nothing missing and "
            "supervise nothing — with no symptom anywhere",
            wdt_flags_raw(&zeroed), wdt_flags_enabled_mask(&zeroed));

    {
        wdt_flags_t back;
        uint32_t    packed;

        wdt_flags_reset(&f);
        wdt_flags_enable(&f, WDT_TID_ASIC, 0);
        wdt_flags_mark(&f, WDT_TID_SUPERVISOR);
        packed = wdt_flags_pack(&f);
        wdt_flags_unpack(&back, packed);
        CHECK(back.mask == f.mask && back.enabled == f.enabled &&
                        (packed & ((1u << WDT_FLAGS_ENABLED_SHIFT) - 1u)) == f.mask &&
                        (packed >> WDT_FLAGS_ENABLED_SHIFT) == f.enabled,
                "a gate packs into ONE word — report mask low, supervised set "
                "high — and unpacks back to itself (0x%08x: mask 0x%x, enabled "
                "0x%x). One word because the fatal-signal handler reads it with a "
                "single load: two variables would let it see this round's mask "
                "against the last round's enable set",
                packed, wdt_flags_raw(&back), wdt_flags_enabled_mask(&back));

        wdt_flags_missing(&back, buf, sizeof(buf));
        CHECK(strcmp(buf, "core") == 0,
                "...so a reader that only has the published word still names the "
                "supervised TID that owes a report, and not the disabled one (saw "
                "'%s')",
                buf);

        wdt_flags_unpack(&back, f.mask);
        wdt_flags_missing(&back, buf, sizeof(buf));
        CHECK(wdt_flags_enabled_mask(&back) == 0 && buf[0] == '\0',
                "...and a value carrying the MASK ALONE supervises nobody, so it can "
                "name nobody (enabled 0x%x, saw '%s'). A reconstruction that "
                "calls wdt_flags_reset() first hides that, since it supervises "
                "EVERYTHING: the missing half is not carried but guessed, and the "
                "guess names exempt participants",
                wdt_flags_enabled_mask(&back), buf);
    }

    printf(fails ? "FAILED (%d)\n" : "wdt-flags goldens OK\n", fails);
    return fails ? 1 : 0;
}
