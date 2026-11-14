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

#ifndef MINER_IPC_H
#define MINER_IPC_H

#include <stdint.h>

#include "mc_sizes.h"
#include "process_version.h"
#include "boardconf.h"

#define MINER_IPC_ASIC_CORE_PATH "/run/avalon_miner_ipcmsg.sock"
#define MINER_IPC_CORE_MM_PATH   "/run/avalon_miner_ctrl.sock"
#define MINER_IPC_ASIC_MM_PATH   "/run/avalon_miner_asicctrl.sock"
#define MM_TZ_NAME_CAP           48 /* timezone name buffer, bytes, including NUL */
#define MINER_MAX_CHIPS          (MAX_ASIC_ON_SINGLE_HASH) /* max chips per hash board, count */

_Static_assert(MINER_MAX_CHIPS >= 1 && MINER_MAX_CHIPS <= 255,
        "MAX_ASIC_ON_SINGLE_HASH must be 1..255: the dispatch loop "
        "counters are uint8_t");

/* Message ids. 5, 8, 9 and 13 are reserved: never reuse them. */
#define MSG_ASIC_JOB     1u  /* core -> asic */
#define MSG_ASIC_NONCE   2u  /* asic -> core */
#define MSG_ASIC_TELEM   3u  /* asic -> core, asic -> mm */
#define MSG_ASIC_VF      4u  /* mm   -> asic */
#define MSG_CTRL_ACK     6u  /* mm   -> core */
#define MSG_MM_PERIPH    7u  /* mm   -> core */
#define MSG_CORE_STATS   10u /* core -> mm */
#define MSG_MM_STATUS    11u /* mm   -> core */
#define MSG_CTRL_CMD     12u /* core -> mm */
#define MSG_ASIC_VERSION 14u /* asic -> core */
#define MSG_ASIC_EFUSE   15u /* asic -> mm */
#define MSG_MM_EFUSE     16u /* mm   -> asic */
#define MSG_MM_BOOTBY    17u /* mm   -> core */
#define MSG_MM_VERSION   18u /* mm   -> core */

#define MC_COINBASE_MAX   (6 * 1024 + 64)           /* max coinbase size, bytes */
#define MC_MERKLES_MAX    30                        /* max merkle branches, count */
#define MC_COINB_HEX_LEN  (2 * MC_COINBASE_MAX + 1) /* coinbase hex string buffer, bytes */
#define MC_MERKLE_HEX_LEN (MC_MERKLES_MAX * 65)     /* merkle branch hex buffer, bytes */

/* MSG_ASIC_JOB payload: one stratum job */
typedef struct {
    uint32_t seq;
    uint32_t n2_base;                                         /* extranonce2 start value */
    uint32_t target_h, target_l;                              /* share target, high and low words */
    int32_t  xnonce2_size;                                    /* extranonce2 size, bytes */
    char     version[16], prevhash[72], ntime[16], nbits[16]; /* stratum header fields, hex */
    char     xnonce1[68], mask[16];                           /* extranonce1 and mask, hex */
    char     coinb1[MC_COINB_HEX_LEN], coinb2[MC_COINB_HEX_LEN]; /* coinbase halves, hex */
    char     merkle[MC_MERKLE_HEX_LEN];                          /* merkle branches, hex */
} mc_asic_job;

_Static_assert(sizeof(mc_asic_job) == 27008,
        "mc_asic_job is the wire struct; its size is the version axis and cannot change silently");

#define ASIC_PLL_POINTS 4 /* PLL points per chip, count */

/* MSG_ASIC_VF payload: PLL frequency set */
typedef struct {
    uint16_t freq[ASIC_PLL_POINTS]; /* PLL point frequency, MHz */
    uint8_t  at_target;             /* frequency ramp has reached its target */
} mc_asic_vf;

#define ASIC_EFUSE_WORDS 16 /* efuse words per chip, count */

