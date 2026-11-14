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

#include "softonoff_sched.h"
#include <stdio.h>

#include "../../tests/include/tst_check.h"

#define T 1755000000LL /* Reference schedule time, unix seconds */

int main(void)
{
    soft_sched_t st;

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 1, 1, T);
    CHECK(st.on_valid == 1 && st.on_ts == T, "a first softon is stored");
    soft_sched_arm(&st, 0, 1, T + 3600);
    CHECK(st.off_valid == 1 && st.off_ts == T + 3600, "a softoff an hour later is stored");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    soft_sched_arm(&st, 1, 1, T + 2);
    CHECK(st.on_valid == 1 && st.on_ts == T + 2 && st.off_valid == 1 && st.off_ts == T,
            "softon 2 s after an armed softoff is stored beside it");
    soft_sched_arm(&st, 1, 1, T - 2);
    CHECK(st.on_valid == 1 && st.on_ts == T - 2, "...and 2 s before it too");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, 1);
    CHECK(st.off_valid == 1 && st.off_ts == 1, "softoff at unix 1 with no softon is stored");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    soft_sched_arm(&st, 0, 0, 0);
    CHECK(st.off_valid == 0 && st.off_ts == 0, "a cancel (0,0) with no softon is recorded");
    CHECK(soft_sched_due(&st, T, 1) == SOFT_SCHED_NONE, "...and the cancelled softoff never fires");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    soft_sched_arm(&st, 0, 0, T);
    CHECK(st.off_valid == 0, "valid=0 disarms");
    CHECK(soft_sched_due(&st, T, 1) == SOFT_SCHED_NONE, "...and a disarmed softoff never fires");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T - 3, 1) == SOFT_SCHED_NONE, "3 s early: not due");
    CHECK(soft_sched_due(&st, T - 2, 1) == SOFT_SCHED_OFF, "2 s early: due");
    CHECK(st.off_valid == 0, "a fired schedule is consumed");
    CHECK(soft_sched_due(&st, T, 1) == SOFT_SCHED_NONE, "...and does not fire twice");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T + 2, 1) == SOFT_SCHED_OFF, "2 s late: still due");
    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T + 3, 1) == SOFT_SCHED_NONE, "3 s late: MISSED");
    CHECK(st.off_valid == 1, "...and a missed schedule stays armed forever");
    CHECK(soft_sched_due(&st, T + 86400, 1) == SOFT_SCHED_NONE,
            "...still not firing a day later (the window is symmetric, not a deadline)");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T - 10, 1) == SOFT_SCHED_NONE, "checked 10 s early: not due");
    CHECK(soft_sched_due(&st, T + 30, 1) == SOFT_SCHED_OFF,
            "the next check comes 30 s late: the window was crossed since the last "
            "check, so it fires late");
    CHECK(soft_sched_due(&st, T + 31, 1) == SOFT_SCHED_NONE, "...and only once");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 1, 1, T);
    CHECK(soft_sched_due(&st, T - 1000, 0) == SOFT_SCHED_NONE, "clock behind: not due");
    CHECK(soft_sched_due(&st, T + 7200, 0) == SOFT_SCHED_ON,
            "a forward clock jump across the scheduled time fires it once");
    CHECK(soft_sched_due(&st, T - 1000, 0) == SOFT_SCHED_NONE &&
                    soft_sched_due(&st, T, 0) == SOFT_SCHED_NONE,
            "...and a jump back before it does not fire it again");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T + 7200, 1) == SOFT_SCHED_NONE,
            "clock ahead of a softoff already past: not due");
    CHECK(soft_sched_due(&st, T - 600, 1) == SOFT_SCHED_NONE,
            "a backward clock jump to before it: not due yet");
    CHECK(soft_sched_due(&st, T - 1, 1) == SOFT_SCHED_OFF,
            "...and it fires when the corrected clock reaches it");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    CHECK(soft_sched_due(&st, T, 0) == SOFT_SCHED_NONE, "softoff while already stopped: no-op");
    CHECK(st.off_valid == 1, "...and NOT consumed, so it fires when mining resumes");
    CHECK(soft_sched_due(&st, T + 1, 1) == SOFT_SCHED_OFF, "...which it then does");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 1, 1, T);
    CHECK(soft_sched_due(&st, T, 1) == SOFT_SCHED_NONE, "softon while mining: no-op");
    CHECK(soft_sched_due(&st, T, 0) == SOFT_SCHED_ON, "softon while stopped: fires");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    soft_sched_arm(&st, 1, 1, T + 1);
    CHECK(soft_sched_due(&st, T + 1, 1) == SOFT_SCHED_OFF, "on and off both due while mining: OFF");
    CHECK(st.off_valid == 0 && st.on_valid == 1, "...the OFF is consumed, the ON stays armed");
    CHECK(soft_sched_due(&st, T + 2, 0) == SOFT_SCHED_ON,
            "...and on the next tick, now stopped, the ON fires");
    CHECK(st.on_valid == 0, "...and is consumed");
    CHECK(soft_sched_due(&st, T + 3, 1) == SOFT_SCHED_NONE &&
                    soft_sched_due(&st, T + 3, 0) == SOFT_SCHED_NONE,
            "...and nothing fires twice");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T);
    soft_sched_arm(&st, 1, 1, T);
    CHECK(soft_sched_due(&st, T, 0) == SOFT_SCHED_ON, "on and off both due while stopped: ON");
    CHECK(st.on_valid == 0 && st.off_valid == 1, "...the ON is consumed, the OFF stays armed");
    CHECK(soft_sched_due(&st, T + 1, 1) == SOFT_SCHED_OFF,
            "...and on the next tick, now mining and still in the window, the OFF fires");
    CHECK(st.off_valid == 0, "...and is consumed");
    CHECK(soft_sched_due(&st, T + 2, 0) == SOFT_SCHED_NONE &&
                    soft_sched_due(&st, T + 2, 1) == SOFT_SCHED_NONE,
            "...and nothing fires twice");

    st = (soft_sched_t){ 0 };
    soft_sched_arm(&st, 0, 1, T - 2);
    soft_sched_arm(&st, 1, 1, T);
    CHECK(soft_sched_due(&st, T, 0) == SOFT_SCHED_ON,
            "stopped, ON due, OFF at the window edge: ON");
    CHECK(soft_sched_due(&st, T + 1, 1) == SOFT_SCHED_NONE && st.off_valid == 1,
            "...next tick the OFF is past its window: not fired, left armed");

    st = (soft_sched_t){ 0 };
    CHECK(soft_sched_due(&st, 0, 1) == SOFT_SCHED_NONE, "nothing armed at unix 0: no-op");
    CHECK(soft_sched_due(&st, T, 0) == SOFT_SCHED_NONE, "nothing armed, stopped: no-op");

    soft_sched_arm(NULL, 1, 1, T);
    CHECK(soft_sched_due(NULL, T, 1) == SOFT_SCHED_NONE, "due(NULL) is a no-op");

    if (fails) {
        fprintf(stderr, "test_softonoff_sched: %d FAIL\n", fails);
        return 1;
    }
    printf("test_softonoff_sched: all pass\n");
    return 0;
}
