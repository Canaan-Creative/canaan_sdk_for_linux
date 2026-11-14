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

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include "boardconf.h"
#include "webauth.h"

#undef USERCONFIG_DIR
/* Overridden to a scratch directory in the working directory */
#define USERCONFIG_DIR "mm_ui_provision_data"
#undef MC_SYSTEMCFG_DEFAULT
#define MC_SYSTEMCFG_DEFAULT \
    USERCONFIG_DIR "/systemcfg.ini" /* Overridden to the scratch directory */
#undef MC_ACTIVATION_DEFAULT
#define MC_ACTIVATION_DEFAULT \
    USERCONFIG_DIR "/activation.ini" /* Overridden to the scratch directory */
#undef CALI_INI_PATH
#define CALI_INI_PATH USERCONFIG_DIR "/hashrate_cali.ini" /* Overridden to the scratch directory */
#undef FACTORY_DEFAULT_DIR
#define FACTORY_DEFAULT_DIR FACTORY_SEED_DIR /* Overridden to the board's files/usrcon */
#undef FACTORY_CALI_DIR
#define FACTORY_CALI_DIR \
    FACTORY_SEED_DIR "/../factory" /* Overridden to the board's files/factory */
#undef MM_EFUSE_FILE_PATH
#define MM_EFUSE_FILE_PATH \
    USERCONFIG_DIR "/efuse_info.txt" /* Overridden to the scratch directory */
#undef MM_CFGWR_WP_DIR
#define MM_CFGWR_WP_DIR USERCONFIG_DIR "/wallpaper" /* Overridden to the scratch directory */
#undef MC_WEB_TOKEN_KEY
#define MC_WEB_TOKEN_KEY \
    USERCONFIG_DIR "/miner-web.tokenkey" /* Overridden to the scratch directory */
#undef CAP_BUTTON
#define CAP_BUTTON tst_cap_button /* Overridden: button presence switchable per case */

static int tst_cap_button = 1; /* Value CAP_BUTTON takes: 1 the board has a button, 0 not */

int tst_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int tst_pthread_join(pthread_t th, void **ret);

#define pthread_create tst_pthread_create
#define pthread_join   tst_pthread_join
#include "../../app/mm_ui_glue.c"
#undef pthread_join
#undef pthread_create
#include "../../app/mm_factory.c"
#include "../../app/mm_cfgwr.c"

static uint64_t clock_ms; /* Simulated clock behind osif_now_ms(), ms */
/* Network type (MM_NET_DEV_*), network state (NET_STATE_*), LED dark flag */
static int network_type = 1, network_state, dark = 1;
/* Wired address net_state_ip() reports; empty = none */
static char wired_ip[INET_ADDRSTRLEN];
/* LED wakes, WiFi provisions, network-config popups, dongle present flag */
static int  wakes, provisions, config_shown, dongle = 1;
static int  has_ssid, wired_present;         /* wifi_has_ssid() and mm_net_wired_present() values */
static int  visible[UI_SHOW_MAX];            /* Per-screen visible flag, by UI id */
static char network_ssid[WIFI_SSID_LEN_MAX]; /* SSID wifi_update_ssid_info() reports */
/* Last mm_display_lcd() id and value, and its call count */
static int      tst_lcd_id, tst_lcd_value, tst_lcd_n;
static int      display_cycles;     /* mm_display_cycle() calls */
static int      reboots, poweroffs; /* mm_system_reboot() and mm_system_poweroff() calls */
static uint8_t  reboot_reason;      /* Reason of the last mm_system_reboot() */
static uint32_t reboot_sub;         /* Subreason of the last mm_system_reboot() */
static int      lost_shown;         /* Lost-connection popups shown */
/* btn_input_start() return value, its calls, btn_input_stop() calls */
static int input_start_rc, input_starts, input_stops;
/* tst_pthread_create() return value (0 = real create), its calls, join calls */
static int thread_create_rc, thread_creates, thread_joins;
static int beep_closes, ui_stops; /* beep_close() and ui_stop() calls */
static int fan_stops, fan_stop_poweroffs = -1;
static int rail_off_unconfirmed;
/* Beeper on flag, beep-on count, input_stops at the last beep-off (-1 none) */
static int           beep_on, beep_ons, beep_off_stops = -1;
static btn_input_cb  input_cb; /* Button callback btn_input_start() registered */
static osif_loglvl_t log_level;
static char          log_line[160];
static char          log_all[4096]; /* Log lines since the last log_all reset */

