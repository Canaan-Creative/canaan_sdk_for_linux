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

#ifndef BOARD_X3_H
#define BOARD_X3_H

#define CAP_HASHBOARD 1
#define CAP_FAN       1
#define CAP_FAN_TACH  1
#define CAP_HB_MCU    1 /* 1 when each hashboard slot has a monitor MCU */
#define CAP_LED_STRIP 0
#define CAP_LCD       1
#define CAP_BUTTON    0
#define CAP_BEEP      0
#define NET_WIRED_ALT 0 /* 1 when a wired USB dongle may replace the WiFi one */
/* 1 when the power supply is a PD input with a DC-DC core rail */
#define IMPL_PSU_IS_DCDC_PD 1
#define PD_POWER_RESERVE_W  3u /* power held back from the PD input limit, W */
/* 1 when the board temperature is an NTC read through an ADC */
#define IMPL_BOARD_TEMP_IS_ADC_NTC 1
#define IMPL_NET_IS_WIRED          1
#define IMPL_NET_IS_WIFI           0

#define IMPL_BACKLIGHT_IS_GPIO 1
#define BACKLIGHT_GPIO         (25)
#define BOARD_NTC_ADC_CH       (1)
#define BOARD_NTC_R_PULLUP     (10000) /* NTC divider pull-up resistance, ohms */
#define BOARD_NTC_R_25         (10000) /* NTC resistance at 25 degC, ohms */
#define BOARD_NTC_B_CONST      (3950)  /* NTC B constant, K */
/* INA226 PD input voltage below which supply is absent, mV */
#define POWER_VALID_VOLT_MIN 8000
#define BOARD_CALI_HEADER    "board_cali_x3.h" /* header defining BOARD_CALI_ROWS */
#define PS_IOUT_PER_A        1000              /* PSU output current units per A */
#define WORK_MODE_DEFAULT_ID 2
#define WORK_LEVEL_COUNT     1 /* work levels per work mode */
/* Inclusive [lo, hi] spans of the work modes ascset workmode and work_mode_lvl accept. */
#define BOARD_WORKMODE_RANGES(X) X(0, 0) X(2, 2)
#define LED_PIXEL_COUNT          (1)
#define I2C_MUX_RESET_GPIO       (44) /* I2C mux reset_n GPIO */
#define SCAN_CADENCE_MS          (8)  /* nonce scan sleep, ms */
#ifndef FAN_PWM_ACTIVE_LOW
#define FAN_PWM_ACTIVE_LOW (0)
#endif
#define FAN_TACH_PULSES_PER_REV  (4)
#define FAN_DUTY_MIN_PCT         (20)  /* lowest fan duty, percent */
#define FAN_DUTY_INIT_PCT        (30)  /* fan duty below the PID temperature band, percent */
#define FAN_DUTY_MAX_PCT         (100) /* highest fan duty, percent */
#define FAN_DUTY_DEFAULT_PCT     (60)  /* fan duty at startup, percent */
#define FAN_DUTY_API_SOFTOFF_PCT (20)  /* fan duty during an API soft-off, percent */
#define FAN_DUTY_SOFTOFF_PCT     (100) /* fan duty in a thermal or fan-fault soft-off, percent */
#define FAN_DUTY_ENUM_PCT        (80)  /* fan duty during enumeration, 0 = not pinned, percent */
/* fan duty during a hash fault, percent; 0 = clamped to FAN_DUTY_MIN_PCT */
#define FAN_DUTY_HASH_FAULT_PCT (20)
#define FAN_TARGET_TEMP         (80) /* fan target temperature before calibration loads, degC */
#define FAN_PID_P               (50)
#define FAN_PID_I               (2)
#define FAN_PID_D               (1)
#define FAN_PID_TEMP_MIN        (45) /* lowest temperature of the fan PID band, degC */
#define FAN_PID_TEMP_MAX        (95) /* highest temperature of the fan PID band, degC */
/* largest PID output increase per step, 0 = unlimited, steps */
#define FAN_PID_DELTA_UP       (30)
#define FAN_PID_ERR_QUANT      (1) /* 1 truncates the PID error toward zero to 0.5 degC */
#define FAN_PID_DUTY_WHOLE_PCT (1) /* 1 truncates the PID duty to whole percent */
#define FAN_PID_INSTART_HOLD   (1) /* 1 holds full duty until the first chip temperature arrives */
#define PVT_TEMP_OVER_HR_MAX   (105) /* chip temperature that trips the thermal soft-off, degC */
/* hashboard outlet temperature that trips the thermal soft-off, degC */
#define HASHOUT_OVER_HR_MAX (90)
/* consecutive hot outlet samples that trip the thermal soft-off, 0 = disarmed */
#define HASHOUT_OVERHOT_CNT_MAX (3)
#define ADG_STAGE_COUNT_UP      (9) /* aging up-sweep stages */
#define ADG_STAGE_COUNT_DOWN    (4) /* aging down-sweep stages */
#define POUT_UP_LIMIT           (7) /* output power the servo accepts above the row max_pout, W */
/* output power the servo accepts below the row max_pout, negative, W */
#define POUT_DOWN_LIMIT         (-7)
#define MAX_POUT_LIMIT_ABNORMAL (900)        /* output power reading above this is invalid, W */
#define I2C_PD_DEV              "/dev/i2c-1" /* power supply control I2C bus device path */
#define I2C_POWER_E2P_DEV       "/dev/i2c-1" /* DC-DC and hashboard EEPROM I2C bus device path */
#define I2C_MCU_DEV             "/dev/i2c-4" /* hashboard MCU I2C bus device path */
#define MM_NET_IFACE            "eth1"       /* primary network interface name */
#define DEVNAME_PRE             "x3_"        /* LAN and BLE device name prefix */
/* fan rows: BOARD_FAN_ROW(PWM pad, tachometer device path, pulses per revolution) */
/* clang-format off */
#define BOARD_FAN_ROWS \
    BOARD_FAN_ROW(1, "/dev/timer0", FAN_TACH_PULSES_PER_REV), \
    BOARD_FAN_ROW(4, "/dev/timer1", FAN_TACH_PULSES_PER_REV), \
    BOARD_FAN_ROW(5, "/dev/timer3", FAN_TACH_PULSES_PER_REV),
/* clang-format on */
/* hashboard slot rows: { ASIC UART device path or NULL, RI GPIO or 0 } */
#define BOARD_HBOARD_ROWS { "/dev/ttyS1", 10 }, { "/dev/ttyS2", 11 }, { NULL, 12 },
/* hashboard monitor MCU rows: { reset GPIO, BSL GPIO } */
#define BOARD_HB_MCU_ROWS { 40, 41 }, { 42, 43 }, { 46, 47 },
/* CTRL_OP_* codes the board accepts */
#define BOARD_CTRL_OPS CTRL_OP_WORKMODE, CTRL_OP_SETTZ, CTRL_OP_REBOOT

#endif /* BOARD_X3_H */
