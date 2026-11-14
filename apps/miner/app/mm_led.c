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

#include "mm_led.h"
#include "cfg_u32.h"
#include "ctrl_args.h"
#include "ctrl_cmds.h"
#include "led_fx.h"
#include "mm_cfgstore.h"
#include "ws2812.h"
#include "osif.h"

#include "mm_ui_glue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

_Static_assert(LED_NUM == LED_PIXELS, "ws2812 LED_NUM != led_fx LED_PIXELS");

typedef struct {
    led_state_t effect;            /* led_fx_tick() renderer state */
    uint32_t    frame[LED_PIXELS]; /* pixel colors, 0xRRGGBB each */
    int         pending;           /* frame not yet written to the strip */
    uint64_t    retry_at;          /* osif_now_ms() of the next write attempt, ms */
} led_write_state;

static struct {
    pthread_t       th;            /* effect thread, joined by mm_led_stop */
    pthread_mutex_t lock;          /* guards cfg, runeffect and lamp_deadline */
    _Atomic int     run;           /* effect thread keeps running while set */
    mm_led_cfg_t    cfg;           /* applied settings, loaded from and saved to systemcfg.ini */
    uint8_t         runeffect;     /* LED_FX_* the thread renders */
    uint64_t        lamp_deadline; /* osif_now_ms() at which the night lamp closes, ms; 0 none */
} g = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Serializes mm_led_apply, mm_led_reload and mm_led_wake, saves included */
static pthread_mutex_t operation_lock = PTHREAD_MUTEX_INITIALIZER;

static void mm_led_cfg_load(mm_led_cfg_t *c)
{
    const char *const sects[] = { "syscfg", "led", "led", "led", "led" };
    const char *const keys[]  = { "ledmode", "effect", "bright", "temper", "rgb" };
    const uint32_t    max[]   = { 1, LED_FX_USER_MAX, 100, 0xFFFFu, LED_RGB_MAX };
    uint32_t          val[]   = { c->ledmode, c->effect, c->bright, c->temper, c->rgb };

    for (size_t i = 0; i < sizeof val / sizeof val[0]; i++) {
        char v[32];
        if (mm_cfgstore_get(sects[i], keys[i], v, sizeof v) != 0)
            continue;
        if (cfg_u32_parse(v, max[i], &val[i]) != 0)
            osif_log(OSIF_LOG_WARN,
                    "LED setting [%s] %s = \"%s\" is not a valid number; "
                    "keeping %u",
                    sects[i], keys[i], v, (unsigned)val[i]);
    }
    c->ledmode = (uint8_t)val[0];
    c->effect  = (uint8_t)val[1];
    c->bright  = (uint8_t)val[2];
    c->temper  = val[3];
    c->rgb     = val[4];
}

static int set_led_keys(const mm_led_cfg_t *c)
{
    char              mode[16], effect[16], bright[16], temper[16], rgb[16];
    const char *const sects[] = { "syscfg", "led", "led", "led", "led" };
    const char *const keys[]  = { "ledmode", "effect", "bright", "temper", "rgb" };
    const char *const vals[]  = { mode, effect, bright, temper, rgb };

    snprintf(mode, sizeof mode, "%u", c->ledmode);
    snprintf(effect, sizeof effect, "%u", c->effect);
    snprintf(bright, sizeof bright, "%u", c->bright);
    snprintf(temper, sizeof temper, "%u", c->temper);
    snprintf(rgb, sizeof rgb, "%u", c->rgb);
    return mm_cfgstore_set_n(sects, keys, vals, 5);
}

static void mm_led_cfg_save(const mm_led_cfg_t *c)
{
    if (set_led_keys(c) == 0 || mm_cfgstore_frozen())
        return;
    osif_log(OSIF_LOG_ERR, "light-strip settings NOT persisted; they are lost on reboot");
}

static void led_write_flush(led_write_state *output, uint64_t now)
{
    if (!output->pending || now < output->retry_at)
        return;
    if (w2812_set(output->frame) == 0) {
        output->pending  = 0;
        output->retry_at = 0;
    } else {
        output->retry_at = now + 1000;
        osif_log(OSIF_LOG_WARN, "LED strip update failed; retrying");
    }
}