uint64_t osif_now_ms(void)
{
    return clock_ms;
}
void osif_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}
void osif_log(osif_loglvl_t level, const char *format, ...)
{
    va_list ap;

    log_level = level;
    va_start(ap, format);
    vsnprintf(log_line, sizeof log_line, format, ap);
    va_end(ap);
    size_t n = strlen(log_all);
    snprintf(log_all + n, sizeof log_all - n, "%s\n", log_line);
}
int tst_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    thread_creates++;
    if (thread_create_rc) {
        if (input_cb)
            input_cb(0, 1, (uint32_t)clock_ms, NULL);
        return thread_create_rc;
    }
    return pthread_create(th, attr, fn, arg);
}
int tst_pthread_join(pthread_t th, void **ret)
{
    thread_joins++;
    return pthread_join(th, ret);
}
int btn_input_start(btn_input_cb cb, void *arg)
{
    (void)arg;
    input_starts++;
    if (!input_start_rc)
        input_cb = cb;
    return input_start_rc;
}
void btn_input_stop(void)
{
    input_stops++;
    input_cb = NULL;
}
void btn_decode_init(btn_decoder *b)
{
    memset(b, 0, sizeof *b);
}
btn_event btn_decode_key(btn_decoder *b, int code, int edge, uint32_t t_ms)
{
    (void)b;
    (void)code;
    (void)edge;
    (void)t_ms;
    return BTN_EV_NONE;
}
btn_event btn_decode_tick(btn_decoder *b, uint32_t t_ms)
{
    (void)b;
    (void)t_ms;
    return BTN_EV_NONE;
}
int beep_init(void)
{
    return 0;
}
void beep_play(int en)
{
    beep_on = en;
    beep_ons += en;
    if (!en)
        beep_off_stops = input_stops;
}
void beep_close(void)
{
    beep_closes++;
}
int ui_start(ui_summary_cb cb, void *cb_arg)
{
    (void)cb;
    (void)cb_arg;
    return 0;
}
void ui_stop(void)
{
    ui_stops++;
}
void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
}
int mm_net_devtype(void)
{
    return network_type;
}
int mm_net_present(void)
{
    return dongle;
}
int mm_net_wired_present(void)
{
    return wired_present;
}
int wifi_has_ssid(void)
{
    return has_ssid;
}
int net_state_ip(char *ip, size_t n)
{
    if (!wired_ip[0])
        return -1;
    snprintf(ip, n, "%s", wired_ip);
    return 0;
}
int net_state_get(void)
{
    if (network_state == 2 || wired_ip[0])
        return 2;
    return network_state;
}
int wifi_get_state(char *ip, int length)
{
    (void)ip;
    (void)length;
    return network_state;
}
int wifi_update_ssid_info(char *ssid, int *rssi)
{
    strcpy(ssid, network_ssid);
    *rssi = 0;
    return 1;
}
void wifi_action_set(unsigned int action)
{
    assert(action == WIFI_ACT_PROV);
    provisions++;
}
void mm_led_state(uint8_t *mode, uint8_t *effect)
{
    *mode   = dark;
    *effect = 0;
}
int mm_led_wake(void)
{
    if (!dark)
        return 0;
    dark = 0;
    wakes++;
    return 1;
}
void mm_display_tick(void)
{
}
int mm_display_lcd(int id, int value)
{
    tst_lcd_id    = id;
    tst_lcd_value = value;
    tst_lcd_n++;
    return 0;
}
void mm_display_cycle(void)
{
    display_cycles++;
}
void mm_led_reload(void)
{
}
void mm_efuse_invalidate(void)
{
}
int mm_display_start(void)
{
    return 0;
}
void filtermesh_load(void)
{
}
uint32_t filtermesh_minutes(void)
{
    return 0;
}
int filtermesh_keep(void)
{
    return 0;
}
mc_set_result mm_tz_apply_default(const char *name)
{
    (void)name;
    return MC_SET_OK;
}
void wifi_cfg_freeze(int on)
{
    (void)on;
}
int mm_workmode_button_cycle(void)
{
    return -1;
}
mc_set_result mm_apply_settz(const char *args)
{
    (void)args;
    return MC_SET_OK;
}
int mm_system_reboot(uint8_t reason, uint32_t sub)
{
    reboots++;
    reboot_reason = reason;
    reboot_sub    = sub;
    return 0;
}
int mm_system_poweroff(void)
{
    poweroffs++;
    return 0;
}
void fan_dev_stop(void)
{
    fan_stops++;
    fan_stop_poweroffs = poweroffs;
}
int mm_rail_off_unconfirmed(void)
{
    return rail_off_unconfirmed;
}
void mm_cfg_flush(void)
{
    (void)mm_cfgstore_flush();
}

