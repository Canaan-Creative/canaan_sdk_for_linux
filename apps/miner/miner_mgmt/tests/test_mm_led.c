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
#include "mm_led_cfg.h"
#include "led_fx.h"
#include "ws2812.h"
#include "ctrl_cmds.h"
#include "mm_cfgstore.h"
#include "osif.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "../../tests/include/tst_check.h"

#define mm_cfgstore_flush() __real_mm_cfgstore_flush() /* unwrapped, not in g_flush_calls */

static mm_led_cfg_t    g_saved;          /* LED config as last saved to the store */
static int             g_save_calls;     /* mm_cfgstore_set_n() call count */
static int             g_ws_init_rc;     /* w2812_init() result */
static int             g_ws_set_calls;   /* w2812_set() call count */
static int             g_ws_close_calls; /* w2812_close() call count */
static _Atomic int     g_tick_calls;     /* led_fx_tick() call count */
static int             g_flush_calls;    /* wrapped mm_cfgstore_flush() call count */
static uint64_t        g_now;            /* fake monotonic clock, ms */
static int             g_night;          /* last mm_ui_night_mode() dark flag */
static int             g_night_calls;    /* mm_ui_night_mode() call count */
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER; /* guards the save fields */
static pthread_cond_t  save_cond;                             /* signals save state changes */
/*
 * Flags: block a dark-mode save; a save is blocked; unblock it; set_day()
 * finished.
 */
static int             hold_save, save_entered, release_save, day_done;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER; /* guards log_text */
static char            log_text[4096];                       /* log lines, newline separated */
static size_t          log_len;                              /* bytes in log_text */

int __real_mm_cfgstore_flush(void);
int __real_mm_cfgstore_set_n(const char *const *sections, const char *const *keys,
        const char *const *values, size_t n);

uint64_t osif_now_ms(void)
{
    return g_now;
}
void mm_ui_night_mode(int dark)
{
    g_night = dark;
    g_night_calls++;
}

int __wrap_mm_cfgstore_set_n(const char *const *sections, const char *const *keys,
        const char *const *values, size_t n)
{
    pthread_mutex_lock(&save_lock);
    mm_led_cfg_t c = g_saved;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)strtoul(values[i], NULL, 10);
        if (!strcmp(keys[i], "ledmode"))
            c.ledmode = (uint8_t)v;
        else if (!strcmp(keys[i], "effect"))
            c.effect = (uint8_t)v;
        else if (!strcmp(keys[i], "bright"))
            c.bright = (uint8_t)v;
        else if (!strcmp(keys[i], "temper"))
            c.temper = v;
        else if (!strcmp(keys[i], "rgb"))
            c.rgb = v;
    }
    if (hold_save && c.ledmode == 1) {
        save_entered = 1;
        pthread_cond_broadcast(&save_cond);
        while (!release_save)
            pthread_cond_wait(&save_cond, &save_lock);
    }
    g_save_calls++;
    g_saved = c;
    pthread_mutex_unlock(&save_lock);
    return __real_mm_cfgstore_set_n(sections, keys, values, n);
}

int __wrap_mm_cfgstore_flush(void)
{
    g_flush_calls++;
    return __real_mm_cfgstore_flush();
}

static void *set_dark(void *unused)
{
    (void)unused;
    mm_led_apply(CTRL_OP_LEDMODE, "1");
    return NULL;
}

static void *set_day(void *unused)
{
    (void)unused;
    mm_led_wake();
    pthread_mutex_lock(&save_lock);
    day_done = 1;
    pthread_cond_broadcast(&save_cond);
    pthread_mutex_unlock(&save_lock);
    return NULL;
}

int w2812_init(void)
{
    return g_ws_init_rc;
}
int w2812_close(void)
{
    g_ws_close_calls++;
    return 0;
}
int w2812_set(uint32_t *led_rgb)
{
    (void)led_rgb;
    g_ws_set_calls++;
    return 0;
}

