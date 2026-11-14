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

#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int simulated_clock(clockid_t clock_id, struct timespec *value);
static int simulated_sleep(useconds_t delay);
#define clock_gettime simulated_clock
#define usleep        simulated_sleep /* ui.c sleeps via simulated_sleep, which advances elapsed */
#include "../ui/ui.c"
#undef clock_gettime
#undef usleep

/* Backlight stub state: on flag and brightness, %; brightness is 0 while off */
extern int physical_on, physical_brightness;

static uint32_t elapsed;      /* Simulated time since the run started, ms */
static uint16_t rendered[16]; /* Screen ids passed to ui_screen_render(), in call order */
/* render_count: entries in rendered[]; cancel_night: non-zero turns the display on at 1000 ms */
static int render_count, cancel_night;
/* Non-zero makes simulated_sleep() drain switch_dispatch on every sleep */
static int service_switch;
/* Dispatcher drained by simulated_sleep() when service_switch is set */
static ui_dispatch switch_dispatch;
static int         screen_init_result;
static int         fail_allocation; /* Non-zero makes calloc() return NULL */
void              *__real_calloc(size_t count, size_t size);
void              *__wrap_calloc(size_t count, size_t size)
{
    return fail_allocation ? NULL : __real_calloc(count, size);
}

int ui_screen_init(void)
{
    return screen_init_result;
}
void ui_screen_deinit(void)
{
}
void ui_screen_render(uint16_t id)
{
    assert(render_count < 16);
    rendered[render_count++] = id;
}
void ui_screen_refresh(const ui_summary *summary)
{
    (void)summary;
}
void ui_screen_reload_wallpaper(void)
{
}

static int simulated_clock(clockid_t clock_id, struct timespec *value)
{
    (void)clock_id;
    value->tv_sec  = elapsed / 1000;
    value->tv_nsec = (elapsed % 1000) * 1000000;
    return 0;
}

static int simulated_sleep(useconds_t delay)
{
    elapsed += delay / 1000;
    if (service_switch)
        drain_triggers(&switch_dispatch);
    if (cancel_night && elapsed == 1000)
        ui_show_onoff(1);
    if (elapsed >= 3500)
        g_ui.running = 0;
    return 0;
}

static void run_night(int cancel)
{
    elapsed      = 0;
    render_count = 0;
    cancel_night = cancel;
    g_ui.running = 1;
    ui_show_onoff(1);
    ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_ADD, UI_NIGHT);
    ui_thread(NULL);
    assert(render_count == 2);
    assert(rendered[0] == UI_NIGHT);
    assert(rendered[1] == (cancel ? UI_TIME : UI_OFF));
    assert(ui_get_regular() == UI_TIME);
}

