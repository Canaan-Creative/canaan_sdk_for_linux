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

#define _DEFAULT_SOURCE
#include <string.h>

#include "ble_wr.h"

#define BLE_WR_TERM 0x24242424u

static uint32_t ble_wr_tail(uint32_t tail, const unsigned char *d, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        tail = (tail << 8) | d[i];
    return tail;
}

int ble_wr_feed(struct ble_wr *w, const void *data, size_t len)
{
    uint32_t tail;
    int      i;

    if (w->skip) {
        w->tail = ble_wr_tail(w->tail, data, len);
        if (BLE_WR_TERM == w->tail)
            ble_wr_reset(w);
        return BLE_WR_MORE;
    }

    if (w->pos + len > sizeof(w->buf) - 1) {
        tail = 0;
        for (i = w->pos > 3 ? w->pos - 3 : 0; i < w->pos; i++)
            tail = (tail << 8) | (unsigned char)w->buf[i];
        tail = ble_wr_tail(tail, data, len);
        ble_wr_reset(w);
        if (BLE_WR_TERM != tail) {
            w->skip = 1;
            w->tail = tail;
        }
        return BLE_WR_OVERFLOW;
    }

    if (data && len) {
        memcpy(w->buf + w->pos, data, len);
        w->pos += len;
    }

    if (w->pos > 4 && memcmp(&w->buf[w->pos - 4], "$$$$", 4) == 0) {
        w->buf[w->pos - 4] = 0;
        return BLE_WR_LINE;
    }
    return BLE_WR_MORE;
}

void ble_wr_reset(struct ble_wr *w)
{
    w->pos  = 0;
    w->skip = 0;
    w->tail = 0;
    explicit_bzero(w->buf, sizeof(w->buf));
}