void ui_show_update(int action, uint16_t id)
{
    assert(id != UI_MINING);
    if (action == UI_ACT_ADD || action == UI_ACT_DELETE)
        visible[id] = action == UI_ACT_ADD;
    if (action != UI_ACT_ADD)
        return;
    if (id == UI_CONFIG_NETWORK)
        config_shown++;
    if (id == UI_LOST_CONNECT)
        lost_shown++;
}

static int file_is(const char *path, const char *want)
{
    char  buf[128] = { 0 };
    FILE *f        = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return n == strlen(want) && !memcmp(buf, want, n);
}

static void factory_restore_atomic(void)
{
    static const char src[] = "mm_ui_provision_default.conf";
    static const char dst[] = "mm_ui_provision_user.conf";
    struct stat       st;
    FILE             *f = fopen(src, "w");
    assert(f && fputs("default\n", f) >= 0 && fclose(f) == 0);
    assert(chmod(src, 0640) == 0);
    f = fopen(dst, "w");
    assert(f && fputs("user edited, longer than the default\n", f) >= 0 && fclose(f) == 0);

    assert(factory_restore_file(src, dst) == 0);
    assert(file_is(dst, "default\n"));
    assert(stat(dst, &st) == 0 && (st.st_mode & 07777) == 0640);
    assert(access("mm_ui_provision_user.conf.tmp", F_OK) != 0);

    assert(factory_restore_file("mm_ui_provision_missing.conf", dst) == -1);
    assert(file_is(dst, "default\n"));
    assert(access("mm_ui_provision_user.conf.tmp", F_OK) != 0);

    remove(src);
    remove(dst);
    remove("mm_ui_provision_user.conf.lock");
}

static int file_same(const char *a, const char *b)
{
    char   x[4096], y[4096];
    FILE  *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    size_t na = fa ? fread(x, 1, sizeof x, fa) : 0, nb = fb ? fread(y, 1, sizeof y, fb) : 0;

    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return fa && fb && na == nb && !memcmp(x, y, na);
}

static int holds_defaults(const char *path, const char *defaults)
{
    ini_t *ini = ini_load(path), *def = ini_load(defaults);
    int    missing = ini && def ? ini_fill(ini, def) : -1;

    ini_free(ini);
    ini_free(def);
    return missing == 0;
}

static int store_is(const char *section, const char *key, const char *want)
{
    char buf[128];
    return mm_cfgstore_get(section, key, buf, sizeof buf) == 0 && !strcmp(buf, want);
}

static int store_is_factory(const char *section, const char *key)
{
    ini_t      *def  = ini_load(MC_SYSTEMCFG_FACTORY);
    const char *want = def ? ini_get(def, section, key) : NULL;
    int         same = want && want[0] && store_is(section, key, want);

    ini_free(def);
    return same;
}

static void boot_clean(void)
{
    static const char *const names[] = {
        "systemcfg.ini",
        "pools.conf",
        "wpa_supplicant.conf",
        "interfaces",
        "activation.ini",
        "hashrate_cali.ini",
    };
    char path[256];

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", USERCONFIG_DIR, names[i]);
        remove(path);
        snprintf(path, sizeof path, "%s/%s.lock", USERCONFIG_DIR, names[i]);
        remove(path);
    }
}

