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

#ifndef MM_UI_GLUE_H
#define MM_UI_GLUE_H

#include <stdint.h>

#ifdef SUPPORT_LCD
void mm_ui_start(void);
void mm_ui_tick(void);
int  mm_ui_btn_wait(uint32_t ms);
void mm_ui_btn_drain(void);
void mm_ui_night_mode(int dark);
void mm_ui_wallpaper_reload(void);
void mm_ui_stop(void);
int  mm_ui_poweroff_requested(void);
#else /* SUPPORT_LCD */
#include "osif.h"

static inline void mm_ui_start(void)
{
}
static inline void mm_ui_tick(void)
{
}
static inline int mm_ui_btn_wait(uint32_t ms)
{
    osif_sleep_ms(ms);
    return 0;
}
static inline void mm_ui_btn_drain(void)
{
}
static inline void mm_ui_night_mode(int dark)
{
    (void)dark;
}
static inline void mm_ui_wallpaper_reload(void)
{
}
static inline void mm_ui_stop(void)
{
}
static inline int mm_ui_poweroff_requested(void)
{
    return 0;
}
#endif /* SUPPORT_LCD */

#endif /* MM_UI_GLUE_H */