int led_fx_tick(led_state_t *st, uint8_t fx, uint32_t rgb, uint32_t out[LED_PIXELS])
{
    (void)st;
    (void)fx;
    (void)rgb;
    (void)out;
    g_tick_calls++;
    return 0;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    (void)lvl;
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vsnprintf(log_text + log_len, sizeof log_text - log_len, fmt, ap);
    va_end(ap);
    log_len += strlen(log_text + log_len);
    if (log_len < sizeof log_text - 1) {
        log_text[log_len++] = '\n';
        log_text[log_len]   = '\0';
    }
    pthread_mutex_unlock(&log_lock);
}

static void log_clear(void)
{
    pthread_mutex_lock(&log_lock);
    log_len     = 0;
    log_text[0] = '\0';
    pthread_mutex_unlock(&log_lock);
}

static int log_has(const char *needle)
{
    pthread_mutex_lock(&log_lock);
    int found = strstr(log_text, needle) != NULL;
    pthread_mutex_unlock(&log_lock);
    return found;
}

void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

static int wait_ticks(int n)
{
    struct timespec ts = { 0, 1000000 };

    for (int i = 0; i < 2000 && g_tick_calls < n; i++)
        nanosleep(&ts, NULL);
    return g_tick_calls >= n;
}

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "  FAIL: cannot write %s\n", path);
        fails++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

static int file_has(const char *path, const char *needle)
{
    char  buf[2048];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static void save(const mm_led_cfg_t *c)
{
    char    args[64];
    uint8_t mode, fx;

    mm_led_state(&mode, &fx);
    snprintf(args, sizeof args, "%u-%u-%u-%u-%u-%u", c->effect, c->bright, c->temper,
            (c->rgb >> 16) & 0xFFu, (c->rgb >> 8) & 0xFFu, c->rgb & 0xFFu);
    if ((mode && mm_led_apply(CTRL_OP_LEDMODE, "0") != MC_SET_OK) ||
            mm_led_apply(CTRL_OP_LEDSET, args) != MC_SET_OK ||
            (c->ledmode && mm_led_apply(CTRL_OP_LEDMODE, "1") != MC_SET_OK)) {
        fprintf(stderr, "  FAIL: cannot apply the light settings %s\n", args);
        fails++;
    }
}

static int save_flush(const mm_led_cfg_t *c)
{
    save(c);
    return mm_cfgstore_flush() < 0 ? -1 : 0;
}

static void load_store(mm_led_cfg_t *c)
{
    uint8_t fx;

    mm_led_reload();
    mm_led_snapshot(c, &fx);
}

static void load_file(const char *path, mm_led_cfg_t *c)
{
    if (mm_cfgstore_open(path) != 0) {
        fprintf(stderr, "  FAIL: cannot open the store on %s\n", path);
        fails++;
    }
    load_store(c);
}

static void *save_concurrently(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 1000; i++) {
        mm_led_cfg_t cfg = mm_led_cfg_default;
        cfg.bright       = i % 100;
        cfg.temper = cfg.rgb = cfg.bright;
        save(&cfg);
    }
    return NULL;
}

