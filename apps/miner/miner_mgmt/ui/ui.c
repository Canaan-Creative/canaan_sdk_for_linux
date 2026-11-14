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

#include "ui_screen.h"
#include "ui_dispatch.h"
#include "ui_types.h"
#include "ui_backlight.h"

#include "lvgl/lvgl.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UI_QUEUE_MAX     16   /* trigger queue capacity, entries */
#define UI_TICK_SLEEP_US 5000 /* UI thread loop sleep, us */
#define UI_DISPATCH_MS   100  /* trigger drain period, ms */
#define UI_ADVANCE_TICKS 30   /* drain periods between screen advances */
#define UI_REFRESH_MS    1000 /* summary refresh period, ms */

_Static_assert(UI_SHOW_MAX <= 32, "fault popup bitmaps hold one bit per screen id");

struct ui_trigger {
    int      action;  /* UI_ACT_* */
    uint16_t id;      /* screen id, UI_* */
    uint32_t request; /* regular-switch request number, 0 none */
};
/* serializes regular-switch requests */
static pthread_mutex_t switch_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    pthread_t    thread;
    volatile int running; /* UI thread keeps looping */
    int          started; /* UI thread created */

    ui_summary_cb cb; /* summary data source */
    void         *cb_arg;

    pthread_mutex_t   qlock;               /* guards the fields below */
    struct ui_trigger queue[UI_QUEUE_MAX]; /* pending screen actions */
    int               qcount;
    uint32_t          next_request;      /* last regular-switch request number issued */
    uint32_t          completed_request; /* last regular-switch request number applied */
    uint16_t          completed_show;    /* regular screen after completed_request */
    int               pending_onoff;     /* requested display on (1) / off (0), -1 none */
    int               cancel_night;      /* night popup to be removed */
    int               pending_night;     /* night popup to be added */
    int               night_position;    /* queue index the night popup is applied at */
    int               pending_wp_reload; /* wallpaper to be reloaded */
    /* fault popups to add / delete, bit n = screen id n */
    uint32_t fault_add, fault_delete;
    int      brightness_dirty;
    int      brightness; /* backlight brightness, percent */
    uint16_t regular;
} g_ui = {
    .qlock         = PTHREAD_MUTEX_INITIALIZER,
    .pending_onoff = -1,
    .brightness    = 100,
    .regular       = UI_SUMMARY,
};

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static int drain_triggers(ui_dispatch *d)
{
    struct ui_trigger local[UI_QUEUE_MAX];
    int n, onoff, wp_reload, brightness, brightness_dirty, cancel_night, night, night_position,
            changed = 0;
    uint32_t fault_add, fault_delete;

    pthread_mutex_lock(&g_ui.qlock);
    fault_add      = g_ui.fault_add;
    fault_delete   = g_ui.fault_delete;
    g_ui.fault_add = g_ui.fault_delete = 0;
    n                                  = g_ui.qcount;
    memcpy(local, g_ui.queue, (size_t)n * sizeof(local[0]));
    g_ui.qcount            = 0;
    onoff                  = g_ui.pending_onoff;
    g_ui.pending_onoff     = -1;
    cancel_night           = g_ui.cancel_night;
    g_ui.cancel_night      = 0;
    night                  = g_ui.pending_night;
    night_position         = g_ui.night_position;
    g_ui.pending_night     = 0;
    wp_reload              = g_ui.pending_wp_reload;
    brightness             = g_ui.brightness;
    brightness_dirty       = g_ui.brightness_dirty;
    g_ui.brightness_dirty  = 0;
    g_ui.pending_wp_reload = 0;
    pthread_mutex_unlock(&g_ui.qlock);

    if (cancel_night)
        ui_show_delete(d, UI_NIGHT);

    if (onoff >= 0) {
        d->onoff = onoff;
        hal_ui_backlight_set(onoff, brightness);
        changed = 3;
    }

    if (brightness_dirty && onoff < 0)
        hal_ui_backlight_set(d->onoff, brightness);

    if (wp_reload)
        ui_screen_reload_wallpaper();

    for (int i = 0; i <= n; i++) {
        if (night && i == night_position) {
            ui_show_add(d, UI_NIGHT);
            if (!d->popup || d->popup->show.id != UI_NIGHT) {
                d->onoff = 0;
                hal_ui_backlight_set(0, 0);
            }
            changed |= 1;
        }
        if (i == n)
            break;
        switch (local[i].action) {
        case UI_ACT_SWITCH:
            ui_show_switch(d, (int16_t)local[i].id);
            break;
        case UI_ACT_ADD:
            ui_show_add(d, local[i].id);
            break;
        case UI_ACT_DELETE:
            ui_show_delete(d, local[i].id);
            break;
        default:
            break;
        }
        if (local[i].request) {
            pthread_mutex_lock(&g_ui.qlock);
            g_ui.completed_show    = d->regular.id;
            g_ui.completed_request = local[i].request;
            pthread_mutex_unlock(&g_ui.qlock);
        }
        changed |= 1;
    }

    for (uint16_t id = 0; id < UI_SHOW_MAX; id++) {
        uint32_t bit = 1u << id;
        if (fault_delete & bit) {
            ui_show_delete(d, id);
            changed |= 1;
        }
        if (fault_add & bit) {
            ui_show_add(d, id);
            changed |= 1;
        }
    }

    pthread_mutex_lock(&g_ui.qlock);
    g_ui.regular = d->regular.id;
    pthread_mutex_unlock(&g_ui.qlock);
    return changed;
}

