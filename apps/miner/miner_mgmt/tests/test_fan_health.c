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

#include "fan_health.h"
#include <stdio.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)
#define DUTY 60 /* commanded fan duty, percent */

static int fails;

static fan_health_verdict run(const fan_health_params_t *pp, fan_health_state_t *st, const int *rpm,
        int n, int iters)
{
    fan_health_verdict v = FAN_HEALTH_OK;
    for (int i = 0; i < iters; i++)
        v = fan_health_check(pp, st, rpm, n, DUTY, 1);
    return v;
}

int main(void)
{
    const fan_health_params_t x3  = { .err_count = 5 };
    const fan_health_params_t off = { .err_count = 0 };

    {
        fan_health_state_t st          = { 0 };
        int                spinning[3] = { 4000, 4200, 4100 };
        int                stopped[3]  = { 4000, 4200, 0 };
        CHECK(run(&x3, &st, spinning, 3, 3) == FAN_HEALTH_OK, "all three fans turning -> OK");
        CHECK(st.bad_run == 0, "...and no run is counted");

        for (int i = 1; i <= 4; i++) {
            fan_health_verdict v = fan_health_check(&x3, &st, stopped, 3, DUTY, 1);
            if (v != FAN_HEALTH_BAD) {
                fprintf(stderr, "  FAIL: sample %d of the run should be BAD\n", i);
                fails++;
            }
        }
        CHECK(st.bad_run == 4, "four bad samples is a run of 4, still under the trip");
        CHECK(fan_health_check(&x3, &st, stopped, 3, DUTY, 1) == FAN_HEALTH_TRIP,
                "the FIFTH consecutive bad sample trips: the run reaches err_count");
        CHECK(fan_health_check(&x3, &st, stopped, 3, DUTY, 1) == FAN_HEALTH_TRIP,
                "...and every bad sample after it keeps tripping, not a fresh count-up");
        CHECK(fan_health_check(&x3, &st, spinning, 3, DUTY, 1) == FAN_HEALTH_OK,
                "one good sample clears the run");
        CHECK(st.bad_run == 0, "...and the counter with it");
    }

    {
        fan_health_state_t st        = { 0 };
        int                absent[3] = { 4000, FAN_RPM_NONE, 4100 };
        CHECK(run(&x3, &st, absent, 3, 200) == FAN_HEALTH_OK,
                "a tach node that never answers (FAN_RPM_NONE) never trips, however long");
        CHECK(st.bad_run == 0, "...and never advances the run");
        CHECK(fan_health_stopped_mask(absent, 3) == 0, "...and is never named as stopped");
    }
    {
        fan_health_state_t st                = { 0 };
        int                zero_from_boot[3] = { 4000, 0, 4100 };
        CHECK(fan_health_check(&x3, &st, zero_from_boot, 3, DUTY, 1) == FAN_HEALTH_BAD,
                "a driven fan reading 0 from the first sample is a bad sample");
        CHECK(run(&x3, &st, zero_from_boot, 3, 4) == FAN_HEALTH_TRIP,
                "...and the fifth such sample trips, with no prior good reading needed");
        CHECK(fan_health_stopped_mask(zero_from_boot, 3) == 0x2u,
                "...and the log mask names that fan");
    }
    {
        fan_health_state_t st       = { 0 };
        int                slow[1]  = { 150 };
        int                edge[1]  = { FAN_STALL_RPM - 1 };
        int                at[1]    = { FAN_STALL_RPM };
        int                above[1] = { 250 };
        CHECK(fan_health_check(&x3, &st, slow, 1, DUTY, 1) == FAN_HEALTH_BAD,
                "150 RPM is below FAN_STALL_RPM -> bad sample");
        CHECK(fan_health_check(&x3, &st, edge, 1, DUTY, 1) == FAN_HEALTH_BAD,
                "FAN_STALL_RPM - 1 is still a bad sample");
        CHECK(st.bad_run == 2, "...and both count toward the run");
        CHECK(fan_health_check(&x3, &st, at, 1, DUTY, 1) == FAN_HEALTH_OK,
                "exactly FAN_STALL_RPM is a good sample");
        CHECK(st.bad_run == 0, "...and clears the run");
        CHECK(run(&x3, &st, above, 1, 50) == FAN_HEALTH_OK,
                "250 RPM is a good sample, however long");
        CHECK(run(&x3, &st, slow, 1, 5) == FAN_HEALTH_TRIP,
                "five consecutive samples at 150 RPM trip");
    }
    {
        const fan_health_params_t one        = { .err_count = 1 };
        fan_health_state_t        st         = { 0 };
        int                       stopped[1] = { 0 };
        CHECK(fan_health_check(&one, &st, stopped, 1, DUTY, 1) == FAN_HEALTH_TRIP,
                "err_count 1: the first bad sample trips");
        CHECK(st.bad_run == 1, "...and the run is clamped at 1");
    }
    {
        fan_health_state_t st            = { 0 };
        int                all_absent[3] = { FAN_RPM_NONE, FAN_RPM_NONE, FAN_RPM_NONE };
        CHECK(run(&x3, &st, all_absent, 3, 50) == FAN_HEALTH_OK,
                "no fan readable -> nothing is judged, never a trip");
        CHECK(st.bad_run == 0, "...and the run stays cleared");
    }
    {
        fan_health_state_t st         = { 0 };
        int                stopped[1] = { 0 };
        int                lost[1]    = { FAN_RPM_NONE };
        (void)run(&x3, &st, stopped, 1, 3);
        CHECK(st.bad_run == 3, "three bad samples build a run of 3");
        CHECK(run(&x3, &st, lost, 1, 50) == FAN_HEALTH_OK,
                "a fan whose tach stops answering is not judged: a dead counter is not "
                "a dead fan");
        CHECK(st.bad_run == 0,
                "...and an unreadable sample clears the run rather than freezing it");
    }

    {
        fan_health_state_t st            = { 0 };
        int                mixed_stop[3] = { 0, FAN_RPM_NONE, 4100 };
        CHECK(run(&x3, &st, mixed_stop, 3, 5) == FAN_HEALTH_TRIP,
                "fan0 stopped trips even though fan1 has never read");
        CHECK(fan_health_stopped_mask(mixed_stop, 3) == 0x1u, "the log mask names fan0 only");
    }

    {
        fan_health_state_t st         = { 0 };
        int                stopped[1] = { 0 };
        for (int i = 0; i < 20; i++)
            CHECK(fan_health_check(&x3, &st, stopped, 1, 0, 1) == FAN_HEALTH_OK,
                    "commanded duty 0: the fans are not asked to spin, so 0 RPM is not "
                    "a fault");
        CHECK(st.bad_run == 0, "...and the run stays cleared while the commanded duty is 0");
        CHECK(fan_health_check(&x3, &st, stopped, 1, -1, 1) == FAN_HEALTH_OK,
                "no duty commanded yet (-1) closes the gate too — fail safe, never judge");
    }

    {
        fan_health_state_t st         = { 0 };
        int                stopped[1] = { 0 };
        for (int i = 0; i < 20; i++)
            (void)fan_health_check(&x3, &st, stopped, 1, DUTY, 0);
        CHECK(st.bad_run == 5, "not mining: the run still advances, clamped at the trip count");
        CHECK(fan_health_check(&x3, &st, stopped, 1, DUTY, 0) == FAN_HEALTH_BAD,
                "...but never trips while not mining");
        CHECK(fan_health_check(&x3, &st, stopped, 1, DUTY, 1) == FAN_HEALTH_TRIP,
                "...and the FIRST mining sample trips on the standing count (the "
                "state gate has no else)");
    }
    {
        fan_health_state_t st         = { 0 };
        int                stopped[1] = { 0 };
        st.bad_run                    = 1 << 30;
        (void)fan_health_check(&x3, &st, stopped, 1, DUTY, 1);
        CHECK(st.bad_run == 1 << 30,
                "the run is clamped, so it can never overflow into a wrap like a "
                "uint8_t would");
    }

    {
        fan_health_state_t st          = { 0 };
        int                stopped[1]  = { 0 };
        int                spinning[1] = { 4000 };
        (void)fan_health_check(&off, &st, spinning, 1, DUTY, 1);
        CHECK(run(&off, &st, stopped, 1, 500) == FAN_HEALTH_OK,
                "err_count 0 -> the arm is off, a dead fan is never a soft-off");
        CHECK(st.bad_run == 0, "...and the disarmed rule keeps no state at all");
    }

    if (fails) {
        fprintf(stderr, "test_fan_health: %d FAIL\n", fails);
        return 1;
    }
    printf("test_fan_health: all pass\n");
    return 0;
}
