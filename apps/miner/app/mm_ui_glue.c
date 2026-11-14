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
#include "ui_types.h"
#include "mm_display.h"
#include "mm_factory.h"
#include "mm_led.h"
#include "mm_telem.h"
#include "btn_decode.h"
#include "btn_input.h"
#include "beep_dev.h"
#include "fan_dev.h"
#include "cali.h"
#include "mm_internal.h"
#include "osif.h"

#include "mm_net.h"
#include "wifi_mgr.h"
#include "net_state.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BTN_TICK_MS       50u   /* decode-thread period: deferred short presses, beep end, ms */
#define BEEP_MS           50u   /* key-press beep length, ms, rounded up to a BTN_TICK_MS tick */
#define BTN_EV_QUEUE_LEN  8     /* decoded events awaiting the main thread; overflow dropped */
#define PROVISION_WAKE_MS 1500u /* WiFi without SSID: delay before waking a dark LED, ms */

static struct {
    btn_decoder     dec;          /* btn_input thread and decode thread, under dec_lock */
    pthread_mutex_t dec_lock;     /* guards dec, beep_off_at, ev_q/ev_head/ev_n; ev_cond mutex */
    pthread_t       tick_thread;  /* BTN_TICK_MS decode thread, joined by mm_ui_stop */
    _Atomic int     tick_run;     /* set/cleared by the main thread; decode thread exits on 0 */
    int             started;      /* 1 from mm_ui_start to mm_ui_stop, written by main thread */
    int             poweroff_req; /* power key press seen: mm_ui_stop powers off, main thread */
    uint32_t        beep_off_at;  /* (uint32_t)osif_now_ms() to stop the beep, ms; 0 = none */
    btn_event       ev_q[BTN_EV_QUEUE_LEN]; /* ring of decoded events, under dec_lock */
    pthread_cond_t  ev_cond;                /* CLOCK_MONOTONIC; signalled once per queued event */
    unsigned        ev_head;           /* index of the oldest queued event, 0..BTN_EV_QUEUE_LEN-1 */
    unsigned        ev_n;              /* queued events, 0..BTN_EV_QUEUE_LEN */
    int             net_present;       /* last mm_net_present(): 1/0, -1 before the first tick */
    int             net_last;          /* last NET_STATE_* acted on; -1 forces a re-evaluation */
    uint64_t        provision_wake_at; /* osif_now_ms() deadline to wake a dark LED, ms; 0 = off */
    int             btn_cfg_net;       /* 1 from a TP1 press until WiFi is up with an SSID */
} g = {
    .dec_lock    = PTHREAD_MUTEX_INITIALIZER,
    .net_present = -1,
    .net_last    = -1,
};

static void start_provisioning(void)
{
    int wired = (mm_net_devtype() == MM_NET_DEV_WIRED);
    if (wired)
        mm_led_wake();
    g.btn_cfg_net = 1;
    if (!wired)
        wifi_action_set(WIFI_ACT_PROV);
}

static void provision_wake_tick(void)
{
    char    ssid[WIFI_SSID_LEN_MAX] = { 0 };
    int     rssi                    = 0;
    uint8_t mode, effect;
    mm_led_state(&mode, &effect);
    if (mm_net_devtype() != MM_NET_DEV_WIFI || !mode) {
        g.provision_wake_at = 0;
        return;
    }
    wifi_update_ssid_info(ssid, &rssi);
    if (ssid[0]) {
        g.provision_wake_at = 0;
        return;
    }
    uint64_t now = osif_now_ms();
    if (!g.provision_wake_at) {
        if (g.net_last == NET_STATE_CONNECTING)
            ui_show_update(UI_ACT_ADD, UI_CONFIG_NETWORK);
        g.provision_wake_at = now + PROVISION_WAKE_MS;
    } else if (now >= g.provision_wake_at) {
        g.provision_wake_at = 0;
        mm_led_wake();
    }
}

