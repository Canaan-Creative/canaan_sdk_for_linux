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

#ifndef TEST_LVGL_H
#define TEST_LVGL_H
#include <stdint.h>
static inline void lv_tick_set_cb(uint32_t (*callback)(void))
{
    (void)callback;
}
static inline uint32_t lv_timer_handler(void)
{
    return 0;
}
#endif
