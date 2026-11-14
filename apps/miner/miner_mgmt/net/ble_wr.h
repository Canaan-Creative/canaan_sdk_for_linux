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

#ifndef BLE_WR_H
#define BLE_WR_H

#include <stddef.h>
#include <stdint.h>

#include "ble_prov.h"

enum {
    BLE_WR_MORE,
    BLE_WR_LINE,
    BLE_WR_OVERFLOW,
};

struct ble_wr {
    char     buf[BLE_DATA_WR_LEN]; /* write characteristic reassembly buffer */
    int      pos;                  /* bytes held in buf */
    int      skip;
    uint32_t tail;
};

int  ble_wr_feed(struct ble_wr *w, const void *data, size_t len);
void ble_wr_reset(struct ble_wr *w);

#endif /* BLE_WR_H */
