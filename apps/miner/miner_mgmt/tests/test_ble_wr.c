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

#include <stdio.h>
#include <string.h>

#include "ble_wr.h"
#include "../../tests/include/tst_check.h"

static int feed(struct ble_wr *w, const char *s)
{
    return ble_wr_feed(w, s, strlen(s));
}

static int all_zero(const struct ble_wr *w)
{
    for (size_t i = 0; i < sizeof w->buf; i++)
        if (w->buf[i])
            return 0;
    return 1;
}

int main(void)
{
    struct ble_wr w;
    char          big[BLE_DATA_WR_LEN];
    int           rc;

    printf("ble_wr: write-characteristic reassembly\n");

    ble_wr_reset(&w);
    CHECK(w.pos == 0 && all_zero(&w), "1.1 a reset buffer is empty");

    rc = feed(&w, "setssid,41");
    CHECK(rc == BLE_WR_MORE && w.pos == 10, "1.2 a fragment without the terminator is held");
    rc = feed(&w, "42$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "setssid,4142"),
            "1.3 the terminator completes the line without it (got \"%s\")", w.buf);

    ble_wr_reset(&w);
    CHECK(feed(&w, "$$$$") == BLE_WR_MORE, "1.4 a bare terminator is not a command");

    ble_wr_reset(&w);
    feed(&w, "setssid,41");
    ble_wr_reset(&w);
    rc = feed(&w, "scan$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "scan"),
            "1.5 a half line dropped by a reset, as on disconnect, does not prefix the "
            "next session's first command (got \"%s\")",
            w.buf);

    ble_wr_reset(&w);
    memset(big, 'a', sizeof big);
    feed(&w, "x");
    rc = ble_wr_feed(&w, big, sizeof big - 1);
    CHECK(rc == BLE_WR_OVERFLOW && w.pos == 0 && all_zero(&w),
            "1.6 a line longer than %d bytes is refused and the buffer cleared",
            BLE_DATA_WR_LEN - 1);

    ble_wr_reset(&w);
    rc = ble_wr_feed(&w, big, sizeof big - 5);
    rc = rc == BLE_WR_MORE ? feed(&w, "$$$$") : rc;
    CHECK(rc == BLE_WR_LINE && strlen(w.buf) == sizeof big - 5,
            "1.7 a line that fills the buffer exactly is accepted");

    ble_wr_reset(&w);
    rc = ble_wr_feed(&w, big, sizeof big);
    CHECK(rc == BLE_WR_OVERFLOW, "1.8 an overflowing chunk is refused");
    rc = feed(&w, "4142,31,7$$");
    CHECK(rc == BLE_WR_MORE && w.pos == 0,
            "1.9 the rest of the overflowed line is dropped, not buffered (pos %d)", w.pos);
    rc = feed(&w, "$$");
    CHECK(rc == BLE_WR_MORE && w.pos == 0,
            "1.10 a terminator split across chunks ends the dropped line");
    rc = feed(&w, "scan$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "scan"),
            "1.11 the next line is read whole (got \"%s\")", w.buf);

    ble_wr_reset(&w);
    memset(big, 'a', sizeof big);
    memcpy(big + sizeof big - 4, "$$$$", 4);
    rc = ble_wr_feed(&w, big, sizeof big);
    CHECK(rc == BLE_WR_OVERFLOW, "1.12 an overflowing chunk that ends the line is refused");
    rc = feed(&w, "scan$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "scan"),
            "1.13 ...and drops nothing after it (got \"%s\")", w.buf);

    ble_wr_reset(&w);
    memset(big, 'a', sizeof big);
    memcpy(big + sizeof big - 4, "$$", 2);
    ble_wr_feed(&w, big, sizeof big - 2);
    rc = feed(&w, "$$");
    CHECK(rc == BLE_WR_OVERFLOW, "1.14 a terminator split by the overflow is refused");
    rc = feed(&w, "scan$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "scan"),
            "1.15 ...and still ends the line (got \"%s\")", w.buf);

    ble_wr_reset(&w);
    ble_wr_feed(&w, big, sizeof big);
    ble_wr_reset(&w);
    rc = feed(&w, "scan$$$$");
    CHECK(rc == BLE_WR_LINE && !strcmp(w.buf, "scan"),
            "1.16 a reset, as on disconnect, ends a dropped line");

    return tst_done();
}
