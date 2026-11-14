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

#ifndef BTN_DECODE_H
#define BTN_DECODE_H

#include <stdint.h>

#define BTN_CODE_TP1   105  /* TP1 key code (KEY_LEFT) */
#define BTN_CODE_TP2   103  /* TP2 key code (KEY_UP) */
#define BTN_CODE_POWER 116  /* power key code (KEY_POWER) */
#define BTN_SHORT_MS   30   /* shortest press that counts, ms */
#define BTN_LONG_MS    2000 /* shortest long press, ms */
#define BTN_LLONG_MS   5000 /* shortest very long press, ms */
#define BTN_DCLICK_MS  200  /* double-click window after a short press, ms */

typedef enum {
    BTN_EV_NONE = 0,
    BTN_EV_TP1_SHORT, /* TP1 press, BTN_SHORT_MS..BTN_LONG_MS */
    BTN_EV_TP1_LONG,  /* TP1 press, BTN_LONG_MS..BTN_LLONG_MS */
    BTN_EV_TP1_LLONG,
    BTN_EV_TP2_SHORT,  /* TP2 press, BTN_SHORT_MS..BTN_LONG_MS */
    BTN_EV_TP2_DCLICK, /* two TP2 short presses within BTN_DCLICK_MS */
    BTN_EV_TP2_LONG,   /* TP2 press, BTN_LONG_MS..BTN_LLONG_MS */
    BTN_EV_POWER,
} btn_event;

/* button decoder state; key slots are 0 TP1, 1 TP2, 2 power */
typedef struct {
    uint32_t down_ms[3];    /* press time per key slot, 0 when released, ms */
    int      pending_slot;  /* slot of a short press awaiting the double-click window, -1 none */
    uint32_t pending_since; /* release time of the pending short press, ms */
    int      dclick_slot;   /* slot pressed again inside the double-click window, -1 none */
    int      chk_code;      /* key code of the last unpaired edge, 0 none */
    int      dbkey_mode;    /* two keys held together: TP1/TP2 events suppressed until all are up */
} btn_decoder;

void      btn_decode_init(btn_decoder *b);
btn_event btn_decode_key(btn_decoder *b, int code, int edge, uint32_t t_ms);
btn_event btn_decode_tick(btn_decoder *b, uint32_t t_ms);

#endif /* BTN_DECODE_H */
