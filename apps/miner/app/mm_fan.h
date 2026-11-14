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

#ifndef MM_FAN_H
#define MM_FAN_H

#include <stdatomic.h>
#include <stdint.h>

#include "softonoff.h"
#include "enum_hold.h"

#define TELEM_STALE_FAILSAFE_MS 90000u /* no usable chip temp before full-speed failsafe, ms */

typedef struct mm_fan_ctx {
    const softonoff_state_t *softonoff;
    const enum_hold_t       *enum_hold;
    const _Atomic int       *fault_off;
} mm_fan_ctx_t;

extern atomic_int g_fan_dev_ok;

void fan_rule(void *ctx, uint64_t now_ms);
void mm_fan_target_apply(uint8_t mode);
void mm_fan_target_set_manual(uint16_t temp);
void mm_fan_pid_fixed(int duty_pct);
void mm_fan_set_fixed(int duty);

#endif /* MM_FAN_H */