static void store_tests(const char *path)
{
    mm_led_cfg_t c = mm_led_cfg_default;
    log_clear();
    save(&c);
    CHECK(log_has("light-strip settings NOT persisted"),
            "a save refused by an unopened store is logged");
    load_file(path, &c);
    CHECK(c.ledmode == 0 && c.effect == LED_FX_CONTINUE && c.bright == 100 && c.temper == 50 &&
                    c.rgb == 255,
            "load missing file keeps defaults");

    c.ledmode = 1;
    c.effect  = LED_FX_BREATH;
    c.bright  = 42;
    c.temper  = 1234;
    c.rgb     = 0x00FF80u;
    save(&c);
    CHECK(access(path, F_OK) != 0, "save changes RAM only");
    CHECK(mm_cfgstore_flush() == 1, "flush writes the save");
    mm_led_cfg_t r = mm_led_cfg_default;
    load_file(path, &r);
    CHECK(r.ledmode == 1, "ledmode round-trips");
    CHECK(r.effect == LED_FX_BREATH, "effect round-trips");
    CHECK(r.bright == 42, "bright round-trips");
    CHECK(r.temper == 1234, "temper round-trips");
    CHECK(r.rgb == 0x00FF80u, "rgb round-trips");

    CHECK(file_has(path, "[syscfg]") && file_has(path, "ledmode"), "ledmode lands in [syscfg]");
    CHECK(file_has(path, "[led]") && file_has(path, "effect"), "led keys land in [led]");
    CHECK(file_has(path, "65408"), "rgb is written in decimal");

    c.ledmode = 0;
    c.effect  = LED_FX_COLORCYCLE;
    c.bright  = 80;
    c.temper  = 50;
    c.rgb     = LED_RGB_WHITE;
    CHECK(save_flush(&c) == 0, "re-save ok");
    memset(&r, 0xAA, sizeof r);
    load_file(path, &r);
    CHECK(r.ledmode == 0 && r.effect == LED_FX_COLORCYCLE && r.bright == 80 && r.temper == 50 &&
                    r.rgb == LED_RGB_WHITE,
            "re-save overwrites");

    write_file(path,
            "[syscfg]\nledmode = 1\nlanguage = 2\n"
            "[led]\neffect = 2\nrgb = 255\n[wifi]\nssid = home\n");
    load_file(path, &r);
    CHECK(r.ledmode == 1 && r.effect == LED_FX_BLINK && r.rgb == 255,
            "load reads the saved file shape");
    r.effect = LED_FX_CONTINUE;
    CHECK(save_flush(&r) == 0, "save over a populated file ok");
    CHECK(file_has(path, "language") && file_has(path, "ssid"),
            "foreign keys survive a light save");

    write_file(path,
            "[syscfg]\nledmode = 7\n"
            "[led]\neffect = 200\nbright = 9999\ntemper = 70000\n"
            "rgb = 4294967295\n");
    r = mm_led_cfg_default;
    load_file(path, &r);
    CHECK(r.ledmode == 1, "ledmode 7 clamps to 1 (dark)");
    CHECK(r.effect == LED_FX_USER_MAX, "effect 200 clamps to the last user effect");
    CHECK(r.bright == 100, "bright 9999 clamps to 100");
    CHECK(r.temper == 0xFFFFu, "temper 70000 clamps to 65535");
    CHECK(r.rgb == LED_RGB_MAX, "rgb 0xFFFFFFFF clamps to 0xFFFFFF");

    write_file(path,
            "[syscfg]\nledmode = -1\n"
            "[led]\neffect = abc\nbright = 010\ntemper = 7x\n"
            "rgb = 0xFF6100\n");
    r = mm_led_cfg_default;
    log_clear();
    load_file(path, &r);
    CHECK(r.bright == 10, "010 is read as decimal 10, not octal");
    CHECK(r.ledmode == 0 && r.effect == LED_FX_CONTINUE && r.temper == 50,
            "non-numeric or signed values keep the default");
    CHECK(r.rgb == 255, "hex rgb is not the decimal the writer emits: default kept");
    CHECK(log_has("not a valid number"), "a rejected value is logged");

    write_file(path,
            "[led]\nbright = 42\ntemper = 99999999999999999999999\n"
            "rgb = 184467440737095516160\n");
    r = mm_led_cfg_default;
    log_clear();
    load_file(path, &r);
    CHECK(r.bright == 42, "an in-range value beside overflowing ones is read");
    CHECK(r.temper == 50 && r.rgb == 255, "a value that overflows unsigned long keeps the default");
    CHECK(log_has("not a valid number"), "an overflowing value is logged");

    mm_led_cfg_t d = mm_led_cfg_default;
    d.rgb          = 0x010203u;
    save(&d);
    CHECK(mm_cfgstore_reload() == 0, "reload drops the unflushed save");
    r = mm_led_cfg_default;
    load_store(&r);
    CHECK(r.rgb == 255, "a dropped save never reaches the file");

    d     = mm_led_cfg_default;
    d.rgb = 0x040506u;
    mm_cfgstore_freeze(1);
    log_clear();
    save(&d);
    CHECK(!log_has("NOT persisted"), "a save to a frozen store is dropped without a log");
    CHECK(mm_cfgstore_flush() < 0, "a frozen store refuses the flush");
    mm_cfgstore_freeze(0);
    CHECK(mm_cfgstore_flush() == 0, "nothing of the dropped save is pending");
    r = mm_led_cfg_default;
    load_file(path, &r);
    CHECK(r.rgb == 255, "save during factory recovery never reaches the file");

    CHECK(mm_cfgstore_open("/proc/nonexistent/dir/x.ini") == 0,
            "store opens over an unwritable directory");
    save(&r);
    CHECK(mm_cfgstore_flush() < 0, "flush to an unwritable path fails");
    CHECK(mm_cfgstore_flush() < 0, "the failed save stays pending for retry");

    CHECK(mm_cfgstore_open(path) == 0, "store reopens on the test file");
    pthread_t writer;
    CHECK(pthread_create(&writer, NULL, save_concurrently, NULL) == 0,
            "start concurrent settings writer");
    for (unsigned i = 0; i < 50; i++)
        CHECK(mm_cfgstore_flush() >= 0, "concurrent flush succeeds");
    pthread_join(writer, NULL);
    CHECK(mm_cfgstore_flush() >= 0, "flush latest concurrent settings");
    load_file(path, &r);
    CHECK(r.bright == 99 && r.temper == 99 && r.rgb == 99,
            "last complete selection survives concurrent flushes");
}

