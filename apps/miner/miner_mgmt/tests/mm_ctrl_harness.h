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

#ifndef MM_CTRL_HARNESS_H
#define MM_CTRL_HARNESS_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ctrl_cmds.h"
#include "mc_set_result.h"
#include "miner_ipc.h"
#include "osif.h"
#include "mm_ctrl.c"
#include "../../tests/include/tst_check.h"

osif_ipc_t        *g_ipc_core_mm;
static int         tst_ack_n;
static mc_ctrl_ack tst_ack;
static char        tst_oplog[256];

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_CTRL_ACK && data && len == (int)sizeof tst_ack) {
        memcpy(&tst_ack, data, sizeof tst_ack);
        tst_ack_n++;
    }
    return OSIF_OK;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl != OSIF_LOG_OP)
        return;
    va_start(ap, fmt);
    vsnprintf(tst_oplog, sizeof tst_oplog, fmt, ap);
    va_end(ap);
}

uint64_t osif_now_ms(void)
{
    return 0;
}

mc_set_result mm_led_apply(uint16_t op, const char *args)
{
    (void)op;
    (void)args;
    return MC_SET_OK;
}

void filtermesh_clean(void)
{
}

mc_set_result mm_apply_settz(const char *args)
{
    (void)args;
    return MC_SET_OK;
}

int mm_system_reboot(uint8_t reason, uint32_t sub)
{
    (void)reason;
    (void)sub;
    return 0;
}

void mm_cfg_flush(void)
{
}

mc_set_result mm_apply_workmode(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    (void)c;
    return MC_SET_OK;
}

mc_set_result mm_apply_soft(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    (void)c;
    return MC_SET_OK;
}

void mm_periph_push(void)
{
}

#endif /* MM_CTRL_HARNESS_H */
