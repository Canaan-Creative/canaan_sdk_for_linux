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

/*
 * Op to apply-handler routing for mc_ctrl_cmd in mm_miner. Each
 * CTRL_ROUTE_<op> names the handler for one CTRL_CMD_LIST entry.
 */
#ifndef MM_CTRL_H
#define MM_CTRL_H

#include <stdint.h>

#include "ctrl_cmds.h"
#include "mc_set_result.h"

#define CTRL_ROUTE_WORKMODE     mm_apply_workmode
#define CTRL_ROUTE_LEDMODE      route_led
#define CTRL_ROUTE_LEDSET       route_led
#define CTRL_ROUTE_NIGHTLAMP    route_led
#define CTRL_ROUTE_FILTER_CLEAN route_filter_clean
#define CTRL_ROUTE_LCD          route_lcd

#define CTRL_ROUTE_SOFTON  mm_apply_soft
#define CTRL_ROUTE_SOFTOFF mm_apply_soft

#define CTRL_ROUTE_SETTZ     route_settz
#define CTRL_ROUTE_WP_RELOAD route_wp_reload
#define CTRL_ROUTE_REBOOT    route_reboot

/* Apply one control command; @op selects the variant when a handler serves several. */
typedef mc_set_result (*ctrl_apply_fn)(uint16_t op, const mc_ctrl_cmd *c);

mc_set_result mm_ctrl_apply(const mc_ctrl_cmd *c);
void          mm_ctrl_cmd(const void *data, int len);
void          mm_ctrl_refuse(uint16_t why, int64_t arg);

#endif /* MM_CTRL_H */
