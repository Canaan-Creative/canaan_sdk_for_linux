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

#ifndef MC_API_TYPES_H
#define MC_API_TYPES_H

#include <stdint.h>

#include "miner_ipc.h"
#include "process_version.h"
#include "core_board.h"

#define MC_API_FAN_FIELD_CAP 14u /* one Fan field width, bytes */
#define MC_API_MH_CAP        11u /* MH field width per board, bytes */
#define MC_API_ECHU_CAP      11u /* ECHU field width per board, bytes */
#define MC_API_MGHS_CAP      24u /* MGHS field width per board, bytes */
#define MC_API_MTMAX_CAP     7u  /* MTmax field width per board, bytes */

/* MM ID0 string buffer, bytes */
#define MC_API_MM_ID0_CAP \
    (4096u + (unsigned)HBOARD_COUNT * (unsigned)MINER_MAX_CHIPS * 112u + \
            (unsigned)FAN_COUNT * MC_API_FAN_FIELD_CAP + \
            (unsigned)HBOARD_COUNT * \
                    (MC_API_MH_CAP + MC_API_ECHU_CAP + MC_API_MGHS_CAP + MC_API_MTMAX_CAP))
#define MC_API_REPLY_CAP    (MC_API_MM_ID0_CAP + 4096u) /* API reply buffer, bytes */
#define MC_API_POOL_URL_CAP 257u                        /* pool URL buffer, bytes */

typedef struct {
    int         id;
    int         prio; /* pool priority, 0 = highest */
    const char *url;
    const char *status;
    const char *user;
    uint32_t    accepted, rejected;
    double      diff_accepted, diff_rejected; /* accepted and rejected difficulty sum */
    uint32_t    stale;
    double      diff_stale; /* stale difficulty sum */
    double      work_diff;
    double      last_share_diff;
    uint32_t    last_share_time; /* last share time, s */
    double      best_share;
    double      diff1_shares;
    uint32_t    getworks; /* jobs received, count */
    uint32_t    bad_work;
    uint32_t    block_version;
    uint32_t    works;        /* works generated, count */
    uint32_t    get_failures; /* job fetch failures, count */
    uint32_t    remote_failures;
    uint32_t    block_height;
    int         stratum_active;
    uint32_t    tx_cnt, tx_bytes, rx_cnt, rx_bytes;
} mc_api_pool;

typedef struct {
    int      valid;
    int      in_work;            /* any board mining */
    int      freq_mhz;           /* average frequency, MHz */
    double   ghs_mm;             /* nominal hashrate, GH/s */
    double   ghs_spd_chip;       /* speed-log hashrate, GH/s */
    int      temp_max, temp_avg; /* max/avg chip temp, degrees C */
    int      board_temp;         /* control board temperature, degrees C */
    int      ps[7];              /* PS[] fields of the stats reply */
    int      fan_rpm[FAN_COUNT];
    int      fan_pct; /* fan duty, % */
    int      bin_grade;
    int      hb_count;
    int      hb_outlet[HBOARD_COUNT]; /* hash board outlet temperature, degrees C */
    int      hb_inlet[HBOARD_COUNT];  /* hash board inlet temperature, degrees C */
    int      board_n;                 /* boards in the chip arrays, count */
    int      chip_n[HBOARD_COUNT];
    int16_t  chip_pvt_t[HBOARD_COUNT][MINER_MAX_CHIPS]; /* chip temperature, degrees C */
    uint16_t chip_pvt_v[HBOARD_COUNT][MINER_MAX_CHIPS]; /* chip voltage, mV */
    uint8_t  chip_pllcnt[HBOARD_COUNT][MINER_MAX_CHIPS]
                       [ASIC_PLL_POINTS]; /* cores per PLL point, count */
    uint16_t chip_cf[HBOARD_COUNT][MINER_MAX_CHIPS][ASIC_PLL_POINTS]; /* PLL point frequency, MHz */
    uint32_t chip_pass[HBOARD_COUNT][MINER_MAX_CHIPS];                /* speed-log pass counts */
    uint32_t chip_fail[HBOARD_COUNT][MINER_MAX_CHIPS];                /* speed-log fail counts */
    uint32_t board_err_crc[HBOARD_COUNT];            /* chip errors plus UART failures, count */
    uint32_t board_com_crc[HBOARD_COUNT];            /* UART failures, count */
    uint64_t chip_mw[HBOARD_COUNT][MINER_MAX_CHIPS]; /* valid nonces per chip, count */
    uint8_t  softoff_rsn;                            /* enum mc_softoff_rsn */
    uint8_t  work_state;                             /* enum mc_work_state */
} mc_api_telem;