static void boot_seed_files(void)
{
    char src[256], dst[256];

    boot_clean();
    log_all[0] = '\0';
    mm_factory_boot_seed();
    for (size_t i = 0; i < sizeof factory_default_files / sizeof factory_default_files[0]; i++) {
        snprintf(src, sizeof src, "%s/%s", FACTORY_DEFAULT_DIR, factory_default_files[i]);
        snprintf(dst, sizeof dst, "%s/%s", USERCONFIG_DIR, factory_default_files[i]);
        assert(file_same(src, dst));
    }
    assert(strstr(log_all, USERCONFIG_DIR "/pools.conf missing — copied from"));
    assert(strstr(log_all, USERCONFIG_DIR "/interfaces missing — copied from"));
    log_all[0] = '\0';
    assert(file_is(USERCONFIG_DIR "/pools.conf", "{\"pools\":{}}\n") ||
            file_same(FACTORY_DEFAULT_DIR "/pools.conf", USERCONFIG_DIR "/pools.conf"));
    FILE *f = fopen(USERCONFIG_DIR "/interfaces", "w");
    assert(f && fputs("user\n", f) >= 0 && fclose(f) == 0);
    mm_factory_boot_seed();
    assert(file_is(USERCONFIG_DIR "/interfaces", "user\n") && log_all[0] == '\0');
    boot_clean();
}

static void boot_fill_missing(void)
{
    boot_clean();
    log_all[0] = '\0';
    mm_factory_boot_fill();
    assert(holds_defaults(MC_SYSTEMCFG_DEFAULT, MC_SYSTEMCFG_FACTORY));
    assert(store_is_factory("syscfg", "webuser") && store_is_factory("syscfg", "webpass"));
    assert(store_is_factory("syscfg", "work_mode"));
    assert(strstr(log_all, "missing setting(s) set from"));
    assert(holds_defaults(CALI_INI_PATH, CALI_INI_FACTORY));
    assert(store_is_factory("syscfg", "timezone"));
    assert(store_is("syscfg", "timezone", FACTORY_TIMEZONE));
#ifdef SUPPORT_BLE
    assert(holds_defaults(MC_ACTIVATION_DEFAULT, MC_ACTIVATION_FACTORY));
#else
    ini_t *sys = ini_load(MC_SYSTEMCFG_DEFAULT);
    assert(sys && !ini_get(sys, "syscfg", "ledmode") && !ini_get(sys, "led", "effect") &&
            !ini_get(sys, "misc", "filtermesh"));
    ini_free(sys);
#endif
    boot_clean();
}

static void boot_fill_partial(void)
{
    struct stat before, after;

    boot_clean();
    FILE *f = fopen(MC_SYSTEMCFG_DEFAULT, "w");
    assert(f &&
            fputs("[syscfg]\nreason = 3\nwebpass =\nmine = 7\n\n[led]\nbright = 40\n", f) >= 0 &&
            fclose(f) == 0);
    f = fopen(CALI_INI_PATH, "w");
    assert(f && fputs("[mode0]\ncali_param0 = 1-2-3-4-5\n", f) >= 0 && fclose(f) == 0);
    mm_factory_boot_fill();
    assert(holds_defaults(MC_SYSTEMCFG_DEFAULT, MC_SYSTEMCFG_FACTORY));
    assert(store_is("syscfg", "reason", "3") && store_is("syscfg", "mine", "7") &&
            store_is("led", "bright", "40"));
    assert(store_is_factory("syscfg", "webpass") && store_is_factory("syscfg", "webuser"));
    ini_t *cali = ini_load(CALI_INI_PATH);
    assert(cali && !strcmp(ini_get(cali, "mode0", "cali_param0"), "1-2-3-4-5"));
    ini_free(cali);
    assert(holds_defaults(CALI_INI_PATH, CALI_INI_FACTORY));

    assert(stat(MC_SYSTEMCFG_DEFAULT, &before) == 0);
    log_all[0] = '\0';
    mm_factory_boot_fill();
    assert(stat(MC_SYSTEMCFG_DEFAULT, &after) == 0 && before.st_ino == after.st_ino);
    assert(!strstr(log_all, "setting(s) set from") && !strstr(log_all, "unreadable"));
    boot_clean();
}

