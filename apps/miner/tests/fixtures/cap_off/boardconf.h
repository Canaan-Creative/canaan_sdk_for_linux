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

#ifndef TST_CAP_OFF_BOARDCONF_H
#define TST_CAP_OFF_BOARDCONF_H

#include_next "boardconf.h"

#ifdef TST_CAP_LCD_OFF
#undef CAP_LCD
#define CAP_LCD 0
#endif /* TST_CAP_LCD_OFF */

#ifdef TST_CAP_ACTIVATION_OFF
#undef CAP_ACTIVATION
#endif /* TST_CAP_ACTIVATION_OFF */

#ifdef TST_WORKMODE_SINGLE
#undef WORK_MODE_DEFAULT_ID
#define WORK_MODE_DEFAULT_ID 0
#undef BOARD_WORKMODE_RANGES
#define BOARD_WORKMODE_RANGES(X) X(0, 0)
#endif /* TST_WORKMODE_SINGLE */

#endif /* TST_CAP_OFF_BOARDCONF_H */
