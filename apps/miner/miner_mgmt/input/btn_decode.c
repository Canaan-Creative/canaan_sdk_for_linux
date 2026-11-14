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

static int key_slot(int code)
{
    switch (code) {
    case BTN_CODE_TP1:
        return 0;
    case BTN_CODE_TP2:
        return 1;
    case BTN_CODE_POWER:
        return 2;
    default:
        return -1;
    }
}

void btn_decode_init(btn_decoder *b)
{
    *b = (btn_decoder){ .pending_slot = -1, .dclick_slot = -1 };
}

static btn_event short_ev(int slot)
{
    return slot == 0 ? BTN_EV_TP1_SHORT : slot == 1 ? BTN_EV_TP2_SHORT : BTN_EV_NONE;
}
static btn_event long_ev(int slot)
{
    return slot == 0 ? BTN_EV_TP1_LONG : slot == 1 ? BTN_EV_TP2_LONG : BTN_EV_NONE;
}
static btn_event llong_ev(int slot)
{
    return slot == 0 ? BTN_EV_TP1_LLONG : BTN_EV_NONE;
}

btn_event btn_decode_key(btn_decoder *b, int code, int edge, uint32_t t_ms)
{
    int slot = key_slot(code);
    if (slot < 0)
        return BTN_EV_NONE;

    if (!b->chk_code)
        b->chk_code = code;
    else if (b->chk_code == code)
        b->chk_code = 0;
    else {
        b->dbkey_mode = 1;
        b->chk_code   = 0;
    }

    int tier = 0, dclick = 0;
    if (edge) {
        if (b->pending_slot == slot && (t_ms - b->pending_since) <= BTN_DCLICK_MS) {
            b->pending_slot = -1;
            b->dclick_slot  = slot;
        }
        b->down_ms[slot] = t_ms;
    } else if (b->down_ms[slot]) {
        uint32_t dur     = t_ms - b->down_ms[slot];
        b->down_ms[slot] = 0;
        dclick           = (b->dclick_slot == slot);
        b->dclick_slot   = -1;
        tier = dur >= BTN_LLONG_MS ? 3 : dur >= BTN_LONG_MS ? 2 : dur >= BTN_SHORT_MS ? 1 : 0;
    }

    btn_event ev = BTN_EV_NONE;
    if (slot == 2) {
        ev = edge ? BTN_EV_POWER : BTN_EV_NONE;
    } else if (tier && !b->dbkey_mode) {
        if (tier == 3) {
            b->pending_slot = -1;
            ev              = llong_ev(slot);
        } else if (tier == 2) {
            b->pending_slot = -1;
            ev              = long_ev(slot);
        } else if (dclick) {
            ev = slot == 1 ? BTN_EV_TP2_DCLICK : BTN_EV_NONE;
        } else {
            b->pending_slot  = slot;
            b->pending_since = t_ms;
        }
    }

    if (!b->down_ms[0] && !b->down_ms[1] && !b->down_ms[2])
        b->dbkey_mode = 0;

    return ev;
}

btn_event btn_decode_tick(btn_decoder *b, uint32_t t_ms)
{
    if (b->pending_slot >= 0 && (t_ms - b->pending_since) > BTN_DCLICK_MS) {
        int slot        = b->pending_slot;
        b->pending_slot = -1;
        return short_ev(slot);
    }
    return BTN_EV_NONE;
}
