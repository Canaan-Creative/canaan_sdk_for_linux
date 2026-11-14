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

#include "enum_hold.h"
#include <stdio.h>

#include "../../tests/include/tst_check.h"

int main(void)
{
    enum_hold_t p = { 0 };
    CHECK(enum_hold_step(&p, 0, 100) == ENUM_HOLD_NONE,
            "zero-init: nothing is pinned until a spawn arms it");
    enum_hold_rearm(&p);
    CHECK(enum_hold_step(&p, 0, 100) == ENUM_HOLD_PIN && enum_hold_active(&p, 0),
            "armed -> PIN, and the fan rule then holds it");
    CHECK(enum_hold_step(&p, 0, 100) == ENUM_HOLD_NONE && enum_hold_active(&p, 0),
            "...pinned once, not once per tick");
    CHECK(enum_hold_step(&p, 1, 100) == ENUM_HOLD_RELEASE && !enum_hold_active(&p, 0),
            "...and the heartbeat releases it exactly once");
    CHECK(enum_hold_step(&p, 1, 100) == ENUM_HOLD_NONE && p.phase == 0,
            "...leaving the zero-init state, so nothing re-arms by itself");

    p = (enum_hold_t){ 0 };
    enum_hold_rearm(&p);
    CHECK(enum_hold_step(&p, 0, 0) == ENUM_HOLD_NONE && p.phase == 0,
            "enum duty 0: an armed pin is forced back to zero-init");
    CHECK(enum_hold_step(&p, 1, 0) == ENUM_HOLD_NONE && !enum_hold_active(&p, 0),
            "...and neither pins nor holds, ever");

    p = (enum_hold_t){ 0 };
    enum_hold_rearm(&p);
    enum_hold_step(&p, 0, 100);
    CHECK(!enum_hold_active(&p, 1),
            "a soft-off or thermal hold (held_off) REPLACES the pin (gate stands down)");
    CHECK(enum_hold_active(&p, 0), "...and the phase itself was never lost");

    if (fails) {
        fprintf(stderr, "test_enum_hold: %d FAIL\n", fails);
        return 1;
    }
    printf("test_enum_hold: all pass\n");
    return 0;
}