static void boot_fill_unreadable(void)
{
    boot_clean();
    assert(mkdir(MC_SYSTEMCFG_DEFAULT, 0755) == 0);
    assert(mkdir(CALI_INI_PATH, 0755) == 0);
    log_all[0] = '\0';
    mm_factory_boot_fill();
    assert(strstr(log_all, MC_SYSTEMCFG_DEFAULT " unreadable — recreated from"));
    assert(strstr(log_all, CALI_INI_PATH " unreadable — recreated from"));
    assert(holds_defaults(MC_SYSTEMCFG_DEFAULT, MC_SYSTEMCFG_FACTORY));
    assert(holds_defaults(CALI_INI_PATH, CALI_INI_FACTORY));
    assert(store_is_factory("syscfg", "webuser"));
    assert(mm_cfgstore_set("syscfg", "mine", "1") == 0 && mm_cfgstore_flush() == 1);
    boot_clean();
}

static void factory_reboot_failed_fills(void)
{
    boot_clean();
    FILE *f = fopen(MC_SYSTEMCFG_DEFAULT, "w");
    assert(f && fputs("[syscfg]\nreason = 1\n", f) >= 0 && fclose(f) == 0);
    factory_freeze(1, 1);
    factory_reboot_failed();
    assert(!mm_cfgstore_frozen());
    assert(store_is_factory("syscfg", "webuser") && store_is_factory("syscfg", "work_mode"));
    boot_clean();
    assert(mm_cfgstore_reload() == 0);
}

static void factory_default_set(void)
{
    size_t n = sizeof factory_default_files / sizeof factory_default_files[0];
    char   path[512];
    int    wpa = 0;

    for (size_t i = 0; i < n; i++) {
        wpa |= !strcmp(factory_default_files[i], "wpa_supplicant.conf");
        snprintf(path, sizeof path, "%s/%s", FACTORY_SEED_DIR, factory_default_files[i]);
        if (access(path, R_OK) != 0)
            fprintf(stderr, "factory default %s not found\n", path);
        assert(access(path, R_OK) == 0);
    }
#ifdef SUPPORT_BLE
    assert(n == 4 && wpa);
#else
    assert(n == 3 && !wpa);
#endif
}

static void factory_refuses_config_writes(void)
{
    mm_cfgwr_req req = {
        .magic = MM_CFGWR_MAGIC, .version = MM_CFGWR_VERSION, .kind = MM_CFGWR_WEBPASS
    };
    char hex[WEBPASS_HEX_LEN + 1];

    memset(req.u.webpass, 'a', WEBPASS_HEX_LEN);
    snprintf(hex, sizeof hex, "%s", req.u.webpass);
    assert(!mm_factory_in_progress());
    factory_freeze(1, 1);
    assert(mm_factory_in_progress() && mm_cfgstore_frozen());
    assert(mm_cfgwr_apply(&req, MC_SYSTEMCFG_DEFAULT) == -1);
    req.kind = MM_CFGWR_HWCFG;
    strcpy(req.u.hwcfg.prod, "Avalon Nano3s");
    strcpy(req.u.hwcfg.model, "Nano3s");
    assert(mm_cfgwr_apply(&req, MC_SYSTEMCFG_DEFAULT) == -1);
    req.kind = MM_CFGWR_ACTIVATION;
    memset(&req.u, 0, sizeof req.u);
    assert(mm_cfgwr_apply(&req, MC_SYSTEMCFG_DEFAULT) == -1);
    req.kind = MM_CFGWR_WALLPAPER_CLEAR;
    assert(mm_cfgwr_apply(&req, MC_SYSTEMCFG_DEFAULT) == -1);
    const char *const key[] = { "webpass" };
    const char *const val[] = { hex };
    assert(mm_cfgstore_commit("syscfg", key, val, 1) == -1);
    assert(mm_cfgstore_set("syscfg", "webpass", hex) == -1);
    assert(mm_cfgstore_flush() == -1);
    assert(access(MC_SYSTEMCFG_DEFAULT, F_OK) != 0);

    factory_freeze(0, 0);
    assert(!mm_factory_in_progress() && !mm_cfgstore_frozen());
    req.kind = MM_CFGWR_WEBPASS;
    snprintf(req.u.webpass, sizeof req.u.webpass, "%s", hex);
    assert(mm_cfgwr_apply(&req, MC_SYSTEMCFG_DEFAULT) == 0);
    assert(remove(MC_SYSTEMCFG_DEFAULT) == 0);
    remove(MC_SYSTEMCFG_DEFAULT ".lock");
    assert(mm_cfgstore_reload() == 0);
}

