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

#include "fault_report.h"
#include "mm_telem.h"
#include "boardconf.h"
#include <stdio.h>

#include "../../tests/include/tst_check.h"

#if defined(BOARD_X3_H)
/* Expected x3 hashboard count and hashboard-MCU bus flag */
#define BOARD_NAME   "x3"
#define WANT_BOARDS  2
#define WANT_MCU_BUS 1
#elif defined(BOARD_NANO3S_H)
/* Expected nano3s hashboard count and hashboard-MCU bus flag */
#define BOARD_NAME   "nano3s"
#define WANT_BOARDS  1
#define WANT_MCU_BUS 0
#else
#error "no hashboard goldens for this board"
#endif

static void test_classify(void)
{
    printf("[classify] hashmcu_read outcome -> fault code\n");
    CHECK(mm_hbmcu_fault_code(0, 1) == MM_HBMCU_OK, "rc 0 is healthy");
    CHECK(mm_hbmcu_fault_code(-1, 1) == MM_HBMCU_INLET, "outlet good + rc -1 = inlet-only");
    CHECK(mm_hbmcu_fault_code(-1, 0) == MM_HBMCU_DEAD, "no outlet + rc -1 = dead MCU");
    CHECK(MM_HBMCU_OK == 0, "healthy is code 0, so the gate stays silent");
    CHECK(MM_HBMCU_DEAD != MM_HBMCU_INLET, "the two failures are distinguishable");
}

static int run(fault_report_t *st, uint32_t code, int n)
{
    int lines = 0;
    for (int i = 0; i < n; i++)
        if (fault_report_step(st, code) != FAULT_REPORT_NONE)
            lines++;
    return lines;
}

static void test_healthy_is_silent(void)
{
    printf("[gate] a caller that never faults never logs\n");
    fault_report_t st = { 0 };
    CHECK(run(&st, MM_HBMCU_OK, 3600) == 0, "an hour of healthy polls emits nothing");
    CHECK(st.code == MM_HBMCU_OK, "and leaves the gate on the healthy code");
}

static void test_first_fault_is_immediate(void)
{
    printf("[gate] the transition is reported at once, the state is not\n");
    fault_report_t       st = { 0 };
    fault_report_verdict v  = fault_report_step(&st, MM_HBMCU_DEAD);
    CHECK(v == FAULT_REPORT_CHANGED, "first failing poll logs immediately");
    CHECK(run(&st, MM_HBMCU_DEAD, 599) == 0, "the next 599 identical polls are silent");
}

static void test_persistent_fault_is_reported_once(void)
{
    printf("[gate] a permanently dead MCU is reported once, never repeated\n");
    fault_report_t st    = { 0 };
    int            lines = run(&st, MM_HBMCU_DEAD, 3600);
    CHECK(lines == 1, "an hour of a dead MCU costs %d line(s), not 3600", lines);
    CHECK(st.code == MM_HBMCU_DEAD, "and the gate holds the dead code");
}

static void test_recovery_is_reported(void)
{
    printf("[gate] recovery is an edge too, reported at once\n");
    fault_report_t st = { 0 };
    (void)fault_report_step(&st, MM_HBMCU_DEAD);
    fault_report_verdict v = fault_report_step(&st, MM_HBMCU_OK);
    CHECK(v == FAULT_REPORT_CHANGED, "a recovery on the next poll is reported");
    CHECK(st.code == MM_HBMCU_OK, "as a recovery: the code the line names is 0");
    CHECK(run(&st, MM_HBMCU_OK, 3600) == 0, "after which a healthy board is silent again");
}

static void test_fault_recover_fault_all_reported(void)
{
    printf("[gate] fault -> recovery -> fault: each edge is its own line\n");
    fault_report_t st = { 0 };
    CHECK(fault_report_step(&st, MM_HBMCU_DEAD) == FAULT_REPORT_CHANGED, "the fault is reported");
    CHECK(fault_report_step(&st, MM_HBMCU_OK) == FAULT_REPORT_CHANGED, "the recovery is reported");
    CHECK(fault_report_step(&st, MM_HBMCU_DEAD) == FAULT_REPORT_CHANGED,
            "the second fault is reported");
}

static void test_a_flapping_mcu_reports_every_edge(void)
{
    printf("[gate] an MCU that flaps at the poll rate reports every transition\n");
    fault_report_t st    = { 0 };
    int            lines = 0;
    for (int i = 0; i < 3600; i++) {
        uint32_t code = (i & 1) ? MM_HBMCU_DEAD : MM_HBMCU_OK;
        if (fault_report_step(&st, code) != FAULT_REPORT_NONE)
            lines++;
    }
    CHECK(lines == 3599, "3599 transitions cost %d lines", lines);
}

static void test_inlet_failure_is_not_hidden(void)
{
    printf("[gate] a second, different fault on the same board is its own edge\n");
    fault_report_t st = { 0 };
    (void)fault_report_step(&st, MM_HBMCU_DEAD);
    fault_report_verdict v = fault_report_step(&st, MM_HBMCU_INLET);
    CHECK(v == FAULT_REPORT_CHANGED, "dead -> inlet-only is CHANGED");
    CHECK(st.code == MM_HBMCU_INLET, "and the gate holds the new code");
}

static void test_per_board_slots(void)
{
    printf("[%s] %d hashboard slot(s), stepped independently\n", BOARD_NAME, HBOARD_COUNT);
    CHECK(HBOARD_COUNT == WANT_BOARDS, "%s has %d hashboard(s)", BOARD_NAME, WANT_BOARDS);
    if (CAP_HB_MCU)
        CHECK(WANT_MCU_BUS == 1, "%s declares a hashboard-MCU bus", BOARD_NAME);
    else
        CHECK(WANT_MCU_BUS == 0, "%s declares no hashboard-MCU bus: no line is reachable",
                BOARD_NAME);

    fault_report_t st[HBOARD_COUNT]    = { { 0 } };
    int            lines[HBOARD_COUNT] = { 0 };
    for (int i = 0; i < 600; i++) {
        for (int b = 0; b < HBOARD_COUNT; b++) {
            uint32_t code = (b == 0) ? MM_HBMCU_DEAD : MM_HBMCU_OK;
            if (fault_report_step(&st[b], code) != FAULT_REPORT_NONE)
                lines[b]++;
        }
    }
    CHECK(lines[0] == 1, "the dead board reports once in 600 polls (%d)", lines[0]);
    for (int b = 1; b < HBOARD_COUNT; b++)
        CHECK(lines[b] == 0, "healthy board %d stays silent", b);
}

int main(void)
{
    printf("== fault_report (%s) ==\n", BOARD_NAME);
    test_classify();
    test_healthy_is_silent();
    test_first_fault_is_immediate();
    test_persistent_fault_is_reported_once();
    test_recovery_is_reported();
    test_fault_recover_fault_all_reported();
    test_a_flapping_mcu_reports_every_edge();
    test_inlet_failure_is_not_hidden();
    test_per_board_slots();
    printf(fails ? "FAILED (%d)\n" : "PASS (%d failures)\n", fails);
    return fails ? 1 : 0;
}