static void led_write_tick(led_write_state *output, uint8_t effect, uint32_t rgb, uint64_t now)
{
    if (output->effect.prev_fx != effect || output->effect.prev_rgb != rgb) {
        output->pending  = 0;
        output->retry_at = 0;
    }
    if (led_fx_tick(&output->effect, effect, rgb, output->frame))
        output->pending = 1;
    led_write_flush(output, now);
}

static void expire_lamp(void)
{
    if (g.lamp_deadline && osif_now_ms() >= g.lamp_deadline) {
        g.lamp_deadline = 0;
        g.runeffect     = LED_FX_SHRINK;
    }
}

static void reset_cfg(void)
{
    g.cfg           = mm_led_cfg_default;
    g.lamp_deadline = 0;
    mm_led_cfg_load(&g.cfg);
    g.runeffect = g.cfg.ledmode ? (uint8_t)LED_FX_DARK : g.cfg.effect;
}

static void *led_thread(void *arg)
{
    (void)arg;
    led_write_state output = { 0 };

    pthread_mutex_lock(&g.lock);
    output.effect.prev_fx  = g.runeffect;
    output.effect.prev_rgb = g.cfg.rgb;
    uint32_t initial       = g.cfg.ledmode ? LED_RGB_BLACK : g.cfg.rgb;
    pthread_mutex_unlock(&g.lock);
    for (int pixel = 0; pixel < LED_PIXELS; pixel++)
        output.frame[pixel] = initial;
    output.pending = 1;
    led_write_flush(&output, osif_now_ms());

    while (atomic_load(&g.run)) {
        pthread_mutex_lock(&g.lock);
        expire_lamp();
        uint8_t  fx  = g.runeffect;
        uint32_t rgb = g.cfg.rgb;
        pthread_mutex_unlock(&g.lock);

        led_write_tick(&output, fx, rgb, osif_now_ms());
        osif_sleep_ms(LED_TICK_MS);
    }
    return NULL;
}

void mm_led_start(void)
{
    reset_cfg();
    osif_log(OSIF_LOG_DBG,
            "LED: %s mode, effect %u, color #%06X, brightness %u, "
            "color temperature %u",
            g.cfg.ledmode ? "dark" : "day", g.cfg.effect, g.cfg.rgb, g.cfg.bright, g.cfg.temper);

    if (w2812_init() != 0) {
        /* Decorative, and its kernel node is a board option: never block bring-up
         * on it. Commands still validate and persist, so the setting survives to
         * the next boot / a fixed device tree. */
        return;
    }

    atomic_store(&g.run, 1);
    if (pthread_create(&g.th, NULL, led_thread, NULL) != 0) {
        atomic_store(&g.run, 0);
        osif_log(OSIF_LOG_ERR,
                "LED effects unavailable: thread start failed, "
                "the strip keeps its current color");
    }
}

void mm_led_reload(void)
{
    pthread_mutex_lock(&operation_lock);
    pthread_mutex_lock(&g.lock);
    reset_cfg();
    mm_led_cfg_t snap = g.cfg;
    pthread_mutex_unlock(&g.lock);
    pthread_mutex_unlock(&operation_lock);
    osif_log(OSIF_LOG_INFO, "LED settings reloaded: %s mode, effect %u, color #%06X",
            snap.ledmode ? "dark" : "day", snap.effect, snap.rgb);
}

static void set_ledmode(uint8_t mode)
{
    pthread_mutex_lock(&g.lock);
    if (mode)
        g.runeffect = LED_FX_DARK;
    else if (g.cfg.ledmode)
        g.runeffect = g.cfg.effect;
    g.cfg.ledmode     = mode;
    g.lamp_deadline   = 0;
    mm_led_cfg_t snap = g.cfg;
    pthread_mutex_unlock(&g.lock);

    mm_ui_night_mode(mode);
    mm_led_cfg_save(&snap);
    osif_log(OSIF_LOG_INFO, "LED mode set to %s", mode ? "dark" : "day");
}

