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

#include "btn_decode.h"
#include <stdio.h>
#include "../../tests/include/tst_check.h"

int main(void)
{
    btn_decoder b;

    btn_decode_init(&b);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 1, 1000) == BTN_EV_NONE, "tp1 down: none");
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 1100) == BTN_EV_NONE,
            "tp1 up(100ms): pending, none yet");
    CHECK(btn_decode_tick(&b, 1400) == BTN_EV_TP1_SHORT, "tp1 flush after 200ms -> SHORT");
    CHECK(btn_decode_tick(&b, 1500) == BTN_EV_NONE, "tp1 no repeat");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 2000);
    btn_decode_key(&b, BTN_CODE_TP2, 0, 2100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 2250);
    btn_event e = btn_decode_key(&b, BTN_CODE_TP2, 0, 2300);
    CHECK(e == BTN_EV_TP2_DCLICK, "tp2 double-click -> DCLICK");
    CHECK(btn_decode_tick(&b, 2600) == BTN_EV_NONE, "tp2 no trailing SHORT after DCLICK");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 3000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 8500) == BTN_EV_TP1_LLONG, "tp1 5.5s -> LLONG");
    CHECK(btn_decode_tick(&b, 8800) == BTN_EV_NONE, "no pending after LLONG");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 3000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 8500) == BTN_EV_NONE, "tp2 5.5s -> none");
    CHECK(btn_decode_tick(&b, 8800) == BTN_EV_NONE, "no pending after TP2 5.5s");

    btn_decode_init(&b);
    CHECK(btn_decode_key(&b, BTN_CODE_POWER, 1, 4000) == BTN_EV_POWER, "power press -> POWER");
    CHECK(btn_decode_key(&b, BTN_CODE_POWER, 0, 9500) == BTN_EV_NONE, "power release -> none");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 5000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 5010) == BTN_EV_NONE, "10ms blip -> none");
    CHECK(btn_decode_tick(&b, 5300) == BTN_EV_NONE, "blip: nothing to flush");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 4000) == BTN_EV_TP1_LONG, "tp1 3s -> LONG");
    CHECK(btn_decode_tick(&b, 4300) == BTN_EV_NONE, "no SHORT after TP1 LONG");
    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 4999) == BTN_EV_TP2_LONG, "tp2 3999ms -> LONG");
    CHECK(btn_decode_tick(&b, 5300) == BTN_EV_NONE, "no SHORT after TP2 LONG");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP2, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1220);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 1320) == BTN_EV_TP2_DCLICK,
            "120ms gap + 100ms second press -> DCLICK");
    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP2, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1300);
    CHECK(btn_decode_tick(&b, 1600) == BTN_EV_NONE, "second hold does not flush a SHORT");
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 2200) == BTN_EV_TP2_DCLICK,
            "900ms second hold still completes the DCLICK");
    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP2, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1400);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 1450) == BTN_EV_NONE, "late second press: no DCLICK");
    CHECK(btn_decode_tick(&b, 1700) == BTN_EV_TP2_SHORT, "late second press -> SHORT");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP1, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1220);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 1320) == BTN_EV_NONE, "tp1 double-click -> nothing");
    CHECK(btn_decode_tick(&b, 1600) == BTN_EV_NONE, "tp1 double-click: nothing flushed");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1100);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 6200) == BTN_EV_NONE, "both held 5s: no TP1 LLONG");
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 6300) == BTN_EV_NONE, "both held 5s: no TP2 event");
    CHECK(btn_decode_tick(&b, 6600) == BTN_EV_NONE, "two-key grab: nothing flushed");
    btn_decode_key(&b, BTN_CODE_TP1, 1, 7000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 12100) == BTN_EV_TP1_LLONG,
            "interlock cleared: TP1 LLONG works again");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP1, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1500);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1600);
    CHECK(btn_decode_tick(&b, 1900) == BTN_EV_TP1_SHORT, "pending SHORT still flushes");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 1010);
    btn_decode_key(&b, BTN_CODE_TP2, 0, 1015);
    btn_decode_key(&b, BTN_CODE_TP1, 0, 1100);
    btn_decode_key(&b, BTN_CODE_TP2, 1, 2000);
    CHECK(btn_decode_key(&b, BTN_CODE_TP2, 0, 5000) == BTN_EV_TP2_LONG,
            "interlock cleared once all keys are up: TP2 LONG works after a blip");

    btn_decode_init(&b);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1000);
    CHECK(btn_decode_key(&b, BTN_CODE_POWER, 1, 1100) == BTN_EV_POWER,
            "power press fires while latched");
    CHECK(btn_decode_key(&b, BTN_CODE_POWER, 0, 6200) == BTN_EV_NONE, "power release -> none");
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 6300) == BTN_EV_NONE, "TP1 LLONG suppressed");
    btn_decode_key(&b, BTN_CODE_TP1, 1, 7000);
    btn_decode_key(&b, BTN_CODE_TP1, 0, 7100);
    CHECK(btn_decode_tick(&b, 7400) == BTN_EV_TP1_SHORT, "TP1 SHORT after a power combo");

    btn_decode_init(&b);
    CHECK(btn_decode_key(&b, BTN_CODE_POWER, 1, 1000) == BTN_EV_POWER, "power press -> POWER");
    btn_decode_key(&b, BTN_CODE_TP1, 1, 1100);
    CHECK(btn_decode_key(&b, BTN_CODE_TP1, 0, 1200) == BTN_EV_NONE, "TP1 under power: suppressed");
    btn_decode_key(&b, BTN_CODE_POWER, 0, 1300);
    btn_decode_key(&b, BTN_CODE_TP1, 1, 2000);
    btn_decode_key(&b, BTN_CODE_TP1, 0, 2100);
    CHECK(btn_decode_tick(&b, 2400) == BTN_EV_TP1_SHORT, "TP1 SHORT after power released last");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