/* MSG_ASIC_EFUSE payload */
typedef struct {
    uint32_t seq;
    uint16_t board; /* hash board index */
    uint16_t chip;
    uint8_t  last; /* last record of the dump */
    uint8_t  rsvd[3];
    uint32_t data[ASIC_EFUSE_WORDS];
} asic_efuse_rec;

_Static_assert(sizeof(asic_efuse_rec) == 76,
        "asic_efuse_rec is the wire struct; its size is the version axis and cannot change "
        "silently");

/* MSG_MM_EFUSE payload: efuse summary */
typedef struct {
    uint8_t present; /* efuse data read */
    uint8_t bin_grade;
    uint8_t rsvd[2];
} mm_efuse_state;

_Static_assert(sizeof(mm_efuse_state) == 4,
        "mm_efuse_state is the wire struct; its size is the version axis and cannot change "
        "silently");

/* MSG_CTRL_ACK payload */
typedef struct {
    int32_t  result; /* mc_set_result */
    uint32_t seq;    /* mc_ctrl_cmd.seq of the command answered */
    int64_t  arg;    /* value the reason names, see enum mc_ctrl_why */
    uint16_t why;    /* enum mc_ctrl_why */
    uint16_t rsv[3];
} mc_ctrl_ack;

_Static_assert(sizeof(mc_ctrl_ack) == 24,
        "mc_ctrl_ack is the wire struct; its size is the version axis and cannot change silently");

/* reason a control command was refused */
enum mc_ctrl_why {
    MC_CTRL_WHY_NONE = 0,
    MC_CTRL_WHY_PAST,     /* scheduled time already passed; arg: mm_miner time, Unix s */
    MC_CTRL_WHY_COOLDOWN, /* over-temperature cool-down; arg: seconds left */
    MC_CTRL_WHY_FAULT,    /* fault softoff latched; arg: enum mc_softoff_rsn */
};

#define ASIC_TEMP_NONE (-273) /* no temperature reading, degrees C */

typedef struct {
    uint16_t freq_mhz;           /* average core frequency, MHz */
    uint8_t  in_work;            /* any board mining */
    uint8_t  temp_valid;         /* always 0 */
    int16_t  temp_max;           /* always ASIC_TEMP_NONE */
    int16_t  temp_avg;           /* always ASIC_TEMP_NONE */
    uint16_t ps_vout_mv;         /* always 0 */
    uint16_t ps_iout_ma;         /* always 0 */
    uint16_t ps_pout_w;          /* always 0 */
    uint16_t ps_voutcmd_mv;      /* always 0 */
    uint16_t fan_rpm[FAN_COUNT]; /* always 0 */
    uint8_t  fan_pct;            /* always 0 */
    uint8_t  aging_bar;          /* always 0 */
    uint8_t  bin_grade;
    uint8_t  diag;              /* non-fatal diagnostics, kept out of asic_health */
    uint32_t mhs_mm;            /* nominal MH/s from cores and PLL frequency */
    uint32_t ghs_spd_mhs;       /* MH/s from speed-log pass counts */
    uint32_t realtime_dh_x1000; /* realtime DH, 0.001 % */
} asic_telem;

#define ERR_HU_ENUM               (1u << 1) /* hu_errcode bit: chip enum failed */
#define ERR_HU_COMM               (1u << 2) /* hu_errcode bit: board comm error */
#define ERR_HU_MCU                (1u << 3) /* hu_errcode bit: board MCU error */
#define ERR_HU_LOST               (1u << 4) /* hu_errcode bit: board lost */
#define CORE_HEALTH_OK            0u        /* core_health: healthy */
#define CORE_UNHEALTHY_MAIN_STALL (1u << 0) /* core_health bit: main loop stalled */
#define CORE_UNHEALTHY_API_STALL  (1u << 1) /* core_health bit: :4028 or trusted request stalled */

/* MSG_CORE_STATS payload */
typedef struct {
    uint32_t ghs_avg_mhs; /* average hashrate, MH/s */
    uint32_t core_health; /* CORE_HEALTH_* bits */
    uint32_t job_gap_s;   /* time since the last pool job, s; 0 before the first */
} mc_core_stats;

