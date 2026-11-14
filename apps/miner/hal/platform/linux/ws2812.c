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

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "osif.h"
#include "ws2812.h"

#define DEV_PATH "/dev/ws2812" /* LED strip device node */

/* one LED as written to DEV_PATH, GRB order */
typedef struct led_value {
    uint8_t green; /* green brightness, 0..255 */
    uint8_t red;   /* red brightness, 0..255 */
    uint8_t blue;  /* blue brightness, 0..255 */
} led_value_t;

static int g_ws2812_fd = -1; /* DEV_PATH fd, -1 when closed */

int w2812_set(uint32_t *led_rgb)
{
    led_value_t set_val[LED_NUM];

    if (g_ws2812_fd < 0) {
        return -1;
    }

    for (int i = 0; i < LED_NUM; i++) {
        set_val[i].red   = (uint8_t)(led_rgb[i] >> 16);
        set_val[i].green = (uint8_t)(led_rgb[i] >> 8);
        set_val[i].blue  = (uint8_t)led_rgb[i];
    }

    ssize_t want = (ssize_t)(LED_NUM * sizeof(led_value_t));
    if (write(g_ws2812_fd, set_val, (size_t)want) != want) {
        return -1;
    }
    return 0;
}

int w2812_init()
{
    if (!CAP_LED_STRIP) {
        return -1;
    }

    g_ws2812_fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (g_ws2812_fd < 0) {
        osif_log(OSIF_LOG_WARN, "LED strip: cannot open " DEV_PATH ": %s", strerror(errno));
        return -1;
    }
    return 0;
}

int w2812_close()
{
    if (g_ws2812_fd < 0) {
        return -1;
    }
    close(g_ws2812_fd);
    g_ws2812_fd = -1;
    return 0;
}
