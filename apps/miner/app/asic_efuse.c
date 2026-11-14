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

#include "asic_internal.h"
#include "hal.h"
#include "uart_pro.h"
#include "osif.h"

#include <string.h>

#define EFUSE_SEND_RETRY_MIN_MS 50 /* efuse record send retry interval, ms */

/* efuse sweep state: waiting, sending, done */
enum { EFUSE_SCAN_IDLE, EFUSE_SCAN_RUN, EFUSE_SCAN_DONE };

static int            g_efuse_scan;             /* EFUSE_SCAN_* */
static uint8_t        g_efuse_send_blocked;     /* send retry warning logged */
static uint64_t       g_efuse_retry_not_before; /* next send time, ms */
static asic_efuse_rec g_efuse_rec;

int is_all_running(void)
{
    int present = 0;

    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        if (g_bd[bd].chips_n <= 0)
            continue;
        if (g_bd[bd].stage == BS_RI)
            return 0;
        present++;
    }
    return present > 0;
}

static void efuse_read_chip(uint8_t bd, uint16_t chip, uint32_t *data)
{
    uint16_t saved = uart_get_chip_select(bd);

    uart_chip_select(bd, chip);
    hal_asic()->read_efuse(bd, data);
    uart_chip_select(bd, saved);
}

static void efuse_rec_fill(asic_efuse_rec *r, int bd, int chip)
{
    while (bd < HBOARD_COUNT && chip >= g_bd[bd].chips_n) {
        bd++;
        chip = 0;
    }

    if (bd == HBOARD_COUNT) {
        r->board = 0;
        r->chip  = 0;
        r->last  = 1;
        memset(r->data, 0, sizeof r->data);
        return;
    }

    r->board = (uint16_t)bd;
    r->chip  = (uint16_t)chip;
    r->last  = 0;
    efuse_read_chip((uint8_t)r->board, r->chip, r->data);
    if (r->seq != 0)
        return;
    if (r->data[10] == 0)
        osif_log(OSIF_LOG_WARN, "board %u: chip efuse bin is blank", (unsigned)r->board);
    g_bin_grade = efuse_bin_grade(r->data);
}

void efuse_scan_step(void)
{
    asic_efuse_rec *r = &g_efuse_rec;

    if (g_efuse_scan == EFUSE_SCAN_DONE)
        return;
    if (g_efuse_scan == EFUSE_SCAN_IDLE) {
        if (!g_efuse_req || !is_all_running())
            return;
        g_efuse_scan = EFUSE_SCAN_RUN;
        efuse_rec_fill(r, 0, 0);
        return;
    }

    uint64_t now = osif_now_ms();

    if ((int64_t)(now - g_efuse_retry_not_before) < 0)
        return;
    if (osif_ipc_send(g_ipc_asic_mm, MSG_ASIC_EFUSE, r, (int)sizeof *r) != OSIF_OK) {
        g_efuse_retry_not_before = now + EFUSE_SEND_RETRY_MIN_MS;
        if (!g_efuse_send_blocked) {
            g_efuse_send_blocked = 1;
            osif_log(OSIF_LOG_WARN,
                    "efuse record %u (board %u asic %u%s) not "
                    "sent to mm_miner yet; retrying",
                    r->seq, (unsigned)r->board, (unsigned)r->chip, r->last ? ", end of sweep" : "");
        }
        return;
    }

    g_efuse_send_blocked = 0;
    if (r->last) {
        g_efuse_scan = EFUSE_SCAN_DONE;
        return;
    }
    r->seq++;
    efuse_rec_fill(r, r->board, r->chip + 1);
}
