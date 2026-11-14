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

#include "thermal.h"
#include <stdio.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

int main(void)
{
    thermal_params_t pp = { .over_temp = 113, .outlet_over_temp = 95, .outlet_over_count = 5 };
    thermal_softoff_state_t ss = { 0 };

    CHECK(thermal_softoff_check(&pp, &ss, 114, NULL, 0, 1) == 1,
            "above threshold while mining -> trigger");
    CHECK(thermal_softoff_check(&pp, &ss, 113, NULL, 0, 1) == 0,
            "exactly at threshold -> no trigger (strictly greater)");
    CHECK(thermal_softoff_check(&pp, &ss, 200, NULL, 0, 1) == 1, "well above -> trigger");
    CHECK(thermal_softoff_check(&pp, &ss, 120, NULL, 0, 0) == 0,
            "hot but not mining -> no trigger");
    CHECK(thermal_softoff_check(&pp, &ss, 50, NULL, 0, 1) == 0, "cool while mining -> no trigger");
    CHECK(thermal_softoff_check(&pp, &ss, ASIC_TEMP_NONE, NULL, 0, 1) == 0,
            "no chip read back -> temp_max is the sentinel -> no trigger");

    const int cool     = 50;
    int       hot1[1]  = { 96 };
    int       cool1[1] = { 50 };
    int       none1[1] = { ASIC_TEMP_NONE };

    {
        thermal_params_t        off   = { .over_temp = 113 };
        thermal_softoff_state_t st    = { 0 };
        int                     trips = 0;
        for (int i = 0; i < 10; i++)
            trips += thermal_softoff_check(&off, &st, cool, hot1, 1, 1);
        CHECK(trips == 0, "outlet_over_count 0 -> arm disabled, never trips");
    }

    {
        thermal_softoff_state_t st    = { 0 };
        int                     at[1] = { 95 };
        int                     trips = 0;
        for (int i = 0; i < 50; i++)
            trips += thermal_softoff_check(&pp, &st, cool, at, 1, 1);
        CHECK(trips == 0, "outlet at/below threshold -> never trips (strictly greater)");
    }

    {
        thermal_softoff_state_t st    = { 0 };
        int                     trips = 0;
        for (int i = 0; i < 4; i++)
            trips += thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(trips == 0, "4 consecutive hot outlet samples -> no trip");
        CHECK(thermal_softoff_check(&pp, &st, cool, hot1, 1, 1) == 1,
                "...the 5th consecutive hot sample trips");
        CHECK(st.outlet_hot_run == 0, "...and an executed trip clears the run");
        trips = 0;
        for (int i = 0; i < 4; i++)
            trips += thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(trips == 0, "...after the trip the next 4 hot samples do not re-trip");
    }

    {
        thermal_softoff_state_t st = { 0 };
        for (int i = 0; i < 4; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(thermal_softoff_check(&pp, &st, cool, cool1, 1, 1) == 0 && st.outlet_hot_run == 0,
                "a cool sample mid-run resets the count");
        int trips = 0;
        for (int i = 0; i < 4; i++)
            trips += thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(trips == 0, "...so 4 more hot samples still do not trip");
        CHECK(thermal_softoff_check(&pp, &st, cool, hot1, 1, 1) == 1,
                "...the run restarted, and the 5th of the NEW run trips");
    }

    {
        thermal_softoff_state_t st    = { 0 };
        int                     trips = 0;
        for (int i = 0; i < 20; i++)
            trips += thermal_softoff_check(&pp, &st, cool, none1, 1, 1);
        CHECK(trips == 0 && st.outlet_hot_run == 0,
                "all sensors -273 -> never hot, and the run stays at 0");

        for (int i = 0; i < 4; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(thermal_softoff_check(&pp, &st, cool, none1, 1, 1) == 0 && st.outlet_hot_run == 0,
                "a -273 mid-run resets it like a cool reading (max semantics)");
    }
    {
        thermal_softoff_state_t st = { 0 };
        for (int i = 0; i < 4; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(thermal_softoff_check(&pp, &st, cool, NULL, 0, 1) == 0 && st.outlet_hot_run == 0,
                "zero boards -> max is the sentinel -> resets the run");
    }

    {
        thermal_softoff_state_t st       = { 0 };
        int                     mixed[3] = { 50, 96, 60 };
        int                     trips    = 0;
        for (int i = 0; i < 4; i++)
            trips += thermal_softoff_check(&pp, &st, cool, mixed, 3, 1);
        CHECK(trips == 0 && st.outlet_hot_run == 4, "one hot board of three drives the max");
        CHECK(thermal_softoff_check(&pp, &st, cool, mixed, 3, 1) == 1, "...and trips on the 5th");

        thermal_softoff_state_t st2          = { 0 };
        int                     with_none[3] = { ASIC_TEMP_NONE, 96, ASIC_TEMP_NONE };
        for (int i = 0; i < 4; i++)
            (void)thermal_softoff_check(&pp, &st2, cool, with_none, 3, 1);
        CHECK(thermal_softoff_check(&pp, &st2, cool, with_none, 3, 1) == 1,
                "a -273 board does not mask a hot sibling");
    }

    {
        thermal_softoff_state_t st    = { 0 };
        int                     trips = 0;
        for (int i = 0; i < 8; i++)
            trips += thermal_softoff_check(&pp, &st, cool, hot1, 1, 0);
        CHECK(trips == 0, "hot outlet while NOT in work -> no trip");
        CHECK(st.outlet_hot_run >= 5, "...but the run was counted anyway");
        CHECK(thermal_softoff_check(&pp, &st, cool, hot1, 1, 1) == 1,
                "...so entering work over the count trips on the FIRST working sample");
    }
    {
        thermal_softoff_state_t st = { 0 };
        for (int i = 0; i < 8; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 0);
        (void)thermal_softoff_check(&pp, &st, cool, cool1, 1, 0);
        CHECK(thermal_softoff_check(&pp, &st, cool, hot1, 1, 1) == 0,
                "...but a cool sample while idle still resets, so no trip on entering work");
    }
    {
        thermal_softoff_state_t st = { 0 };
        for (int i = 0; i < 1000; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 0);
        CHECK(st.outlet_hot_run == 5, "the idle run clamps at the count (no uint8 wrap)");
        CHECK(thermal_softoff_check(&pp, &st, cool, hot1, 1, 1) == 1,
                "...and still trips on entering work");
    }

    {
        thermal_softoff_state_t st = { 0 };
        for (int i = 0; i < 3; i++)
            (void)thermal_softoff_check(&pp, &st, cool, hot1, 1, 1);
        CHECK(thermal_softoff_check(&pp, &st, 114, hot1, 1, 1) == 1,
                "PVT arm trips regardless of the outlet run");
        CHECK(st.outlet_hot_run == 0, "...and the shared trip block clears the outlet run");
    }

    const uint32_t        grace = 90000u;
    thermal_telem_state_t st    = { 0 };

    CHECK(thermal_telem_check(&st, 1, 1, 1000, grace) == THERMAL_TELEM_OK,
            "asic live + valid temp -> run the PID");

    st = (thermal_telem_state_t){ 0 };
    CHECK(thermal_telem_check(&st, 1, 1, 1000, grace) == THERMAL_TELEM_OK,
            "a good sample arms the grace window");
    CHECK(thermal_telem_check(&st, 0, 0, 2000, grace) == THERMAL_TELEM_HOLD,
            "asic telem stalls -> HOLD, never step the PID");
    CHECK(thermal_telem_check(&st, 0, 0, 1000 + grace, grace) == THERMAL_TELEM_HOLD,
            "...still HOLD exactly at the grace boundary");
    CHECK(thermal_telem_check(&st, 0, 0, 1001 + grace, grace) == THERMAL_TELEM_FAILSAFE,
            "...and past the grace it is FAILSAFE (fan max), not a low duty");

    st = (thermal_telem_state_t){ 0 };
    (void)thermal_telem_check(&st, 1, 1, 1000, grace);
    CHECK(thermal_telem_check(&st, 1, 0, 2000, grace) == THERMAL_TELEM_HOLD,
            "live but every chip invalid -> HOLD, never step the PID");
    CHECK(thermal_telem_check(&st, 1, 0, 1001 + grace, grace) == THERMAL_TELEM_FAILSAFE,
            "...and past the grace it is FAILSAFE (fan max), not the PID's fan-min arm");

    st = (thermal_telem_state_t){ 0 };
    (void)thermal_telem_check(&st, 1, 1, 1000, grace);
    CHECK(thermal_telem_check(&st, 0, 1, 2000, grace) == THERMAL_TELEM_HOLD,
            "asic dead but temp still parses -> unusable (HOLD)");

    st = (thermal_telem_state_t){ 0 };
    (void)thermal_telem_check(&st, 1, 1, 1000, grace);
    CHECK(thermal_telem_check(&st, 1, 1, 1001 + grace, grace) == THERMAL_TELEM_OK,
            "a live heartbeat with a parsing temperature is usable however old mm's "
            "own local poll is -> OK");

    st = (thermal_telem_state_t){ 0 };
    (void)thermal_telem_check(&st, 1, 1, 1000, grace);
    for (uint64_t t = 2000; t < 1000000; t += 4000) {
        (void)thermal_telem_check(&st, 0, 0, t, grace);
        (void)thermal_telem_check(&st, 1, 1, t + 2000, grace);
    }
    CHECK(thermal_telem_check(&st, 1, 1, 1002000, grace) == THERMAL_TELEM_OK,
            "a flapping heartbeat never trips the fail-safe (recovery re-arms)");

    st = (thermal_telem_state_t){ 0 };
    CHECK(thermal_telem_check(&st, 0, 0, 5000, grace) == THERMAL_TELEM_HOLD,
            "no telemetry ever seen -> HOLD, and the grace starts at this tick");
    CHECK(thermal_telem_check(&st, 0, 0, 5000 + grace + 1, grace) == THERMAL_TELEM_FAILSAFE,
            "...FAILSAFE only once that grace has run, not immediately");

    st = (thermal_telem_state_t){ 0 };
    (void)thermal_telem_check(&st, 0, 0, 1000, grace);
    (void)thermal_telem_check(&st, 0, 0, 1001 + grace, grace);
    CHECK(st.failsafe == 1, "the fail-safe latches for a one-shot log");
    CHECK(thermal_telem_check(&st, 1, 1, 1002 + grace, grace) == THERMAL_TELEM_OK &&
                    st.failsafe == 0,
            "recovery clears the latch");

    thermal_coverage_state_t cs    = { 0 };
    const uint32_t           ripen = 30000;
    CHECK(thermal_coverage_check(&cs, 8, 10, 60, 1000, ripen) == THERMAL_COVERAGE_QUIET,
            "coverage: inside the ripen window -> QUIET");
    CHECK(thermal_coverage_check(&cs, 8, 10, 60, 1001 + ripen, ripen) == THERMAL_COVERAGE_SHORT,
            "coverage: 2 chips silent after ripening -> SHORT once");
    CHECK(thermal_coverage_check(&cs, 8, 10, 60, 2001 + ripen, ripen) == THERMAL_COVERAGE_QUIET,
            "coverage: same count -> no new report");
    CHECK(thermal_coverage_check(&cs, 7, 10, 60, 3001 + ripen, ripen) == THERMAL_COVERAGE_QUIET,
            "coverage: missing count rises but stays non-zero -> no new report");
    CHECK(thermal_coverage_check(&cs, 9, 10, 60, 4001 + ripen, ripen) == THERMAL_COVERAGE_QUIET,
            "coverage: missing count falls but stays non-zero -> no new report");
    CHECK(thermal_coverage_check(&cs, 10, 10, 60, 5001 + ripen, ripen) == THERMAL_COVERAGE_FULL,
            "coverage: all chips report again -> FULL once");
    CHECK(thermal_coverage_check(&cs, 10, 10, 60, 6001 + ripen, ripen) == THERMAL_COVERAGE_QUIET,
            "coverage: still full -> no new report");

    if (fails) {
        fprintf(stderr, "test_thermal: %d FAIL\n", fails);
        return 1;
    }
    printf("test_thermal: all pass\n");
    return 0;
}