static void handle_btn_event(btn_event ev)
{
    switch (ev) {
    case BTN_EV_TP2_SHORT:
        if (mm_led_wake()) {
            osif_log(OSIF_LOG_INFO, "TP2 button: LED woken");
            break;
        }
        osif_log(OSIF_LOG_INFO, "TP2 button: next screen");
        mm_display_cycle();
        break;
    case BTN_EV_TP1_SHORT:
        osif_log(OSIF_LOG_INFO, "TP1 button: starting provisioning");
        start_provisioning();
        break;
    case BTN_EV_POWER:
        osif_log(OSIF_LOG_INFO, "TP2 button held: shutting down");
        g.poweroff_req = 1;
        kill(getpid(), SIGTERM);
        break;
    case BTN_EV_TP2_DCLICK: {
        int next = mm_workmode_button_cycle();
        if (next >= 0)
            osif_log(OSIF_LOG_INFO, "TP2 button double click: switching to work mode %d", next);
        else
            osif_log(OSIF_LOG_INFO, "TP2 button double click: work mode unchanged");
        break;
    }
    case BTN_EV_TP1_LLONG:
        osif_log(OSIF_LOG_INFO, "TP1 button held: factory recovery");
        mm_factory_recover();
        break;
    default:
        osif_log(OSIF_LOG_INFO, "Button event %d: no action", (int)ev);
        break;
    }
}

static void btn_event_post(btn_event ev)
{
    if (ev == BTN_EV_NONE)
        return;
    if (g.ev_n == BTN_EV_QUEUE_LEN) {
        osif_log(OSIF_LOG_WARN, "Button press %d ignored: too many presses queued", (int)ev);
        return;
    }
    g.ev_q[(g.ev_head + g.ev_n++) % BTN_EV_QUEUE_LEN] = ev;
    pthread_cond_signal(&g.ev_cond);
}

static void btn_ev_cond_init(void)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g.ev_cond, &attr);
    pthread_condattr_destroy(&attr);
}

int mm_ui_btn_wait(uint32_t ms)
{
    struct timespec ts;
    int             pending;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000u;
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g.dec_lock);
    while (!g.ev_n && pthread_cond_timedwait(&g.ev_cond, &g.dec_lock, &ts) != ETIMEDOUT)
        ;
    pending = g.ev_n != 0;
    pthread_mutex_unlock(&g.dec_lock);
    return pending;
}

void mm_ui_btn_drain(void)
{
    btn_event evs[BTN_EV_QUEUE_LEN];
    unsigned  n;

    pthread_mutex_lock(&g.dec_lock);
    n = g.ev_n;
    for (unsigned i = 0; i < n; i++)
        evs[i] = g.ev_q[(g.ev_head + i) % BTN_EV_QUEUE_LEN];
    g.ev_head = (g.ev_head + n) % BTN_EV_QUEUE_LEN;
    g.ev_n    = 0;
    pthread_mutex_unlock(&g.dec_lock);
    for (unsigned i = 0; i < n; i++)
        handle_btn_event(evs[i]);
}

static void btn_cb(int code, int edge, uint32_t t_ms, void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g.dec_lock);
    btn_event_post(btn_decode_key(&g.dec, code, edge, t_ms));
    if (edge) {
        beep_play(1);
        g.beep_off_at = t_ms + BEEP_MS;
    }
    pthread_mutex_unlock(&g.dec_lock);
}

static void *btn_tick_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g.tick_run)) {
        uint32_t now = (uint32_t)osif_now_ms();
        pthread_mutex_lock(&g.dec_lock);
        btn_event_post(btn_decode_tick(&g.dec, now));
        if (g.beep_off_at && (int32_t)(now - g.beep_off_at) >= 0) {
            beep_play(0);
            g.beep_off_at = 0;
        }
        pthread_mutex_unlock(&g.dec_lock);
        osif_sleep_ms(BTN_TICK_MS);
    }
    return NULL;
}

static int summary_cb(ui_summary *out, void *arg)
{
    (void)arg;
    memset(out, 0, sizeof(*out));
    if (net_state_ip(out->ip, sizeof(out->ip)) != 0)
        strncpy(out->ip, "--", sizeof(out->ip) - 1);
    mm_telem_t t;
    mm_telem_get(&t);
    if (t.poll_fresh) {
        static const char *const modes[CALI_MODE_MAX] = { "Low", "Mid", "High" };
        double                   ghs                  = t.ghs_spd;
        snprintf(out->hashrate, sizeof(out->hashrate), "%.2f", ghs / 1000.0);
        snprintf(out->level, sizeof(out->level), "%s",
                (t.workmode >= 0 && t.workmode < CALI_MODE_MAX) ? modes[t.workmode] : "--");
        out->ghs = ghs;
        out->ready |= UI_RDY_HASH;
        if (t.pout_w == MM_TELEM_POUT_NONE) {
            strncpy(out->power, "--", sizeof(out->power) - 1);
        } else {
            snprintf(out->power, sizeof(out->power), "%d", t.pout_w);
            out->watt = t.pout_w;
            out->ready |= UI_RDY_POWER;
        }
    } else {
        strncpy(out->hashrate, "--", sizeof(out->hashrate) - 1);
        strncpy(out->level, "--", sizeof(out->level) - 1);
        strncpy(out->power, "--", sizeof(out->power) - 1);
    }
    return 0;
}

