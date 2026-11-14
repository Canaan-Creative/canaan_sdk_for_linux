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

#include "ui_backlight.h"

#include <pthread.h>

#include "boardconf.h"
#include "gpio_dev.h"
#include "osif.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_probed;

static void probe_gpio(void)
{
    gpio_set_direction(BACKLIGHT_GPIO, GPIO_OUT_LOW);
    osif_log(OSIF_LOG_DBG, "LCD backlight: on/off via GPIO %d", BACKLIGHT_GPIO);
}

void hal_ui_backlight_set(int on, int pct)
{
    pthread_mutex_lock(&g_lock);
    if (!g_probed) {
        probe_gpio();
        g_probed = 1;
    }
    gpio_set_value(BACKLIGHT_GPIO, on && pct > 0 ? GPIO_HIGH : GPIO_LOW);
    pthread_mutex_unlock(&g_lock);
}
