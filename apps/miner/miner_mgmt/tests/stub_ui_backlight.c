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

/* Last hal_ui_backlight_set() state: on 0/1, brightness in percent (0 when off) */
int  physical_on, physical_brightness;
void hal_ui_backlight_set(int on, int pct)
{
    physical_on         = on;
    physical_brightness = on ? pct : 0;
}