int main(void)
{
    uint8_t            mode, fx;
    char               path[] = "/tmp/mm_led_testXXXXXX";
    pthread_condattr_t save_ca;

    pthread_condattr_init(&save_ca);
    pthread_condattr_setclock(&save_ca, CLOCK_MONOTONIC);
    pthread_cond_init(&save_cond, &save_ca);
    pthread_condattr_destroy(&save_ca);
    int tfd = mkstemp(path);
    if (tfd < 0) {
        fprintf(stderr, "  FAIL: cannot create a temporary file\n");
        return 1;
    }
    close(tfd);
    unlink(path);

    g_ws_init_rc = -1;
    store_tests(path);

    write_file(path,
            "[syscfg]\nledmode = 0\n"
            "[led]\neffect = 3\nbright = 7\ntemper = 11\nrgb = 65408\n");
    CHECK(mm_cfgstore_open(path) == 0, "store opens on the boot settings");
    mm_led_start();
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_BREATH, "boot restores the persisted day mode and effect");
    CHECK(g_ws_set_calls == 0 && g_tick_calls == 0,
            "a failed device open starts no thread and writes no frame");

    int n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "1") == MC_SET_OK, "ledmode 1 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_DARK && g_save_calls == n0 + 1 && g_saved.ledmode == 1,
            "CTRL_OP_LEDMODE reaches apply_ledmode: dark drives DARK and persists");

    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "0") == MC_SET_OK, "ledmode 0 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_BREATH,
            "dark -> day restores the persisted effect rather than leaving DARK");

    int nights = g_night_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "0") == MC_SET_OK && g_night_calls == nights + 1 &&
                    g_night == 0,
            "ledmode 0 in day mode still turns the LCD on (%d call(s))", g_night_calls - nights);
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "1") == MC_SET_OK && g_night_calls == nights + 2 &&
                    g_night == 1,
            "...day -> dark reaches the LCD");
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "0") == MC_SET_OK && g_night_calls == nights + 3 &&
                    g_night == 0,
            "...and so does dark -> day");

    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDSET, "1-7-11-0-255-128") == MC_SET_OK,
            "ledset 1-7-11-0-255-128 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_CONTINUE && g_save_calls == n0 + 1 &&
                    g_saved.effect == LED_FX_CONTINUE && g_saved.ledmode == 0,
            "CTRL_OP_LEDSET reaches apply_ledset: effect live + persisted, mode untouched");

    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "1-60") == MC_SET_ERR_ARG,
            "mm rejects a stale nightlamp-open request after switching to day");
    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "0-0") == MC_SET_ERR_ARG,
            "mm rejects a stale nightlamp-close request in day mode");
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_CONTINUE && g_save_calls == n0,
            "rejected nightlamp requests leave live state and persistence unchanged");
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "1") == MC_SET_OK,
            "enter dark before valid nightlamp requests");
    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "1-180") == MC_SET_OK, "nightlamp 1-180 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_EXTEND && g_save_calls == n0,
            "CTRL_OP_NIGHTLAMP reaches apply_nightlamp: EXTEND live, nothing persisted");

    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "0-180") == MC_SET_OK, "nightlamp 0-180 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_SHRINK && g_save_calls == n0,
            "nightlamp close drives SHRINK, still without persisting");

    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_WORKMODE, "1") == MC_SET_ERR_UNSUP,
            "a real op that is not the strip's is unsupported here");
    CHECK(mm_led_apply(CTRL_OP_NONE, "1") == MC_SET_ERR_UNSUP, "op 0 has no arm");
    CHECK(mm_led_apply((uint16_t)CTRL_OP_COUNT, "1") == MC_SET_ERR_UNSUP,
            "an op past the end of the table has no arm");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_SHRINK && g_save_calls == n0,
            "an unsupported op actuates nothing and persists nothing");

    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "1") == MC_SET_OK, "ledmode 1 is accepted");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_DARK, "dark mode drives DARK");
    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDSET, "4-42-1234-1-2-3") == MC_SET_ERR_ARG,
            "a well-formed ledset is refused outside day mode");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_DARK && g_save_calls == n0,
            "the refused ledset neither actuates nor persists");

    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "1-60") == MC_SET_OK,
            "nightlamp is accepted when mm confirms the current dark mode");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_EXTEND,
            "the lamp overrides dark's effect without clearing the persisted mode");

    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "0") == MC_SET_OK, "ledmode 0 is accepted");
    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDSET, "4-42-1234-1-2-3") == MC_SET_OK,
            "the same ledset is accepted in day mode");
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_COLORCYCLE, "ledset drives the requested effect live");
    CHECK(g_save_calls == n0 + 1 && g_saved.effect == LED_FX_COLORCYCLE && g_saved.bright == 42 &&
                    g_saved.temper == 1234 && g_saved.rgb == 0x010203u,
            "effect/bright/temper/rgb all persist, rgb packed r<<16|g<<8|b");
    CHECK(g_saved.ledmode == 0, "the persisted settings keep day mode");

    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "2") == MC_SET_ERR_ARG, "ledmode 2 is out of range");
    CHECK(mm_led_apply(CTRL_OP_LEDSET, "6-100-100-0-0-255") == MC_SET_ERR_ARG,
            "ledset cannot select a night-lamp effect");
    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "1-99999") == MC_SET_ERR_ARG,
            "nightlamp duration is bounded");
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_COLORCYCLE && g_save_calls == n0,
            "the three refusals leave mode, live effect and the file alone");

    n0 = g_save_calls;
    CHECK(mm_led_apply(CTRL_OP_LEDSET, "2-42-1234-1-2-3") == MC_SET_OK,
            "ledset succeeds once the setting is queued");
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_BLINK && g_save_calls == n0 + 1 && g_saved.effect == LED_FX_BLINK,
            "...applied live and queued exactly once");
    CHECK(mm_led_apply(CTRL_OP_LEDMODE, "1") == MC_SET_OK,
            "ledmode succeeds once the setting is queued");
    mm_led_state(&mode, &fx);
    CHECK(mode == 1 && fx == LED_FX_DARK && g_save_calls == n0 + 2 && g_saved.ledmode == 1,
            "...applied live and queued exactly once");
    CHECK(g_night == 1, "ledmode dark reaches the LCD night transition");
    CHECK(mm_cfgstore_flush() == 1,
            "a save only stages the setting; the file is written by mm_cfgstore_flush");
    mm_led_cfg_t live;
    mm_led_snapshot(&live, &fx);
    CHECK(live.effect == LED_FX_BLINK && live.bright == 42 && live.rgb == 0x010203u,
            "snapshot reports the applied settings before any flush");

    g_now = 1000;
    mm_led_apply(CTRL_OP_NIGHTLAMP, "1-2");
    g_now = 2999;
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_EXTEND, "nightlamp stays open before deadline");
    g_now = 4000;
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_SHRINK, "elapsed clock closes lamp without counting frames");
    mm_led_apply(CTRL_OP_NIGHTLAMP, "1-2");
    CHECK(mm_led_wake() == 1, "night button consumes wake event");
    g_now = 10000;
    mm_led_state(&mode, &fx);
    CHECK(mode == 0 && fx == LED_FX_BLINK && g_night == 0,
            "wake restores user effect and cancels nightlamp deadline");
    CHECK(mm_led_wake() == 0, "day button remains available for page switching");

    g_now = 20000;
    mm_led_apply(CTRL_OP_LEDMODE, "1");
    CHECK(mm_led_apply(CTRL_OP_NIGHTLAMP, "1-0") == MC_SET_OK, "nightlamp 1-0 is accepted");
    g_now = 20000 + 179999;
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_EXTEND, "duration 0 opens the lamp for the 180 s default");
    g_now = 20000 + 180000;
    mm_led_state(&mode, &fx);
    CHECK(fx == LED_FX_SHRINK, "...and closes it at 180 s");
    mm_led_apply(CTRL_OP_LEDMODE, "0");

    pthread_t dark_thread, day_thread;
    hold_save = 1;
    CHECK(pthread_create(&dark_thread, NULL, set_dark, NULL) == 0, "start dark operation");
    pthread_mutex_lock(&save_lock);
    while (!save_entered)
        pthread_cond_wait(&save_cond, &save_lock);
    pthread_mutex_unlock(&save_lock);
    CHECK(pthread_create(&day_thread, NULL, set_day, NULL) == 0, "start concurrent wake");
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec++;
    pthread_mutex_lock(&save_lock);
    while (!day_done) {
        if (pthread_cond_timedwait(&save_cond, &save_lock, &deadline) != 0)
            break;
    }
    CHECK(!day_done, "wake cannot overtake an unfinished mode save");
    release_save = 1;
    pthread_cond_broadcast(&save_cond);
    pthread_mutex_unlock(&save_lock);
    pthread_join(dark_thread, NULL);
    pthread_join(day_thread, NULL);
    mm_led_snapshot(&live, &fx);
    CHECK(live.ledmode == 0 && g_saved.ledmode == 0 && g_night == 0,
            "concurrent wake leaves runtime, LCD and persisted mode consistent");

    mm_led_stop();
    CHECK(g_ws_set_calls == 0 && g_ws_close_calls == 1 && g_tick_calls == 0,
            "stop calls w2812_close even when the open failed");

    g_ws_init_rc = 0;
    int closes   = g_ws_close_calls;
    mm_led_start();
    CHECK(wait_ticks(1), "a successful device open starts the effect thread");
    CHECK(g_ws_set_calls == 1, "thread start writes one initial frame before the first tick");
    mm_led_stop();
    int ticks = g_tick_calls;
    CHECK(g_ws_close_calls == closes + 1, "stop joins the effect thread and closes the device");
    mm_led_stop();
    CHECK(g_ws_close_calls == closes + 2 && g_tick_calls == ticks,
            "a second stop returns without a join and still closes the device");
    mm_led_start();
    CHECK(wait_ticks(ticks + 1), "a restart after stop runs the effect thread again");
    mm_led_stop();
    ticks = g_tick_calls;
    mm_led_start();
    CHECK(wait_ticks(ticks + 1), "a second start/stop round trip runs the thread");
    mm_led_stop();
    CHECK(g_ws_close_calls == closes + 4 && g_ws_set_calls == 3,
            "each stop closes the device once; each start wrote one initial frame (%d)",
            g_ws_set_calls);
    CHECK(g_flush_calls == 0, "no LED path calls mm_cfgstore_flush");

    unlink(path);
    char lockpath[sizeof path + 5];
    snprintf(lockpath, sizeof lockpath, "%s.lock", path);
    unlink(lockpath);
    return tst_done();
}