void mm_ui_start(void)
{
    if (g.started)
        return;

    btn_ev_cond_init();
    btn_decode_init(&g.dec);
    beep_init();
    mm_display_start();
    if (ui_start(summary_cb, NULL) != 0)
        osif_log(OSIF_LOG_ERR, "Screen unavailable: UI thread start failed");
    if (btn_input_start(btn_cb, NULL) != 0) {
        if (CAP_BUTTON)
            osif_log(OSIF_LOG_ERR, "Buttons unavailable: input start failed");
    } else {
        atomic_store(&g.tick_run, 1);
        if (pthread_create(&g.tick_thread, NULL, btn_tick_thread, NULL) != 0) {
            atomic_store(&g.tick_run, 0);
            btn_input_stop();
            beep_play(0);
            g.beep_off_at = 0;
            osif_log(OSIF_LOG_ERR, "Buttons unavailable: decode thread start failed");
        }
    }
    g.started = 1;
}

void mm_ui_wallpaper_reload(void)
{
    if (!g.started)
        return;
    ui_show_wallpaper_reload();
}

void mm_ui_night_mode(int dark)
{
    if (!g.started)
        return;
    mm_display_night(dark);
}

static void net_notice_tick(void)
{
    int st = net_state_get();
    if (st != g.net_last) {
        switch (st) {
        case NET_STATE_DOWN:
            if (!mm_net_wired_present() && !wifi_has_ssid()) {
                ui_show_update(UI_ACT_ADD, UI_CONFIG_NETWORK);
                ui_show_update(UI_ACT_DELETE, UI_LOST_CONNECT);
            } else {
                ui_show_update(UI_ACT_ADD, UI_LOST_CONNECT);
                ui_show_update(UI_ACT_DELETE, UI_CONFIG_NETWORK);
            }
            break;
        case NET_STATE_UP: {
            char ssid[WIFI_SSID_LEN_MAX] = { 0 };
            int  rssi                    = 0;
            ui_show_update(UI_ACT_DELETE, UI_CONFIG_NETWORK);
            ui_show_update(UI_ACT_DELETE, UI_LOST_CONNECT);
            if (mm_net_devtype() == MM_NET_DEV_WIFI)
                wifi_update_ssid_info(ssid, &rssi);
            if (ssid[0] && g.btn_cfg_net) {
                g.btn_cfg_net = 0;
                mm_display_lcd(MM_LCD_SHOW, UI_SUMMARY);
            }
            break;
        }
        default:
            if (g.net_last == NET_STATE_UP)
                ui_show_update(UI_ACT_ADD, UI_LOST_CONNECT);
            break;
        }
        g.net_last = st;
    }
}

void mm_ui_tick(void)
{
    mm_ui_btn_drain();

    int present = mm_net_present();
    if (present != g.net_present) {
        if (present) {
            ui_show_update(UI_ACT_DELETE, UI_USB_DEVICE);
        } else {
            ui_show_update(UI_ACT_ADD, UI_USB_DEVICE);
            ui_show_update(UI_ACT_DELETE, UI_CONFIG_NETWORK);
            ui_show_update(UI_ACT_DELETE, UI_LOST_CONNECT);
        }
        g.net_present       = present;
        g.net_last          = -1;
        g.provision_wake_at = 0;
    }

    if (present) {
        net_notice_tick();
        provision_wake_tick();
    }

    mm_display_tick();
}

int mm_ui_poweroff_requested(void)
{
    return g.poweroff_req;
}

void mm_ui_stop(void)
{
    if (!g.started)
        return;
    if (atomic_load(&g.tick_run)) {
        atomic_store(&g.tick_run, 0);
        pthread_join(g.tick_thread, NULL);
    }
    btn_input_stop();
    beep_close();
    ui_stop();
    g.started = 0;

    if (g.poweroff_req) {
        if (!mm_rail_off_unconfirmed())
            fan_dev_stop();
        (void)mm_system_poweroff();
    }
}