static void *ui_thread(void *arg)
{
    (void)arg;
    ui_dispatch d;

    if (ui_screen_init() != 0) {
        pthread_mutex_lock(&g_ui.qlock);
        g_ui.running = 0;
        pthread_mutex_unlock(&g_ui.qlock);
        return NULL;
    }
    lv_tick_set_cb(tick_ms);
    ui_dispatch_init(&d, UI_SUMMARY);

    uint16_t last_shown    = UI_OFF;
    uint32_t loop_cnt      = 0;
    uint32_t last_dispatch = tick_ms();
    uint32_t last_refresh  = last_dispatch;

    while (g_ui.running) {
        uint32_t now = tick_ms();

        if ((uint32_t)(now - last_dispatch) >= UI_DISPATCH_MS) {
            last_dispatch = now;
            loop_cnt++;
            int force = drain_triggers(&d);
            if (loop_cnt >= UI_ADVANCE_TICKS || force) {
                loop_cnt = 0;
                if (last_shown == UI_NIGHT && !(force & 2)) {
                    d.onoff = 0;
                    hal_ui_backlight_set(0, 0);
                }
                ui_show show = ui_pick_show(&d);
                if (show.id != last_shown) {
                    ui_screen_render(show.id);
                    last_shown = show.id;
                }
            }
        }

        if ((uint32_t)(now - last_refresh) >= UI_REFRESH_MS) {
            last_refresh = now;
            if (g_ui.cb) {
                ui_summary s;
                memset(&s, 0, sizeof(s));
                if (g_ui.cb(&s, g_ui.cb_arg) == 0) {
                    s.err_flag |= ui_attach_err_flag(&d);
                    ui_screen_refresh(&s);
                }
            }
        }

        lv_timer_handler();
        usleep(UI_TICK_SLEEP_US);
    }

    ui_dispatch_free(&d);
    ui_screen_deinit();
    return NULL;
}

int ui_start(ui_summary_cb cb, void *cb_arg)
{
    if (g_ui.started)
        return 0;
    g_ui.cb      = cb;
    g_ui.cb_arg  = cb_arg;
    g_ui.running = 1;
    if (pthread_create(&g_ui.thread, NULL, ui_thread, NULL) != 0) {
        g_ui.running = 0;
        return -1;
    }
    g_ui.started = 1;
    return 0;
}

void ui_stop(void)
{
    if (!g_ui.started)
        return;
    g_ui.running = 0;
    pthread_join(g_ui.thread, NULL);
    g_ui.started = 0;
}