static void factory_reboot_failed_reload(void)
{
    assert(access(MC_SYSTEMCFG_DEFAULT, F_OK) != 0);
    factory_freeze(1, 1);
    factory_reboot_failed();
    assert(!mm_factory_in_progress() && !mm_cfgstore_frozen());

    assert(mkdir(MC_SYSTEMCFG_DEFAULT, 0755) == 0);
    factory_freeze(1, 1);
    factory_reboot_failed();
    assert(!mm_factory_in_progress() && mm_cfgstore_frozen());
    assert(mm_cfgstore_set("syscfg", "webuser", "admin") == -1);
    assert(mm_cfgstore_flush() == -1);
    assert(rmdir(MC_SYSTEMCFG_DEFAULT) == 0);
    factory_freeze(0, 0);
    assert(mm_cfgstore_reload() == 0);
}

static void factory_recover_drops_secrets(void)
{
    FILE *f = fopen(MC_WEB_TOKEN_KEY, "w");
    assert(f && fputs("secret", f) >= 0 && fclose(f) == 0);
    f = fopen(MM_EFUSE_FILE_PATH, "w");
    assert(f && fclose(f) == 0);
    reboots = 0;
    mm_factory_recover();
    assert(access(MC_WEB_TOKEN_KEY, F_OK) != 0);
    assert(access(MM_EFUSE_FILE_PATH, F_OK) != 0);
    assert(reboots == 1 && reboot_reason == MC_BOOTBY_FAC_RESTORE);
    reboots       = 0;
    reboot_reason = 0;
    reboot_sub    = 0;
    factory_freeze(0, 0);
    boot_clean();
    assert(mm_cfgstore_reload() == 0);
}