_Static_assert(sizeof(mc_core_stats) == 12,
        "mc_core_stats is the wire struct; its size is the version axis and cannot change "
        "silently");

enum mc_work_state {
    MC_WORK_INIT = 0, /* not mining */
    MC_WORK_WORK,     /* mining */
    MC_WORK_IDLE,     /* stopped by softoff */
    MC_WORK_FAULT     /* stopped by a fault; mm never sends it, a fault softoff is MC_WORK_IDLE */
};

/* MSG_MM_STATUS payload: miner state */
typedef struct {
    uint32_t state; /* enum mc_work_state */
} mc_mm_status;

/* MSG_MM_BOOTBY payload: boot reason */
typedef struct {
    uint32_t reason; /* enum mc_bootby */
    uint32_t sub;
} mc_mm_bootby;

_Static_assert(sizeof(mc_mm_bootby) == 8,
        "mc_mm_bootby is the wire struct; its size is the version axis and cannot change silently");

enum mc_softoff_rsn {
    MC_SOFTOFF_NONE          = 0, /* not soft-off */
    MC_SOFTOFF_POWER_FAULT   = 1,
    MC_SOFTOFF_HASH_FAULT    = 2,
    MC_SOFTOFF_ASIC_OVERHOT  = 3,
    MC_SOFTOFF_API           = 4, /* API or schedule request */
    MC_SOFTOFF_FAN_ERR       = 5, /* fan stalled or unreadable */
    MC_SOFTOFF_LAN_DAMAGED   = 6,
    MC_SOFTOFF_LOW_HASHRATE  = 7,
    MC_SOFTOFF_HASHMCU_ERR   = 8,
    MC_SOFTOFF_HW_ERR        = 9,
    MC_SOFTOFF_START_OVERHOT = 10,
    MC_SOFTOFF_CALI_FAILED   = 11, /* aging found no usable frequency */
};

#define HASH_SN_LEN 40 /* hash board serial number length, bytes */
#define MM_DNA_LEN  8  /* chip DNA length, bytes */

/* MSG_MM_PERIPH payload */
typedef struct {
    uint8_t  valid;
    uint8_t  ledmode;
    uint8_t  runeffect; /* running LED effect */
    uint8_t  fan_pct;   /* fan duty, % */
    uint16_t fan_rpm[FAN_COUNT];
    uint8_t  hb_count;                /* hash boards present, count */
    int16_t  hb_outlet[HBOARD_COUNT]; /* hash board outlet temperature, degrees C */
    int16_t  hb_inlet[HBOARD_COUNT];  /* hash board inlet temperature, degrees C */
    uint8_t  aging_bar;               /* aging progress, % */
    uint8_t  aging_finished;
    uint8_t  ps_valid;                 /* PSU readings valid */
    uint32_t hu_errcode[HBOARD_COUNT]; /* hash board error bits, ERR_HU_* */
    uint32_t mm_errcode;               /* controller error bits, ERR_MM_* */
    /* hash rail voltage, DC-DC rail counts, mV */
    uint16_t rail_mv;
    uint16_t ps_err; /* PSU error code */
    /* PSU voltage: DC-DC PD input, mV */
    uint16_t ps_vout_mv;
    /* PSU current: DC-DC PD input current, 1/PS_IOUT_PER_A A */
    uint16_t ps_iout_ma;
    uint16_t ps_pout_w; /* PSU output power, W */
    /* commanded rail voltage: DC-DC output setpoint, mV */
    uint16_t ps_voutcmd_mv;
    int16_t  board_temp;  /* control board temperature, degrees C */
    uint8_t  softoff_rsn; /* enum mc_softoff_rsn */
    char     ssid[128];
    int16_t  rssi; /* WiFi signal strength, dBm */
    uint8_t  net_devtype;
    uint8_t  lcd_bright; /* LCD brightness, low byte */
    uint32_t filtermesh; /* filter mesh usage time, minutes */
    uint8_t  lcd_onoff;
    uint16_t lcd_show; /* LCD display page */
    uint8_t  volume;
    uint8_t  lcd_bright_hi; /* LCD brightness, high byte */
    uint8_t  led_effect;
    uint8_t  led_bright;
    uint32_t led_temper; /* LED color temperature */
    uint32_t led_rgb;
    char     hash_sn[HBOARD_COUNT][HASH_SN_LEN + 1]; /* hash board serial numbers */
    uint8_t  dna[MM_DNA_LEN];
    uint16_t cali_max_pout;     /* calibration max power, W */
    uint16_t cali_temp;         /* calibration target temp, degrees C */
    uint16_t cali_volt;         /* calibrated rail voltage, rail counts */
    uint16_t cali_pll_start;    /* calibration start frequency, MHz */
    uint16_t cali_pll_interval; /* calibration frequency step, MHz */
    uint8_t  cali_aged;
    char     webpass[WEBPASS_HEX_LEN + 1];
    uint8_t  workmode;
    uint8_t  caliall; /* calibrate all modes */
    uint8_t  activation;
    char     prod[PROD_NAME_CAP]; /* product name */
    char     model[MODEL_NAME_CAP];
    uint8_t  maxmode;            /* highest work mode */
    char     tz[MM_TZ_NAME_CAP]; /* timezone name, [syscfg] timezone */
} mm_periph;