void ui_show_update(int action, uint16_t id)
{
    pthread_mutex_lock(&g_ui.qlock);
    if ((id == UI_POWER_ERR || id == UI_FAN_ERR || id == UI_FILTER_DIRTY || id == UI_OVERHOT ||
                id == UI_USB_DEVICE || id == UI_HASH_ERR || id == UI_FAN_STALL ||
                id == UI_AGING_ERR || id == UI_POWER_FAULT || id == UI_CHECK_POOL) &&
            (action == UI_ACT_ADD || action == UI_ACT_DELETE)) {
        uint32_t bit = 1u << id;
        g_ui.fault_add &= ~bit;
        g_ui.fault_delete &= ~bit;
        if (action == UI_ACT_ADD)
            g_ui.fault_add |= bit;
        else
            g_ui.fault_delete |= bit;
        pthread_mutex_unlock(&g_ui.qlock);
        return;
    }
    if (id == UI_NIGHT && (action == UI_ACT_ADD || action == UI_ACT_DELETE)) {
        g_ui.pending_night  = action == UI_ACT_ADD;
        g_ui.night_position = g_ui.qcount;
        if (action == UI_ACT_DELETE)
            g_ui.cancel_night = 1;
        pthread_mutex_unlock(&g_ui.qlock);
        return;
    }
    if (g_ui.qcount < UI_QUEUE_MAX) {
        g_ui.queue[g_ui.qcount].action  = action;
        g_ui.queue[g_ui.qcount].id      = id;
        g_ui.queue[g_ui.qcount].request = 0;
        g_ui.qcount++;
    }
    pthread_mutex_unlock(&g_ui.qlock);
}

int ui_switch_regular(uint16_t id, uint16_t *settled)
{
    if (!settled)
        return -1;
    pthread_mutex_lock(&switch_lock);
    pthread_mutex_lock(&g_ui.qlock);
    if (!g_ui.started || !g_ui.running || g_ui.qcount == UI_QUEUE_MAX) {
        pthread_mutex_unlock(&g_ui.qlock);
        pthread_mutex_unlock(&switch_lock);
        return -1;
    }
    uint32_t request = ++g_ui.next_request;
    if (!request)
        request = ++g_ui.next_request;
    g_ui.queue[g_ui.qcount++] = (struct ui_trigger){ UI_ACT_SWITCH, id, request };
    pthread_mutex_unlock(&g_ui.qlock);
    int result = -1;
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        pthread_mutex_lock(&g_ui.qlock);
        if (g_ui.completed_request == request) {
            *settled = g_ui.completed_show;
            result   = 0;
        }
        pthread_mutex_unlock(&g_ui.qlock);
        if (result == 0)
            break;
        usleep(5000);
    }
    pthread_mutex_unlock(&switch_lock);
    return result;
}

void ui_show_onoff(int onoff)
{
    pthread_mutex_lock(&g_ui.qlock);
    if (onoff) {
        g_ui.cancel_night  = 1;
        g_ui.pending_night = 0;
    }
    g_ui.pending_onoff = onoff ? 1 : 0;
    pthread_mutex_unlock(&g_ui.qlock);
}

void ui_show_wallpaper_reload(void)
{
    pthread_mutex_lock(&g_ui.qlock);
    g_ui.pending_wp_reload = 1;
    pthread_mutex_unlock(&g_ui.qlock);
}

void ui_set_brightness(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    pthread_mutex_lock(&g_ui.qlock);
    g_ui.brightness       = pct;
    g_ui.brightness_dirty = 1;
    pthread_mutex_unlock(&g_ui.qlock);
}

uint16_t ui_get_regular(void)
{
    uint16_t r;
    pthread_mutex_lock(&g_ui.qlock);
    r = g_ui.regular;
    pthread_mutex_unlock(&g_ui.qlock);
    return r;
}

void ui_seed_regular(uint16_t id)
{
    pthread_mutex_lock(&g_ui.qlock);
    g_ui.regular = id;
    pthread_mutex_unlock(&g_ui.qlock);
}