static void post_events(btn_event ev, int n)
{
    pthread_mutex_lock(&g.dec_lock);
    while (n--)
        btn_event_post(ev);
    pthread_mutex_unlock(&g.dec_lock);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *late_press(void *arg)
{
    (void)arg;
    usleep(20000);
    post_events(BTN_EV_TP2_SHORT, 1);
    return NULL;
}

static void btn_wake_drain(void)
{
    int       cycles = display_cycles, ticks = tst_lcd_n;
    uint64_t  t0, clk                        = clock_ms;
    pthread_t th;

    dark = 0;
    assert(mm_ui_btn_wait(10) == 0);

    post_events(BTN_EV_TP2_SHORT, 1);
    t0 = mono_ms();
    assert(mm_ui_btn_wait(60000) == 1);
    assert(mono_ms() - t0 < 1000);
    assert(display_cycles == cycles);
    mm_ui_btn_drain();
    assert(display_cycles == cycles + 1);

    assert(pthread_create(&th, NULL, late_press, NULL) == 0);
    t0 = mono_ms();
    assert(mm_ui_btn_wait(60000) == 1);
    assert(mono_ms() - t0 < 10000);
    assert(pthread_join(th, NULL) == 0);
    mm_ui_btn_drain();
    assert(display_cycles == cycles + 2);
    assert(mm_ui_btn_wait(10) == 0);
    assert(clock_ms == clk && tst_lcd_n == ticks);
}

static void ui_counts_reset(void)
{
    input_starts = input_stops = 0;
    thread_creates = thread_joins = 0;
    beep_closes = ui_stops = 0;
    beep_on = beep_ons = 0;
    beep_off_stops     = -1;
    log_line[0]        = '\0';
}

static void ui_start_no_buttons(void)
{
    ui_counts_reset();
    input_start_rc = -1;
    mm_ui_start();
    assert(input_starts == 1 && thread_creates == 0 && !atomic_load(&g.tick_run));
    assert(log_level == OSIF_LOG_ERR &&
            !strcmp(log_line, "Buttons unavailable: input start failed"));
    mm_ui_stop();
    assert(thread_joins == 0 && beep_closes == 1 && ui_stops == 1 && !g.started);
    mm_ui_stop();
    assert(thread_joins == 0 && beep_closes == 1 && ui_stops == 1);

    ui_counts_reset();
    tst_cap_button = 0;
    mm_ui_start();
    assert(input_starts == 1 && thread_creates == 0 && !atomic_load(&g.tick_run));
    assert(log_line[0] == '\0');
    mm_ui_stop();
    assert(thread_joins == 0 && beep_closes == 1 && ui_stops == 1 && !g.started);
    tst_cap_button = 1;
    input_start_rc = 0;
}

static void ui_start_buttons(void)
{
    ui_counts_reset();
    mm_ui_start();
    assert(input_starts == 1 && thread_creates == 1 && atomic_load(&g.tick_run));
    assert(input_stops == 0 && log_line[0] == '\0');
    mm_ui_stop();
    assert(!atomic_load(&g.tick_run) && thread_joins == 1 && input_stops == 1);
    assert(beep_closes == 1 && ui_stops == 1 && !g.started);
    mm_ui_stop();
    assert(thread_joins == 1 && input_stops == 1 && beep_closes == 1 && ui_stops == 1);
}

static void ui_start_decode_thread_fails(void)
{
    ui_counts_reset();
    thread_create_rc = EAGAIN;
    mm_ui_start();
    assert(input_starts == 1 && thread_creates == 1 && input_stops == 1);
    assert(!atomic_load(&g.tick_run));
    assert(beep_ons == 1 && !beep_on && beep_off_stops == 1 && g.beep_off_at == 0);
    assert(log_level == OSIF_LOG_ERR &&
            !strcmp(log_line, "Buttons unavailable: decode thread start failed"));
    mm_ui_stop();
    assert(thread_joins == 0 && beep_closes == 1 && ui_stops == 1 && !g.started);
    mm_ui_stop();
    assert(thread_joins == 0 && beep_closes == 1 && ui_stops == 1);
    thread_create_rc = 0;
}

static void ui_stop_poweroff(void)
{
    ui_counts_reset();
    mm_ui_start();
    g.poweroff_req = 1;
    mm_ui_stop();
    assert(fan_stops == 1 && fan_stop_poweroffs == 0 && poweroffs == 1 && !g.started);
    mm_ui_stop();
    assert(fan_stops == 1 && poweroffs == 1);

    ui_counts_reset();
    rail_off_unconfirmed = 1;
    mm_ui_start();
    mm_ui_stop();
    assert(fan_stops == 1 && poweroffs == 2 && !g.started);
    rail_off_unconfirmed = 0;
    g.poweroff_req       = 0;
}

int main(void)
{
    ui_summary summary;
    assert(summary_cb(&summary, NULL) == 0 && summary.ready == 0);
    assert(!strcmp(summary.ip, "--") && !strcmp(summary.hashrate, "--"));
    assert(!strcmp(summary.level, "--") && !strcmp(summary.power, "--"));
    strcpy(wired_ip, "10.0.0.2");
    assert(summary_cb(&summary, NULL) == 0 && !strcmp(summary.ip, "10.0.0.2"));
    wired_ip[0] = 0;
    btn_ev_cond_init();
    mkdir(USERCONFIG_DIR, 0755);
    boot_seed_files();
    boot_fill_missing();
    boot_fill_partial();
    boot_fill_unreadable();
    factory_reboot_failed_fills();
    remove(MC_SYSTEMCFG_DEFAULT);
    assert(mm_cfgstore_open(MC_SYSTEMCFG_DEFAULT) == 0);
    factory_restore_atomic();
    factory_default_set();
    factory_refuses_config_writes();
    factory_reboot_failed_reload();
    factory_recover_drops_secrets();
    mm_ui_tick();
    assert(config_shown == 1 && wakes == 0);
    clock_ms = 1499;
    mm_ui_tick();
    assert(wakes == 0);
    clock_ms = 1500;
    mm_ui_tick();
    assert(wakes == 1 && !dark);
    mm_ui_tick();
    assert(wakes == 1);

    dark = 1;
    strcpy(network_ssid, "configured-network");
    clock_ms = 2000;
    mm_ui_tick();
    clock_ms = 5000;
    mm_ui_tick();
    assert(wakes == 1 && dark);

    network_ssid[0] = 0;
    mm_ui_tick();
    strcpy(network_ssid, "new-network");
    clock_ms = 7000;
    mm_ui_tick();
    assert(wakes == 1 && dark);

    network_type    = 0;
    network_ssid[0] = 0;
    mm_ui_tick();
    clock_ms = 10000;
    mm_ui_tick();
    assert(wakes == 1 && dark);
    start_provisioning();
    assert(wakes == 2 && provisions == 0 && !dark);
    network_type = 1;
    dark         = 1;
    start_provisioning();
    assert(wakes == 2 && provisions == 1 && dark);

    network_state = 2;
    mm_ui_tick();
    wired_present = 0;
    has_ssid      = 1;
    network_state = 0;
    config_shown = lost_shown = 0;
    mm_ui_tick();
    assert(lost_shown == 1 && config_shown == 0);

    network_state = 2;
    mm_ui_tick();
    wired_present = 1;
    has_ssid      = 0;
    network_state = 0;
    config_shown = lost_shown = 0;
    mm_ui_tick();
    assert(lost_shown == 1 && config_shown == 0);

    network_state = 2;
    mm_ui_tick();
    wired_present = 0;
    has_ssid      = 0;
    network_state = 0;
    config_shown = lost_shown = 0;
    mm_ui_tick();
    assert(config_shown == 1 && lost_shown == 0);

    network_ssid[0] = 0;
    network_state   = 0;
    dongle          = 0;
    clock_ms        = 20000;
    mm_ui_tick();
    assert(visible[UI_USB_DEVICE] && !visible[UI_CONFIG_NETWORK] && !visible[UI_LOST_CONNECT]);
    int shown = config_shown;
    clock_ms  = 30000;
    mm_ui_tick();
    assert(config_shown == shown && wakes == 2 && dark);
    dongle = 1;
    mm_ui_tick();
    assert(!visible[UI_USB_DEVICE] && visible[UI_CONFIG_NETWORK]);

    network_type  = 0;
    dark          = 0;
    network_state = 2;
    mm_ui_tick();
    int lcd = tst_lcd_n;
    start_provisioning();
    network_state = 0;
    mm_ui_tick();
    network_state = 2;
    mm_ui_tick();
    assert(tst_lcd_n == lcd);

    network_type    = 1;
    network_ssid[0] = 0;
    network_state   = 0;
    mm_ui_tick();
    network_state = 2;
    mm_ui_tick();
    assert(tst_lcd_n == lcd);
    strcpy(network_ssid, "home");
    network_state = 0;
    mm_ui_tick();
    network_state = 2;
    mm_ui_tick();
    assert(tst_lcd_n == lcd + 1 && tst_lcd_id == 2 && tst_lcd_value == UI_SUMMARY);
    network_state = 0;
    mm_ui_tick();
    network_state = 2;
    mm_ui_tick();
    assert(tst_lcd_n == lcd + 1);
    network_state = 0;
    mm_ui_tick();
    start_provisioning();
    network_state = 2;
    mm_ui_tick();
    assert(tst_lcd_n == lcd + 2 && tst_lcd_id == 2 && tst_lcd_value == UI_SUMMARY);
    network_ssid[0] = 0;

    network_type = 1;
    dark         = 0;
    int prov     = provisions;
    post_events(BTN_EV_TP1_SHORT, 1);
    post_events(BTN_EV_TP2_SHORT, 2);
    assert(provisions == prov && display_cycles == 0);
    mm_ui_tick();
    assert(provisions == prov + 1 && display_cycles == 2);
    mm_ui_tick();
    assert(provisions == prov + 1 && display_cycles == 2);
    post_events(BTN_EV_TP2_SHORT, BTN_EV_QUEUE_LEN + 1);
    mm_ui_tick();
    assert(display_cycles == 2 + BTN_EV_QUEUE_LEN);
    btn_wake_drain();
    ui_start_no_buttons();
    ui_start_buttons();
    ui_start_decode_thread_fails();
    assert(reboots == 0 && poweroffs == 0 && reboot_reason == 0 && reboot_sub == 0);
    assert(fan_stops == 0);
    ui_stop_poweroff();
    return 0;
}