#define ASIC_CORES_PER_CHIP 152 /* hashing cores per chip, count */

/* MSG_ASIC_TELEM payload */
typedef struct {
    asic_telem tm;                                   /* must stay first */
    uint16_t   boards;                               /* HBOARD_COUNT */
    uint16_t   chips[HBOARD_COUNT];                  /* chips enumerated; 0 if the board is off */
    int16_t    pvt_t[HBOARD_COUNT][MINER_MAX_CHIPS]; /* degrees C, or ASIC_TEMP_NONE */
    uint16_t   pvt_v[HBOARD_COUNT][MINER_MAX_CHIPS]; /* mV */
    /* cores per PLL point */
    uint8_t pllcnt[HBOARD_COUNT][MINER_MAX_CHIPS][ASIC_PLL_POINTS];
    /* measured PLL point frequency, MHz */
    uint16_t cf[HBOARD_COUNT][MINER_MAX_CHIPS][ASIC_PLL_POINTS];
    /* speed-log pass/fail counts of the last completed window */
    uint32_t spdlog_pass[HBOARD_COUNT][MINER_MAX_CHIPS];
    uint32_t spdlog_fail[HBOARD_COUNT][MINER_MAX_CHIPS];
    /* chip errors plus UART failures since bring-up */
    uint32_t err_crc[HBOARD_COUNT];
    uint32_t com_crc[HBOARD_COUNT]; /* UART failures since bring-up */
    /* every chip has a temperature read */
    uint8_t  pvt_lapped[HBOARD_COUNT];
    uint32_t asic_health;              /* ASIC_HEALTH_*, non-OK fails the watchdog gate */
    uint32_t nonce_discarded;          /* nonces dropped on a full ring */
    uint32_t local_work[HBOARD_COUNT]; /* works sent since bring-up */
} asic_chip_telem;

#define ASIC_HEALTH_OK            0u        /* asic_health: healthy */
#define ASIC_UNHEALTHY_MAIN_STALL (1u << 0) /* asic_health bit: main loop stalled */

static inline uint8_t efuse_bin_majority(uint8_t a, uint8_t b, uint8_t c)
{
    return (uint8_t)((a & b) | (a & c) | (b & c));
}

static inline uint8_t efuse_bin_grade(const uint32_t *w)
{
    return efuse_bin_majority((uint8_t)(w[11] & 0xffu), (uint8_t)(w[10] & 0xffu),
            (uint8_t)((w[10] >> 8) & 0xffu));
}

#endif /* MINER_IPC_H */
