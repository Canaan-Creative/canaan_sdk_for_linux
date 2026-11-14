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

#ifndef ASIC_INTERNAL_H
#define ASIC_INTERNAL_H

#include "boardconf.h"
#include "miner_ipc.h"
#include "nonce_ring.h"
#include "osif.h"

#include <signal.h>
#include <stdint.h>

/* board bring-up stage: enumerated, in work mode, mining */
enum { BS_RI = 0, BS_COREREGS, BS_RUN };

struct asic_board {
    uint32_t              local_work; /* works sent since bring-up */
    uint32_t              cur_ver[8]; /* rolled block version per AB core (VERSION0..7) */
    int                   chips_n;    /* chips enumerated, 0 if board off */
    volatile sig_atomic_t pll_pending;
    uint16_t              work_freq;                /* work frequency, MHz */
    uint16_t              pll_pts[ASIC_PLL_POINTS]; /* PLL points to apply, MHz */
    uint16_t              cmd_pts[ASIC_PLL_POINTS]; /* last programmed PLL points, MHz */
    uint8_t               stage;                    /* BS_* */
    uint8_t               chip;                     /* current chip for work and nonce read */
    uint8_t               ri_sta;                   /* board left BS_RI */
    volatile uint8_t      target_pending;
};

extern volatile sig_atomic_t g_run;
extern osif_ipc_t           *g_ipc_asic_core;
extern osif_ipc_t           *g_ipc_asic_mm;
extern nonce_ring_t          g_nring; /* nonces waiting to be sent */
extern volatile sig_atomic_t g_efuse_req;
extern volatile uint8_t      g_bin_grade; /* chip bin grade from efuse */
extern struct asic_board     g_bd[HBOARD_COUNT];

void asic_telem_init(void);
void asic_telem_board_reset(uint8_t ch);
void asic_collect_status(void);
void asic_send_info(uint8_t ch);
int  is_all_running(void);
void efuse_scan_step(void);

#endif /* ASIC_INTERNAL_H */