typedef struct {
    const char *prod, *model, *hwtype, *swtype, *version, *dna, *mac;
    char        mm_version[MINER_VERSION_CAP];
    uint32_t    when;    /* snapshot time, s */
    uint32_t    elapsed; /* uptime, s */
    uint32_t    accepted, rejected, hw_errors, discarded;
    uint32_t    stale;
    uint32_t    hw_board[HBOARD_COUNT];                  /* hardware errors per board, count */
    uint32_t    hu_errcode[HBOARD_COUNT];                /* hash board error bits */
    uint32_t    mm_errcode;                              /* controller error bits */
    uint32_t    dhw;                                     /* duplicate nonces, count */
    double      mhs_av, mhs_5s, mhs_1m, mhs_5m, mhs_15m; /* hashrate averages, MH/s */
    double      diff_accepted;                           /* accepted difficulty sum */
    double      diff_rejected;                           /* rejected difficulty sum */
    double      diff_stale;                              /* stale difficulty sum */
    double      best_share;
    uint32_t    last_valid_work; /* last valid work time, s */
    double      dev_elapsed;     /* device elapsed time, s */
    double      diff1;
    double      diff1_board[HBOARD_COUNT];
    uint32_t    local_work;
    uint32_t    mm_local_work; /* works sent to chips, count */
    uint32_t    get_failures;  /* job fetch failures, count */
    uint32_t    remote_failures;
    uint32_t    ping_ms;    /* pool ping, ms */
    uint32_t    memfree_kb; /* free memory, KiB */
    uint32_t    pool_failcnt;
    int         pool_idle;
    uint32_t    netfail_time[8]; /* network failure times, s */
    int         workmode;
    int         maxmode;
    int         max_pout, target_temp; /* max power W, target temp C */
    int         adj;                   /* aging state */
    int         caliall;               /* calibrate all modes */
    int         bar;                   /* aging progress, % */
    int         activation;
    uint8_t     bootby_reason; /* enum mc_bootby */
    uint32_t    bootby_sub;
    int         cali_volt, cali_pll_start,
            cali_pll_interval; /* cali volt rail counts, start/step freq MHz */
    int ledmode;
    CORE_MEM_LED
    char ssid[128];
    int  rssi; /* WiFi signal strength, dBm */
    int  net_devtype;
    CORE_MEM_LCD_BRIGHT
    CORE_MEM_LCD_ONOFF
    CORE_MEM_VOLUME
    int          filtermesh; /* filter mesh usage time, minutes */
    char         hash_sn[HBOARD_COUNT][HASH_SN_LEN + 1];
    int          asc_count, pool_count;
    int          npools; /* pools in use, count */
    mc_api_pool  pool[8];
    const char  *block_hash;
    double       net_difficulty;
    double       block_time; /* block time, s */
    uint32_t     net_blocks;
    mc_api_telem telem;
} mc_api_stats;

_Static_assert(sizeof(((mc_api_stats *)0)->ssid) == sizeof(((mm_periph *)0)->ssid),
        "litestats SSID[..] is copied straight out of mm_periph; a narrower "
        "copy here truncates the wpa_cli escaped form mid-\\xNN escape");

#endif /* MC_API_TYPES_H */