static mc_set_result apply_ledmode(const char *args)
{
    uint8_t mode;
    if (ctrl_parse_ledmode(args, &mode) != 0)
        return MC_SET_ERR_ARG;
    set_ledmode(mode);
    return MC_SET_OK;
}

static mc_set_result apply_ledset(const char *args)
{
    pthread_mutex_lock(&g.lock);
    mm_led_cfg_t snap = g.cfg;
    pthread_mutex_unlock(&g.lock);

    if (ctrl_parse_ledset(args, &snap.effect, &snap.bright, &snap.temper, &snap.rgb) != 0)
        return MC_SET_ERR_ARG;
    if (snap.ledmode) {
        osif_log(OSIF_LOG_WARN, "LED setting \"%s\" refused: the LED is in dark mode", args);
        return MC_SET_ERR_ARG;
    }

    pthread_mutex_lock(&g.lock);
    g.cfg       = snap;
    g.runeffect = snap.effect;
    pthread_mutex_unlock(&g.lock);

    mm_led_cfg_save(&snap);
    osif_log(OSIF_LOG_INFO,
            "LED set: effect %u, color #%06X, brightness %u, "
            "color temperature %u",
            snap.effect, snap.rgb, snap.bright, snap.temper);
    return MC_SET_OK;
}

static mc_set_result apply_nightlamp(const char *args)
{
    uint8_t  mode;
    uint32_t dur_s;
    if (ctrl_parse_nightlamp(args, &mode, &dur_s) != 0)
        return MC_SET_ERR_ARG;
    if (!dur_s)
        dur_s = 180;

    pthread_mutex_lock(&g.lock);
    if (!g.cfg.ledmode) {
        pthread_mutex_unlock(&g.lock);
        return MC_SET_ERR_ARG;
    }
    if (mode) {
        g.runeffect     = LED_FX_EXTEND;
        g.lamp_deadline = osif_now_ms() + (uint64_t)dur_s * 1000u;
    } else {
        g.runeffect     = LED_FX_SHRINK;
        g.lamp_deadline = 0;
    }
    pthread_mutex_unlock(&g.lock);

    if (mode)
        osif_log(OSIF_LOG_INFO, "Night lamp on for %u s", dur_s);
    else
        osif_log(OSIF_LOG_INFO, "Night lamp off");
    return MC_SET_OK;
}

void mm_led_state(uint8_t *ledmode, uint8_t *runeffect)
{
    pthread_mutex_lock(&g.lock);
    expire_lamp();
    *ledmode   = g.cfg.ledmode;
    *runeffect = g.runeffect;
    pthread_mutex_unlock(&g.lock);
}

void mm_led_snapshot(mm_led_cfg_t *cfg, uint8_t *runeffect)
{
    pthread_mutex_lock(&g.lock);
    expire_lamp();
    *cfg       = g.cfg;
    *runeffect = g.runeffect;
    pthread_mutex_unlock(&g.lock);
}

int mm_led_wake(void)
{
    pthread_mutex_lock(&operation_lock);
    pthread_mutex_lock(&g.lock);
    uint8_t mode = g.cfg.ledmode;
    pthread_mutex_unlock(&g.lock);
    if (mode)
        set_ledmode(0);
    pthread_mutex_unlock(&operation_lock);
    return mode != 0;
}

mc_set_result mm_led_apply(uint16_t op, const char *args)
{
    mc_set_result result;
    pthread_mutex_lock(&operation_lock);
    switch (op) {
    case CTRL_OP_LEDMODE:
        result = apply_ledmode(args);
        break;
    case CTRL_OP_LEDSET:
        result = apply_ledset(args);
        break;
    case CTRL_OP_NIGHTLAMP:
        result = apply_nightlamp(args);
        break;
    default:
        result = MC_SET_ERR_UNSUP;
        break;
    }
    pthread_mutex_unlock(&operation_lock);
    return result;
}

void mm_led_stop(void)
{
    if (atomic_load(&g.run)) {
        atomic_store(&g.run, 0);
        pthread_join(g.th, NULL);
    }
    w2812_close();
}