int main(void)
{
    ui_dispatch dark;
    ui_dispatch_init(&dark, UI_SUMMARY);
    ui_show_onoff(0);
    ui_set_brightness(100);
    assert(physical_on == 0);
    drain_triggers(&dark);
    ui_set_brightness(37);
    drain_triggers(&dark);
    assert(physical_on == 0 && physical_brightness == 0);
    ui_show_onoff(1);
    drain_triggers(&dark);
    assert(physical_on == 1 && physical_brightness == 37);
    ui_dispatch_free(&dark);
    ui_dispatch fault;
    ui_dispatch_init(&fault, UI_SUMMARY);
    for (int i = 0; i < UI_QUEUE_MAX; i++)
        ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_ADD, UI_FAN_ERR);
    drain_triggers(&fault);
    assert(fault.popup && fault.popup->show.id == UI_FAN_ERR);
    for (int i = 0; i < UI_QUEUE_MAX; i++)
        ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_DELETE, UI_FAN_ERR);
    drain_triggers(&fault);
    assert(fault.popup == NULL);
    for (int i = 0; i < UI_QUEUE_MAX; i++)
        ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_ADD, UI_USB_DEVICE);
    drain_triggers(&fault);
    assert(fault.popup && fault.popup->show.id == UI_USB_DEVICE);
    for (int i = 0; i < UI_QUEUE_MAX; i++)
        ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_DELETE, UI_USB_DEVICE);
    drain_triggers(&fault);
    assert(fault.popup == NULL);
    static const uint16_t latched[] = {
        UI_HASH_ERR,
        UI_FAN_STALL,
        UI_AGING_ERR,
        UI_POWER_FAULT,
        UI_CHECK_POOL,
    };
    for (unsigned i = 0; i < sizeof latched / sizeof latched[0]; i++) {
        for (int j = 0; j < UI_QUEUE_MAX; j++)
            ui_show_update(UI_ACT_SWITCH, UI_TIME);
        ui_show_update(UI_ACT_ADD, latched[i]);
        drain_triggers(&fault);
        assert(fault.popup && fault.popup->show.id == latched[i]);
        for (int j = 0; j < UI_QUEUE_MAX; j++)
            ui_show_update(UI_ACT_SWITCH, UI_TIME);
        ui_show_update(UI_ACT_DELETE, latched[i]);
        drain_triggers(&fault);
        assert(fault.popup == NULL);
    }
    ui_dispatch_free(&fault);
    run_night(0);
    run_night(1);
    elapsed      = 0;
    render_count = 0;
    cancel_night = 0;
    g_ui.running = 1;
    ui_show_onoff(1);
    for (int index = 0; index < UI_QUEUE_MAX; index++)
        ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_ADD, UI_NIGHT);
    ui_thread(NULL);
    assert(render_count == 2 && rendered[0] == UI_NIGHT && rendered[1] == UI_OFF);
    elapsed      = 0;
    render_count = 0;
    cancel_night = 0;
    g_ui.running = 1;
    ui_show_update(UI_ACT_SWITCH, UI_TIME);
    ui_show_update(UI_ACT_ADD, UI_NIGHT);
    ui_show_onoff(1);
    ui_thread(NULL);
    assert(render_count == 1 && rendered[0] == UI_TIME);
    uint16_t settled = 0;
    assert(ui_switch_regular(UI_TIME, &settled) < 0);
    g_ui.started   = 1;
    g_ui.running   = 1;
    elapsed        = 0;
    service_switch = 1;
    ui_dispatch_init(&switch_dispatch, UI_SUMMARY);
    fail_allocation = 1;
    ui_show_update(UI_ACT_ADD, UI_NIGHT);
    drain_triggers(&switch_dispatch);
    assert(switch_dispatch.onoff == 0);
    fail_allocation = 0;
    ui_show_onoff(1);
    drain_triggers(&switch_dispatch);
    ui_show_update(UI_ACT_ADD, UI_NIGHT);
    ui_show_update(UI_ACT_SWITCH, UI_WALLPAPER);
    drain_triggers(&switch_dispatch);
    assert(switch_dispatch.regular.id == UI_SUMMARY);
    ui_show_onoff(1);
    drain_triggers(&switch_dispatch);
    ui_show_add(&switch_dispatch, UI_NIGHT);
    ui_show_add(&switch_dispatch, UI_POWER_ERR);
    ui_show_onoff(1);
    drain_triggers(&switch_dispatch);
    ui_show_delete(&switch_dispatch, UI_POWER_ERR);
    assert(ui_pick_show(&switch_dispatch).id == UI_SUMMARY);
    assert(ui_switch_regular(UI_WALLPAPER, &settled) == 0 && settled == UI_WALLPAPER);
    ui_show_add(&switch_dispatch, UI_POWER_ERR);
    assert(ui_switch_regular(UI_TIME, &settled) == 0 && settled == UI_WALLPAPER);
    ui_dispatch_free(&switch_dispatch);
    service_switch     = 0;
    screen_init_result = -1;
    g_ui.started       = 1;
    g_ui.running       = 1;
    ui_thread(NULL);
    assert(g_ui.running == 0);
    elapsed = 0;
    assert(ui_switch_regular(UI_TIME, &settled) < 0 && elapsed == 0);
    g_ui.started = 0;
    puts("UI night transition and day cancellation OK");
    return 0;
}
