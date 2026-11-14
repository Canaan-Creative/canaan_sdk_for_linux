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

#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osif.h"
#include "miner_ipc.h"
#include "board_sensors.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

int main(void)
{
    printf("test_core_x3_smoke: OK (%s)\n", BOARDCONF_SECTION);
    return 0;
}
