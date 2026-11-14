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

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "boardconf.h"
#include "mm_spawn.h"
#include "tst_board_expect.h"
#include "cali.h"
#include "fan_dev.h"
#include "miner_ipc.h"
#include "mm_led.h"
#include "mm_telem.h"
#include "mm_display.h"
#include "osif.h"
#include "wdt_dev.h"
#include "mm_cfgstore.h"
#include "ui_screen.h"
#include "ui_types.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef MM_MINER_VERSION
#define MM_MINER_VERSION "26091104_2138_abcdef0"
#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* asic_miner spawn delay, s: none */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_PUSH_MAX  256      /* MSG_MM_PERIPH sends recorded */
#define TST_SLEEP_CAP 4096     /* osif_sleep_ms() calls before g_run is forced to 0 */
#define TST_SN        "SN0001" /* hash board serial prefix; eeprom_read() appends -<id> */
#define HOT_OUT       65       /* hashboard outlet temperature, degC */
#define HOT_IN        41       /* hashboard inlet temperature, degC */
#define MCU_NONE      (-273)   /* hashboard MCU temperature not read, degC */
/* mm_errcode bits for the asic_miner link */
#define ECMM_ASIC_BITS (ERR_MM_INTERNUCLEAR | ERR_MM_SLAVE)

osif_status_t  tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len);
void           tst_led_state(uint8_t *ledmode, uint8_t *runeffect);
void           tst_led_snapshot(mm_led_cfg_t *cfg, uint8_t *effect);
pid_t          tst_fork(void);
int            tst_system(const char *cmd);
int            tst_kill(pid_t pid, int sig);
pid_t          tst_waitpid(pid_t pid, int *status, int options);
int            tst_stat(const char *path, struct stat *st);
static void    test_log(osif_loglvl_t level, const char *format, ...);
static int     tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
            cali_param *out, uint8_t *aged);
static uint8_t tst_cali_workmode_get(const cali_wm_store *s);
static int     tst_cali_get(const char *path, uint8_t mode, int8_t level, char *out, size_t cap);
static int     tst_cali_parse(const char *str, cali_param *out);

/* Calls in the included sources, routed to the fakes below */
#define osif_ipc_send(h, t, d, l)      tst_ipc_send((h), (t), (d), (l))
#define osif_log                       test_log
#define mm_led_state(m, r)             tst_led_state((m), (r))
#define mm_led_snapshot                tst_led_snapshot
#define fork()                         tst_fork()
#define system(c)                      tst_system(c)
#define kill(p, s)                     tst_kill((p), (s))
#define waitpid(p, s, o)               tst_waitpid((p), (s), (o))
#define stat(p, b)                     tst_stat((p), (b))
#define cali_resolve(p, c, m, l, o, a) tst_cali_resolve((p), (c), (m), (l), (o), (a))
#define cali_workmode_get(p)           tst_cali_workmode_get(p)
#define cali_get(p, m, l, o, c)        tst_cali_get((p), (m), (l), (o), (c))
#define cali_parse(s, o)               tst_cali_parse((s), (o))

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef cali_parse
#undef cali_get
#undef cali_workmode_get
#undef cali_resolve
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork
#undef mm_led_state
#undef osif_ipc_send
#undef osif_log

static int overhot_visible, overhot_updates; /* UI_OVERHOT shown flag; its updates */
/* Shown flag and ui_show_update() count per screen ID */
static int  visible[UI_SHOW_MAX], updates_by_id[UI_SHOW_MAX];
static char startup_banner[256]; /* last "Mac ..." log line */
/* Display state mm_display_snapshot() returns */
static mm_display_state test_display = { .show = 3, .onoff = 0, .bright = 37, .volume = 25 };
static int              filter_stages;       /* mm_cfgstore_set() filtermesh call count */
static int              filter_puts;         /* mm_cfgstore_put() filtermesh call count */
static const char      *filter_value;        /* stored [misc] filtermesh; NULL absent */
static const char      *filter_legacy_value; /* stored [syscfg] filtermesh_time; NULL absent */
static char             filter_saved[32];    /* last filtermesh value written */
static int              tst_cali_calls;      /* cali_resolve() call count */
static uint16_t         tst_cali_asic_cnt;   /* last cali_resolve() chip count */
static uint8_t          tst_cali_mode;       /* last cali_resolve() work mode */
static int              tst_cali_override;   /* nonzero returns tst_cali_row, else zeros */
static cali_param       tst_cali_row;        /* calibration row cali_resolve() returns */
static uint8_t          tst_cali_aged;       /* aged flag with tst_cali_row */
static uint8_t          tst_workmode;        /* stored work mode */
static unsigned         tst_cali_get_mask;   /* bit per work mode whose cali_get() succeeds */
static uint8_t          tst_aging_finished;  /* aging_finished in mm_vf_snapshot() */

static int          tst_fresh;      /* mm_telem_get() poll_fresh and in_work */
static int          tst_asic_live;  /* mm_telem_get() asic_live */
static int          tst_asic_chips; /* mm_telem_get() asic_chips before tst_chips[] is added */
static int          tst_hb_count;   /* mm_telem_get() hb_count */
static int          tst_outlet[HBOARD_COUNT];    /* outlet temperature per board, degC */
static int          tst_inlet[HBOARD_COUNT];     /* inlet temperature per board, degC */
static int          tst_asic_boards;             /* mm_telem_get() asic_boards */
static int          tst_chips[HBOARD_COUNT];     /* chips per board */
static uint8_t      tst_ledmode, tst_runeffect;  /* LED mode and running effect */
static int          tst_duty_pct;                /* fan duty, percent */
static mm_periph    tst_push[TST_PUSH_MAX];      /* MSG_MM_PERIPH payloads, 0xa5 past len */
static uint32_t     tst_push_type[TST_PUSH_MAX]; /* message type of each */
static int          tst_push_len[TST_PUSH_MAX];  /* length of each, bytes */
static int          tst_pushes;                  /* MSG_MM_PERIPH sends recorded */
static int          tst_other_msgs;              /* sends of other unrecorded types */
static mc_mm_status last_status;                 /* last MSG_MM_STATUS payload */
static char last_version[MINER_VERSION_CAP];     /* last MSG_MM_VERSION payload, 0xa5 past len */
static int  last_version_len;                    /* its length, bytes */
static int  version_messages;                    /* MSG_MM_VERSION sends */
static mc_mm_bootby last_bootby;                 /* last MSG_MM_BOOTBY payload */
static int          bootby_messages;             /* MSG_MM_BOOTBY sends */
static uint64_t     tst_now;                     /* fake monotonic clock, ms */
/* Passes run and wanted; osif_sleep_ms() calls; TST_SLEEP_CAP overrun flag */
static int      tst_pass, tst_want, tst_sleeps, tst_runaway;
static int      tst_eeprom_read_n;
static pid_t    tst_next_pid = 4000; /* next fake fork() PID */
static int      tst_asic_tag;        /* address is the fake asic_miner IPC handle */
static int      tst_asic_connected = 1;
static uint32_t echu_bits; /* last hu_errcode[0] */
static uint32_t ecmm_bits; /* last mm_errcode asic bits */

static void test_log(osif_loglvl_t level, const char *format, ...)
{
    (void)level;
    if (strncmp(format, "Mac ", 4))
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(startup_banner, sizeof startup_banner, format, args);
    va_end(args);
}
void ui_show_update(int action, uint16_t id)
{
    visible[id] = action == UI_ACT_ADD;
    updates_by_id[id]++;
    if (id == UI_OVERHOT) {
        overhot_visible = action == UI_ACT_ADD;
        overhot_updates++;
    }
}
void mm_ui_start(void)
{
}
void mm_ui_tick(void)
{
}
int mm_ui_btn_wait(uint32_t ms)
{
    osif_sleep_ms(ms);
    return 0;
}
void mm_ui_btn_drain(void)
{
}
void mm_ui_stop(void)
{
}
void mm_ui_wallpaper_reload(void)
{
}
int mm_ui_poweroff_requested(void)
{
    return 0;
}
int mm_factory_in_progress(void)
{
    return 0;
}
int mm_display_lcd(int id, int value)
{
    (void)id;
    (void)value;
    return 0;
}
void mm_display_snapshot(mm_display_state *out)
{
    *out = test_display;
}

int mm_cfgstore_get(const char *section, const char *key, char *buf, size_t cap)
{
    const char *v = NULL;
    if (!strcmp(section, "misc") && !strcmp(key, "filtermesh"))
        v = filter_value;
    if (!strcmp(section, "syscfg") && !strcmp(key, "filtermesh_time"))
        v = filter_legacy_value;
    if (!v || strlen(v) >= cap)
        return -1;
    snprintf(buf, cap, "%s", v);
    return 0;
}
int mm_cfgstore_set(const char *section, const char *key, const char *value)
{
    if (strcmp(section, "misc") || strcmp(key, "filtermesh"))
        return -1;
    filter_stages++;
    snprintf(filter_saved, sizeof filter_saved, "%s", value);
    return 0;
}
int mm_cfgstore_put(const char *section, const char *key, const char *value)
{
    if (strcmp(section, "misc") || strcmp(key, "filtermesh"))
        return -1;
    filter_puts++;
    snprintf(filter_saved, sizeof filter_saved, "%s", value);
    return 0;
}

static int tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
        cali_param *out, uint8_t *aged)
{
    (void)path;
    (void)level;
    tst_cali_calls++;
    tst_cali_asic_cnt = asic_cnt;
    tst_cali_mode     = mode;
    if (out) {
        if (tst_cali_override)
            *out = tst_cali_row;
        else
            memset(out, 0, sizeof *out);
    }
    if (aged)
        *aged = tst_cali_override ? tst_cali_aged : 0;
    return 0;
}

static uint8_t tst_cali_workmode_get(const cali_wm_store *s)
{
    (void)s;
    return tst_workmode;
}

static int tst_cali_get(const char *path, uint8_t mode, int8_t level, char *out, size_t cap)
{
    (void)path;
    (void)level;
    if (!(tst_cali_get_mask & (1u << mode)))
        return -1;
    snprintf(out, cap, "150-90-3450-400-20");
    return 0;
}

static int tst_cali_parse(const char *str, cali_param *out)
{
    if (out)
        memset(out, 0, sizeof *out);
    return str && *str ? 0 : -1;
}

void mm_vf_snapshot(mm_periph *p)
{
    if (p)
        p->aging_finished = tst_aging_finished;
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->ps_vout_mv  = POWER_VALID_VOLT_MIN;
    out->poll_fresh  = tst_fresh;
    out->asic_live   = tst_asic_live;
    out->asic_chips  = tst_asic_chips;
    out->in_work     = tst_fresh;
    out->tmax        = 60;
    out->tavg        = 55;
    out->hb_count    = tst_hb_count;
    out->asic_boards = tst_asic_boards;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        out->hb_outlet[b]        = tst_outlet[b];
        out->hb_inlet[b]         = tst_inlet[b];
        out->asic_chips_board[b] = tst_chips[b];
        out->asic_chips += tst_chips[b];
    }
}

void tst_led_state(uint8_t *ledmode, uint8_t *runeffect)
{
    if (ledmode)
        *ledmode = tst_ledmode;
    if (runeffect)
        *runeffect = tst_runeffect;
}
void tst_led_snapshot(mm_led_cfg_t *cfg, uint8_t *effect)
{
    *cfg         = mm_led_cfg_default;
    cfg->ledmode = tst_ledmode;
    *effect      = tst_runeffect;
}

int fan_dev_init(void)
{
    return 0;
}
void fan_dev_set_duty_x10(int d)
{
    tst_duty_pct = d / 10;
}
int fan_dev_last_duty_x10(void)
{
    return tst_duty_pct * 10;
}
int fan_dev_get_duty_pct(void)
{
    return tst_duty_pct;
}

osif_status_t tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_MM_VERSION && data && len > 0 && len <= MINER_VERSION_CAP) {
        memset(last_version, 0xa5, sizeof last_version);
        memcpy(last_version, data, (size_t)len);
        last_version_len = len;
        version_messages++;
        return OSIF_OK;
    }
    if (type == MSG_MM_STATUS && len == sizeof last_status)
        memcpy(&last_status, data, sizeof last_status);
    if (type == MSG_MM_BOOTBY && data && len == (int)sizeof last_bootby) {
        memcpy(&last_bootby, data, sizeof last_bootby);
        bootby_messages++;
        return OSIF_OK;
    }
    if (type != MSG_MM_PERIPH) {
        tst_other_msgs++;
        return OSIF_OK;
    }
    if (tst_pushes < TST_PUSH_MAX) {
        tst_push_type[tst_pushes] = type;
        tst_push_len[tst_pushes]  = len;
        memset(&tst_push[tst_pushes], 0xa5, sizeof tst_push[0]);
        if (data && len > 0 && (size_t)len <= sizeof tst_push[0])
            memcpy(&tst_push[tst_pushes], data, (size_t)len);
        tst_pushes++;
    }
    return OSIF_OK;
}

static void push_reset(void)
{
    tst_pushes       = 0;
    tst_other_msgs   = 0;
    version_messages = 0;
    last_version_len = 0;
    bootby_messages  = 0;
}
static const mm_periph *last_push(void)
{
    return tst_pushes > 0 ? &tst_push[tst_pushes - 1] : NULL;
}

uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_now += ms;
    if (++tst_sleeps <= TST_SLEEP_CAP)
        return;
    tst_runaway = 1;
    g_run       = 0;
    if (tst_sleeps > 2 * TST_SLEEP_CAP) {
        fprintf(stderr,
                "  FAIL: the supervisor loop does not exit on a cleared "
                "g_run — aborting\n");
        _exit(1);
    }
}
void mm_telem_poll(void)
{
    if (++tst_pass >= tst_want)
        g_run = 0;
}
void mm_telem_ingest(const void *d, int l, unsigned c)
{
    (void)d;
    (void)l;
    (void)c;
}
void mm_telem_ingest_core(const void *d, int l)
{
    (void)d;
    (void)l;
}
int eeprom_init(void)
{
    return 0;
}
int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t len, uint8_t id)
{
    (void)addr;
    tst_eeprom_read_n++;
    memset(pbuf, 0, len);
    snprintf((char *)pbuf, len, "%s-%u", TST_SN, (unsigned)id);
    return 0;
}

int wdt_dev_init(void)
{
    return 0;
}
int wdt_dev_bootstatus(void)
{
    return 0;
}
int wdt_dev_timeout_s(void)
{
    return 2 * WDT_TIMEOUT_S;
}
int wdt_dev_deadline_s(void)
{
    return WDT_TIMEOUT_S;
}
int wdt_dev_feed_period_ms(void)
{
    return WDT_FEED_PERIOD_MS_MAX;
}
void wdt_dev_disarm(void)
{
}
void wdt_dev_feed(void)
{
}

pid_t tst_fork(void)
{
    return tst_next_pid++;
}
int tst_system(const char *cmd)
{
    (void)cmd;
    return 0;
}
int tst_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)options;
    if (status)
        *status = 0;
    return 0;
}
int tst_stat(const char *path, struct stat *st)
{
    (void)path;
    if (st)
        memset(st, 0, sizeof *st);
    return -1;
}

static void telem_set(int fresh, int n, int degc_out, int degc_in)
{
    tst_fresh      = fresh;
    tst_asic_live  = fresh;
    tst_asic_chips = 0;
    tst_hb_count   = n;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        tst_outlet[b] = degc_out;
        tst_inlet[b]  = degc_in;
    }
}

static int rows_all_zero(const mm_periph *p)
{
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (p->hb_outlet[b] != 0 || p->hb_inlet[b] != 0)
            return 0;
    return 1;
}

static int first_nonzero_slot(const mm_periph *p)
{
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (p->hb_outlet[b] != 0 || p->hb_inlet[b] != 0)
            return b;
    return -1;
}

int osif_ipc_connected(osif_ipc_t *h)
{
    return h == (osif_ipc_t *)&tst_asic_tag ? tst_asic_connected : 1;
}

static void err_reset(void)
{
    tst_asic_connected = 1;
    tst_asic_live      = 1;
    tst_asic_chips     = MINER_MAX_CHIPS;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    g_fault_off   = 0;
    g_softoff_rsn = MC_SOFTOFF_NONE;
}

static void err_push(void)
{
    push_reset();
    mm_periph_push();
    echu_bits = last_push()->hu_errcode[0];
    ecmm_bits = last_push()->mm_errcode & ECMM_ASIC_BITS;
}

int main(void)
{
    const mm_periph *p;

    printf("descriptor %s: %d board(s), hashboard MCUs %s, sizeof(mm_periph) %zu\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT,
#if CAP_HB_MCU
            "present (" I2C_MCU_DEV ")",
#else  /* CAP_HB_MCU */
            "ABSENT — mm_telem's own hb_count is structurally 0 here",
#endif /* CAP_HB_MCU */
            sizeof(mm_periph));

    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    tst_ledmode   = 1;
    tst_runeffect = 7;
    tst_duty_pct  = 42;
    push_reset();
    mm_periph_push();
    p = last_push();
    CHECK(p && p->led_effect == 1 && p->led_bright == 100 && p->led_temper == 50 &&
                    p->led_rgb == 255,
            "mm publishes every LEDUser field from its LED snapshot");

    CHECK(tst_pushes == 1 && tst_other_msgs == 0 && p != NULL && version_messages == 1 &&
                    !strcmp(last_version, MM_MINER_VERSION) && bootby_messages == 1,
            "one call sends the peripheral snapshot, its own compiled version and "
            "the boot reason -- the last of these is how core_miner learns BOOTBY "
            "rather than reading it out of systemcfg.ini");
    CHECK(last_version_len == (int)sizeof MM_MINER_VERSION &&
                    !memcmp(last_version, MM_MINER_VERSION, sizeof MM_MINER_VERSION),
            "the version datagram carries the terminating NUL INSIDE its length "
            "(%d bytes, want %d) -- miner_version_receive refuses a payload whose "
            "last byte is not '\\0', so a producer that sent one byte less would "
            "leave core_miner reporting LVer[" MINER_VERSION_UNKNOWN "] for ever "
            "while every value byte on the wire was correct",
            last_version_len, (int)sizeof MM_MINER_VERSION);
    CHECK(tst_push_type[0] == MSG_MM_PERIPH && tst_push_len[0] == (int)sizeof(mm_periph),
            "...as MSG_MM_PERIPH (%u) at the FULL %zu bytes — core drops anything "
            "shorter whole, so a short push would silently kill FanR, LED, LEDUser "
            "and both temperature rows together (saw type %u, %d bytes)",
            MSG_MM_PERIPH, sizeof(mm_periph), tst_push_type[0], tst_push_len[0]);
    CHECK(p && p->valid == 1,
            "...with valid=1, which is what makes core prefer this live state over "
            "the config file it otherwise falls back to");
    CHECK(p && p->lcd_onoff == 0 && p->lcd_show == 3 && p->lcd_bright == 37 && p->volume == 25,
            "LCD and audio state travel in the peripheral snapshot");

    CHECK(p && p->hash_sn[0][0] == '\0' && tst_eeprom_read_n == 0,
            "HASHBOARD SERIALS: mm owns the power-board SN EEPROM because it "
            "shares the i2c bus mm writes the ASIC core rail on and the bus mutex "
            "is process-local, so core_miner answers serial reads off this push "
            "instead of reading the bus at every one of its (re)starts. "
            "Before mm loads them the table is blank and no read has happened — "
            "this is exactly the window a freshly started core_miner answers "
            "a serial read blank in, and it closes on mm's next ~1 s push");

    mm_hash_sn_load();
    CHECK(tst_eeprom_read_n == HBOARD_COUNT,
            "the load reads all %d board(s) ONCE, at mm startup (saw %d)", (int)HBOARD_COUNT,
            tst_eeprom_read_n);

    push_reset();
    mm_periph_push();
    mm_periph_push();
    p = last_push();
    {
        int  all = 1;
        char want[HASH_SN_LEN + 1];
        for (int b = 0; b < HBOARD_COUNT; b++) {
            snprintf(want, sizeof want, "%s-%d", TST_SN, b);
            if (!p || strcmp(p->hash_sn[b], want))
                all = 0;
        }
        CHECK(all && tst_eeprom_read_n == HBOARD_COUNT,
                "...and every push carries all %d serial(s) verbatim while the "
                "read count stays %d: the table is published, not re-read, so a "
                "core_miner respawn costs no i2c traffic at all (saw %d read(s), "
                "hash_sn[0]='%s')",
                (int)HBOARD_COUNT, (int)HBOARD_COUNT, tst_eeprom_read_n, p ? p->hash_sn[0] : "");
    }

    {
        char sn[HASH_SN_LEN];
        char want[HASH_SN_LEN + 1];
        int  others = 1;
        memset(sn, 'N', sizeof sn);
        mm_hash_sn_set(HBOARD_COUNT - 1, sn);
        push_reset();
        mm_periph_push();
        p = last_push();
        memset(want, 'N', HASH_SN_LEN);
        want[HASH_SN_LEN] = '\0';
        for (int b = 0; b < HBOARD_COUNT - 1; b++) {
            char old[HASH_SN_LEN + 1];
            snprintf(old, sizeof old, "%s-%d", TST_SN, b);
            if (!p || strcmp(p->hash_sn[b], old))
                others = 0;
        }
        CHECK(p && !strcmp(p->hash_sn[HBOARD_COUNT - 1], want) && others &&
                        tst_eeprom_read_n == HBOARD_COUNT,
                "mm_hash_sn_set() replaces board %d's cached serial with all %d bytes, the next "
                "push carries it, the other boards keep theirs and no EEPROM read happens "
                "(saw '%s', %d read(s))",
                (int)HBOARD_COUNT - 1, (int)HASH_SN_LEN, p ? p->hash_sn[HBOARD_COUNT - 1] : "",
                tst_eeprom_read_n);
    }

    telem_set(0, HBOARD_COUNT, HOT_OUT, HOT_IN);
    push_reset();
    mm_periph_push();
    p = last_push();

    CHECK(p && p->hb_count == 0,
            "a STALE snapshot publishes hb_count 0 — the consumer loops that "
            "count, so it reads nothing rather than a temperature that is no "
            "longer true (saw %d)",
            p ? (int)p->hb_count : -1);
    CHECK(p && rows_all_zero(p),
            "...and BOTH rows are left zeroed, not merely uncounted: the %d degC "
            "outlet / %d degC inlet behind the gate reach no slot at all, so a "
            "consumer that ignored hb_count still could not read a stale value "
            "(first non-zero slot %d)",
            HOT_OUT, HOT_IN, p ? first_nonzero_slot(p) : -2);

    push_reset();
    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    mm_periph_push();
    telem_set(0, HBOARD_COUNT, HOT_OUT, HOT_IN);
    mm_periph_push();

    CHECK(tst_pushes == 2 && tst_push[0].hb_count == HBOARD_COUNT && tst_push[1].hb_count == 0 &&
                    rows_all_zero(&tst_push[1]),
            "a hot push followed by a stale one leaves NO residue in the second: "
            "%d board(s) then 0, and every slot back to zero — the snapshot is "
            "rebuilt per call, so going stale erases rather than freezes",
            (int)tst_push[0].hb_count);

    CHECK(tst_push[0].hb_outlet[0] == HOT_OUT && tst_push[0].hb_inlet[0] == HOT_IN,
            "...and the FRESH push before it did carry %d/%d degC, so the erasure "
            "above is the gate acting and not the rows never being filled",
            HOT_OUT, HOT_IN);

    telem_set(1, HBOARD_COUNT, MCU_NONE, MCU_NONE);
    push_reset();
    mm_periph_push();
    p = last_push();

    CHECK(p && p->hb_count == HBOARD_COUNT && p->hb_outlet[0] == MCU_NONE &&
                    p->hb_inlet[0] == MCU_NONE,
            "a FRESH snapshot from a DEAD MCU still publishes: %d counted "
            "board(s) carrying the %d sentinel, NOT a withheld count. 'no board' "
            "and 'board present, MCU silent' stay distinguishable, which is what "
            "the thermal soft-off's outlet arm and the fault classifier both depend on",
            (int)HBOARD_COUNT, MCU_NONE);

    CHECK((int)(int16_t)MCU_NONE == MCU_NONE,
            "...and %d survives the int -> int16_t narrowing into mm_periph's row "
            "(the rows are int16_t while mm_telem's are int)",
            MCU_NONE);

    {
        int fewer = HBOARD_COUNT - 1;
        telem_set(1, fewer, HOT_OUT, HOT_IN);
        push_reset();
        mm_periph_push();
        p = last_push();

        int tail_clean = 1;
        for (int b = fewer; b < HBOARD_COUNT; b++)
            if (p->hb_outlet[b] != 0 || p->hb_inlet[b] != 0)
                tail_clean = 0;

        CHECK(p && p->hb_count == fewer && tail_clean,
                "a snapshot reporting %d of %d board(s) writes only those slots — "
                "the remaining %d stay at zero, so a board that dropped out reads "
                "as absent rather than keeping its last temperature",
                fewer, (int)HBOARD_COUNT, (int)HBOARD_COUNT - fewer);
    }

    {
        int over = 0;
        for (int i = 0; i < tst_pushes; i++)
            if (tst_push[i].hb_count > HBOARD_COUNT)
                over++;
        telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
        push_reset();
        mm_periph_push();
        CHECK(over == 0 && last_push()->hb_count <= HBOARD_COUNT,
                "no push ever reports more than the %d row slot(s) this descriptor "
                "has — mm's copy loop is bounded by the SNAPSHOT's count and is "
                "NOT clamped (the consumer's is), so this invariant is what keeps "
                "it in range",
                (int)HBOARD_COUNT);
    }

    tst_ledmode   = 1;
    tst_runeffect = 5;
    tst_duty_pct  = 37;
    telem_set(0, HBOARD_COUNT, HOT_OUT, HOT_IN);
    push_reset();
    mm_periph_push();
    p = last_push();
    CHECK(p && p->ledmode == 1 && p->runeffect == 5 && p->fan_pct == 37,
            "on a STALE snapshot the LED mode/effect (%u/%u) and the fan duty "
            "(%u%%) are still published — mm owns all three, so their validity "
            "does not depend on core answering an estats poll",
            p ? p->ledmode : 0, p ? p->runeffect : 0, p ? p->fan_pct : 0);

    tst_ledmode   = 0;
    tst_runeffect = 0;
    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    push_reset();
    mm_periph_push();
    CHECK(last_push()->ledmode == 0 && last_push()->runeffect == 0,
            "...and both track mm_led_state call for call, including back to 0 — "
            "the value is read each push, not latched");

    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    push_reset();
    tst_duty_pct = -1;
    mm_periph_push();
    tst_duty_pct = 0;
    mm_periph_push();
    tst_duty_pct = 100;
    mm_periph_push();
    CHECK(tst_pushes == 3 && tst_push[0].fan_pct == 0 && tst_push[1].fan_pct == 0 &&
                    tst_push[2].fan_pct == 100,
            "a FAILED duty read (-1) and a genuine 0%% both publish FanR 0, while "
            "100%% passes through — so FanR[0%%] does not distinguish 'no fan "
            "control' from 'fan commanded off', unlike hb_count three lines below "
            "it (saw %u/%u/%u)",
            tst_push[0].fan_pct, tst_push[1].fan_pct, tst_push[2].fan_pct);

    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    tst_duty_pct = 30;
    push_reset();
    tst_now     = 0;
    tst_pass    = 0;
    tst_want    = 8;
    tst_sleeps  = 0;
    tst_runaway = 0;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    g_run               = 1;
    mm_miner_main_unused();

    CHECK(!tst_runaway && tst_pushes == 8,
            "the supervisor pushes once per ~1 s iteration — 8 passes, 8 snapshots "
            "(saw %d); the app re-reads estats right after every ascset, so this "
            "cadence is what bounds how stale that readback can be",
            tst_pushes);
    {
        int all_valid = 1, all_counted = 1;
        for (int i = 0; i < tst_pushes; i++) {
            if (!tst_push[i].valid)
                all_valid = 0;
            if (tst_push[i].hb_count != HBOARD_COUNT)
                all_counted = 0;
        }
        CHECK(all_valid && all_counted,
                "...and every one of them is a complete snapshot (valid, %d "
                "board(s)) rather than a delta — core memcpy's the whole struct, "
                "so a push that omitted a field would blank it",
                (int)HBOARD_COUNT);
    }

    CHECK(!strcmp(startup_banner, "Mac 00:11:22:33:44:55"),
            "startup banner carries the colon-separated MAC");
    filter_value = "1200";
    tst_now      = 100;
    filtermesh_load();
    CHECK(g_filtermesh_time == 1200, "filter minutes restore from misc/filtermesh");
    int stages = filter_stages;
    filtermesh_rule(NULL, 0);
    CHECK(g_filtermesh_time == 1201, "each rule call adds one minute");
    CHECK(filter_stages == stages, "a minute off the hour is not staged");
    for (int i = 0; i < 58; i++)
        filtermesh_rule(NULL, 0);
    CHECK(g_filtermesh_time == 1259 && filter_stages == stages,
            "the run time is not staged before the next multiple of 60");
    filtermesh_rule(NULL, 0);
    CHECK(g_filtermesh_time == 1260 && !strcmp(filter_saved, "1260") && filter_stages == stages + 1,
            "the run time is staged at a multiple of 60");
    filtermesh_rule(NULL, 0);
    int nput = filter_puts;
    CHECK(filtermesh_keep() == 0 && filter_puts == nput + 1 && filter_stages == stages + 1 &&
                    !strcmp(filter_saved, "1261"),
            "keep writes the run time through put, which ignores the factory freeze");
    filter_value        = NULL;
    filter_legacy_value = "4321";
    filtermesh_load();
    CHECK(g_filtermesh_time == 0, "the legacy syscfg/filtermesh_time key is ignored");
    filter_legacy_value = NULL;
    filter_value        = "abc";
    filtermesh_load();
    CHECK(g_filtermesh_time == 0, "a non-numeric runtime loads as 0");
    filter_value = "-1";
    filtermesh_load();
    CHECK(g_filtermesh_time == 0, "invalid negative runtime is rejected");
    filter_value = "4294967297";
    filtermesh_load();
    CHECK(g_filtermesh_time == 0, "overflowing runtime is rejected");
    filter_value = "4294967294";
    filtermesh_load();
    filtermesh_rule(NULL, 0);
    filtermesh_rule(NULL, 0);
    CHECK(g_filtermesh_time == UINT32_MAX, "runtime saturates rather than wrapping");
    g_filtermesh_time = 54321;
    mm_periph_push();
    CHECK(last_push()->filtermesh == 54321, "filter-mesh minutes ride mm_periph to core");
    filtermesh_clean();
    CHECK(g_filtermesh_time == 0, "filter-clean resets the run-minute counter");
    CHECK(!strcmp(filter_saved, "0"), "filter-clean stores the reset");
    g_filtermesh_time = 0;
    g_softoff_rsn     = MC_SOFTOFF_ASIC_OVERHOT;
    g_fault_off       = 0;
    atomic_store(&g_fan_dev_ok, 1);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_OVERHOT),
            "recovered thermal fault clears live error, not history");
    CHECK(last_status.state == MC_WORK_INIT, "history does not keep live state idle");
    g_softonoff.api_softoff = 1;
    mm_status_push();
    CHECK(last_status.state == MC_WORK_IDLE,
            "API soft-off publishes idle without a historical reason");
    g_softonoff.api_softoff   = 0;
    g_softonoff.thermal_until = 1000;
    mm_status_push();
    CHECK(g_live_ecmm & ERR_MM_OVERHOT, "active thermal hold reports OVERHOT");
    CHECK(overhot_visible, "thermal hold displays LCD E-05");
    int updates = overhot_updates;
    mm_status_push();
    CHECK(overhot_updates == updates, "unchanged thermal hold does not refill UI queue");
    g_startup_errors = 0;
    g_softoff_rsn    = MC_SOFTOFF_POWER_FAULT;
    g_fault_off      = 1;
    atomic_store(&g_fan_dev_ok, 0);
    g_filtermesh_time = FILTERMESH_TIME_MAX;
    mm_status_push();
    /* the network bits follow the host's interfaces here; test_mm_pool_gate covers them */
    CHECK((g_live_ecmm & ~(ERR_MM_LAN_DAMAGED | ERR_MM_IPLOST)) ==
                    (ERR_MM_OVERHOT | ERR_MM_POWER | ERR_MM_FAN),
            "independent live faults coexist; a dirty filter mesh raises no ECMM bit");
    CHECK(visible[UI_FAN_ERR] && visible[UI_FILTER_DIRTY],
            "fan and filter faults trigger their LCD notices");
    CHECK(!visible[UI_POWER_ERR],
            "a runtime power fault raises ECMM only; the adapter screen is a startup verdict");
    int filter_updates = updates_by_id[UI_FILTER_DIRTY];
    mm_status_push();
    CHECK(updates_by_id[UI_FILTER_DIRTY] == filter_updates,
            "unchanged fault does not requeue popup");
    filtermesh_clean();
    mm_status_push();
    CHECK(!visible[UI_FILTER_DIRTY], "filter clean clears LCD notice");
    g_softonoff.thermal_until = 0;
    mm_status_push();
    CHECK(!overhot_visible, "thermal recovery clears LCD E-05");
    g_startup_errors = ERR_MM_FAN;
    atomic_store(&g_fan_dev_ok, 1);
    g_model_mismatch = 1;
    mm_status_push();
    CHECK((g_live_ecmm & (ERR_MM_FAN | ERR_MM_HW_UNMATCH)) == (ERR_MM_FAN | ERR_MM_HW_UNMATCH),
            "boot fan failure and model mismatch remain visible");
    push_reset();
    mm_periph_push();
    CHECK((last_push()->mm_errcode & g_live_ecmm) == g_live_ecmm,
            "live and startup errors use the existing MM wire field");
    g_fault_off            = 0;
    g_rail_off_unconfirmed = 1;
    mm_status_push();
    CHECK(g_live_ecmm & ERR_MM_POWER, "unconfirmed disable reports a live power error");
    CHECK(!visible[UI_POWER_ERR], "...without the startup-only adapter screen");
    g_rail_off_unconfirmed = 0;
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_POWER), "confirmed disable clears its live power error");
    g_fault_off = 0;
    g_startup_errors |= ERR_MM_POWER;
    g_power_notice = 1;
    mm_status_push();
    CHECK(visible[UI_POWER_ERR], "a startup power failure shows the LCD adapter screen");
    g_startup_errors &= ~ERR_MM_POWER;
    mm_status_push();
    CHECK(visible[UI_POWER_ERR] && !(g_live_ecmm & ERR_MM_POWER),
            "the adapter screen stays until reboot while the ECMM power bit clears");
    g_fault_off   = 1;
    g_softoff_rsn = MC_SOFTOFF_POWER_FAULT;
    mm_status_push();
    CHECK(!visible[UI_POWER_FAULT],
            "a startup power failure shows the adapter screen only, not also E-09");
    g_fault_off           = 0;
    g_softoff_rsn         = MC_SOFTOFF_NONE;
    g_power_notice        = 0;
    visible[UI_POWER_ERR] = 0;
    g_startup_errors      = 0;
    mm_status_push();

    static const struct {
        uint8_t  rsn;
        uint16_t popup;
        uint32_t ecmm;
    } stop_faults[] = {
        { MC_SOFTOFF_HASH_FAULT, UI_HASH_ERR, 0 },
        { MC_SOFTOFF_FAN_ERR, UI_FAN_STALL, ERR_MM_FAN },
        { MC_SOFTOFF_CALI_FAILED, UI_AGING_ERR, ERR_MM_AGING_FAILED },
        { MC_SOFTOFF_POWER_FAULT, UI_POWER_FAULT, ERR_MM_POWER },
    };
    for (unsigned i = 0; i < sizeof stop_faults / sizeof stop_faults[0]; i++) {
        g_fault_off   = 1;
        g_softoff_rsn = stop_faults[i].rsn;
        mm_status_push();
        CHECK(visible[stop_faults[i].popup], "a fault stop shows its LCD code");
        CHECK((g_live_ecmm & stop_faults[i].ecmm) == stop_faults[i].ecmm,
                "a fault stop raises its ECMM bit");
        for (unsigned j = 0; j < sizeof stop_faults / sizeof stop_faults[0]; j++)
            if (j != i)
                CHECK(!visible[stop_faults[j].popup], "...and no other fault stop code");
        CHECK(!visible[UI_POWER_ERR], "a fault stop does not show the startup adapter screen");
        CHECK(!visible[UI_FAN_ERR], "a fault stop does not show the fan controller screen");
        int stop_updates = updates_by_id[stop_faults[i].popup];
        mm_status_push();
        CHECK(updates_by_id[stop_faults[i].popup] == stop_updates,
                "an unchanged fault stop does not requeue its popup");
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
        mm_status_push();
        CHECK(!visible[stop_faults[i].popup], "ending the fault stop clears its code");
        CHECK(!(g_live_ecmm & ERR_MM_FAN), "ending the fault stop clears its ECMM bits");
    }
    g_softoff_rsn = MC_SOFTOFF_FAN_ERR;
    mm_status_push();
    CHECK(!visible[UI_FAN_STALL] && !(g_live_ecmm & ERR_MM_FAN),
            "a stale soft-off reason without a fault stop shows nothing");
    g_softoff_rsn = MC_SOFTOFF_NONE;

    atomic_store(&g_pool_inactive, 1);
    mm_status_push();
    CHECK(visible[UI_CHECK_POOL] && (g_live_ecmm & ERR_MM_POOL_INACTIVE),
            "an inactive pool shows the check-pool screen");
    atomic_store(&g_pool_inactive, 0);
    mm_status_push();
    CHECK(!visible[UI_CHECK_POOL], "a pool job clears the check-pool screen");
    atomic_store(&g_pool_unreached, 1);
    mm_status_push();
    CHECK(visible[UI_CHECK_POOL] && !(g_live_ecmm & ERR_MM_POOL_INACTIVE),
            "a pool never reached shows the check-pool screen without the ECMM bit");
    atomic_store(&g_pool_unreached, 0);
    mm_status_push();
    CHECK(!visible[UI_CHECK_POOL], "the first pool job clears the check-pool screen");

    g_ipc_asic_mm = (osif_ipc_t *)&tst_asic_tag;
    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);

    err_reset();
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == 0, "a normally running miner claims no ASIC error at all");

    err_reset();
    tst_asic_chips  = 0;
    tst_asic_boards = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        tst_chips[b] = 0;
    err_push();
    CHECK(echu_bits == ERR_HU_ENUM && (ecmm_bits & ERR_MM_SLAVE),
            "a live ASIC child reporting zero chips is a real enumeration failure "
            "and still raises ENUM + SLAVE");

    err_reset();
    tst_asic_connected     = 0;
    tst_asic_live          = 0;
    tst_asic_chips         = 0;
    g_softonoff.start_wait = 1;
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == 0,
            "the cold-start gate claims nothing: the ASIC child was never spawned");

    err_reset();
    tst_asic_connected        = 0;
    tst_asic_live             = 0;
    tst_asic_chips            = 0;
    g_softonoff.thermal_until = 1000;
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == 0,
            "a thermal soft-off claims nothing: the ASIC child is parked, not lost");

    err_reset();
    tst_asic_connected      = 0;
    tst_asic_live           = 0;
    tst_asic_chips          = 0;
    g_softonoff.api_softoff = 1;
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == 0,
            "an API soft-off claims nothing: the user parked the machine");

    err_reset();
    tst_asic_connected = 0;
    tst_asic_live      = 0;
    tst_asic_chips     = 0;
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == ERR_MM_INTERNUCLEAR,
            "a crashed ASIC child reports INTERNUCLEAR alone — no enumeration was seen, "
            "so none is claimed to have failed");

    err_reset();
    tst_asic_connected = 0;
    tst_asic_live      = 0;
    tst_asic_chips     = 0;
    g_fault_off        = 1;
    g_softoff_rsn      = MC_SOFTOFF_HASH_FAULT;
    err_push();
    CHECK(echu_bits == ERR_HU_ENUM && ecmm_bits == (ERR_MM_INTERNUCLEAR | ERR_MM_SLAVE),
            "a latched hash fault IS the enum-failed latch and keeps "
            "ENUM + INTERNUCLEAR + SLAVE");

    err_reset();
    tst_asic_connected = 0;
    tst_asic_live      = 0;
    tst_asic_chips     = 0;
    g_fault_off        = 1;
    g_softoff_rsn      = MC_SOFTOFF_POWER_FAULT;
    mm_status_push();
    err_push();
    CHECK(echu_bits == 0 && ecmm_bits == 0 && (last_push()->mm_errcode & ERR_MM_POWER),
            "an intentional power-fault stop reports POWER without INTERNUCLEAR or ENUM");

    g_ipc_asic_mm      = NULL;
    tst_asic_connected = 1;
    {
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
        telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
        tst_asic_boards = HBOARD_COUNT;

        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = MINER_MAX_CHIPS;
        push_reset();
        mm_periph_push();
        p       = last_push();
        int hot = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (p->hu_errcode[b] & ERR_HU_ENUM)
                hot++;
        CHECK(hot == 0,
                "a fully enumerated machine flags no board with ERR_HU_ENUM "
                "(%d of %d flagged)",
                hot, (int)HBOARD_COUNT);

        tst_chips[0] = 0;
        for (int b = 1; b < HBOARD_COUNT; b++)
            tst_chips[b] = MINER_MAX_CHIPS;
        push_reset();
        mm_periph_push();
        p          = last_push();
        int only_0 = (p->hu_errcode[0] & ERR_HU_ENUM) != 0;
        for (int b = 1; b < HBOARD_COUNT; b++)
            if (p->hu_errcode[b] & ERR_HU_ENUM)
                only_0 = 0;
        CHECK(only_0,
                "THE DEFECT: board 0 dead with the other %d hashing flags board 0 "
                "and ONLY board 0. The old arm derived one bit from the machine "
                "total (asic_chips <= 0), so this partial failure flagged NOTHING "
                "-- the operator saw three healthy boards and one third of the "
                "hashrate. It is invisible on a 1-board product, which is why it "
                "survived (hu_errcode now %u/%u)",
                (int)HBOARD_COUNT - 1, p->hu_errcode[0], p->hu_errcode[HBOARD_COUNT - 1]);

        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = 0;
        push_reset();
        mm_periph_push();
        p       = last_push();
        int all = 1;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (!(p->hu_errcode[b] & ERR_HU_ENUM))
                all = 0;
        CHECK(all,
                "a machine that enumerated nothing still flags every board -- the "
                "per-board arm never flags fewer boards than a machine-wide one");

        tst_asic_boards = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = 0;
        push_reset();
        mm_periph_push();
        p        = last_push();
        int none = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (p->hu_errcode[b] & ERR_HU_ENUM)
                none++;
        CHECK(none == 0,
                "NO asic snapshot at all (asic_boards 0) flags NOTHING (%d "
                "flagged): a board is only reported as failing to enumerate on an "
                "enumeration result that named it, which is core_miner's rule "
                "(board < g_chip.boards). A silent "
                "asic_miner is the watchdog feed gate's problem, "
                "not 3 boards of forged ENUM",
                none);

        tst_asic_boards = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = MINER_MAX_CHIPS;
    }

    {
        cali_param row = {
            .max_pout = 4001, .temp = 61, .volt = 2049, .pll_start = 313, .pll_interval = 7
        };
        tst_cali_override = 1;
        tst_cali_row      = row;
        tst_cali_aged     = 1;
        tst_workmode      = cali_mode_top(CALI_MODE_BOARD_MASK);
        tst_asic_boards   = HBOARD_COUNT;
        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = 1;

        tst_cali_calls    = 0;
        tst_cali_asic_cnt = 0;
        tst_cali_mode     = 0xFF;
        push_reset();
        mm_periph_push();
        p = last_push();

        CHECK(tst_cali_calls >= 1 && tst_cali_asic_cnt == g_board.max_asic,
                "mm resolves the published row on THIS BOARD'S OWN population "
                "(%u) — the same argument mm_fan and mm_vf actuate on — and "
                "never on a runtime enumeration count (%d board(s) each "
                "report 1 chip here; saw %u)",
                (unsigned)g_board.max_asic, (int)HBOARD_COUNT, (unsigned)tst_cali_asic_cnt);
        CHECK(tst_cali_mode == tst_workmode, "...at the persisted work mode (%u, saw %u)",
                (unsigned)tst_workmode, (unsigned)tst_cali_mode);
        CHECK(p && p->cali_max_pout == 4001 && p->cali_temp == 61 && p->cali_volt == 2049 &&
                        p->cali_pll_start == 313 && p->cali_pll_interval == 7 && p->cali_aged == 1,
                "...and all five fields plus the aged flag ride mm_periph to "
                "core, which renders MPO[]/TarT[]/ATA<n>[] from them instead of "
                "asking the cali table itself");

        tst_cali_aged = 0;
        push_reset();
        mm_periph_push();
        p = last_push();
        CHECK(p && p->cali_aged == 0,
                "the aged flag on the wire is mm's own resolve result, not a "
                "constant — it is the input core turns into ADJ[]");

        CHECK(p && p->workmode == tst_workmode,
                "the persisted work mode rides mm_periph, so core renders "
                "WORKMODE[] from the snapshot instead of reading "
                "systemcfg.ini itself (want %u, saw %u)",
                (unsigned)tst_workmode, (unsigned)(p ? p->workmode : 0));

        tst_cali_get_mask = 0;
        push_reset();
        mm_periph_push();
        p = last_push();
        CHECK(p && p->caliall == 0,
                "CALIALL is 0 when hashrate_cali.ini holds no valid record for "
                "any mode (saw 0x%02x)",
                (unsigned)(p ? p->caliall : 0));

        tst_cali_get_mask = 0x02;
        push_reset();
        mm_periph_push();
        p = last_push();
        CHECK(p && p->caliall == (0x02 & CALI_MODE_BOARD_MASK),
                "...and one bit per board mode that has one — mm walks the ini, "
                "core does not (saw 0x%02x)",
                (unsigned)(p ? p->caliall : 0));

        tst_cali_get_mask = (1u << CALI_MODE_MAX) - 1u;
        push_reset();
        mm_periph_push();
        p = last_push();
        CHECK(p && p->caliall == CALI_MODE_BOARD_MASK,
                "[%s] a record for a mode outside BOARD_WORKMODE_RANGES is not "
                "reported (want 0x%02x, saw 0x%02x)",
                BOARDCONF_SECTION, CALI_MODE_BOARD_MASK, (unsigned)(p ? p->caliall : 0));
        tst_cali_get_mask = 0x02;

        tst_aging_finished = 1;
        push_reset();
        mm_periph_push();
        p = last_push();
        CHECK(p && p->caliall == (uint8_t)((0x02 & CALI_MODE_BOARD_MASK) | (1u << tst_workmode)),
                "...plus the selected mode's own bit once aging finished, taken "
                "from the SAME snapshot that carries aging_finished, so the two "
                "can never disagree (saw 0x%02x)",
                (unsigned)(p ? p->caliall : 0));
        tst_aging_finished = 0;
        tst_cali_get_mask  = 0;

        tst_cali_override = 0;
        tst_workmode      = 0;
        tst_asic_boards   = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            tst_chips[b] = MINER_MAX_CHIPS;
    }
    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);
    push_reset();
    mm_periph_push();
    CHECK(!(last_push()->hu_errcode[0] & ERR_HU_ENUM),
            "local sensor polling alone cannot declare ASIC enumeration failure");
    tst_outlet[0] = MCU_NONE;
    push_reset();
    mm_periph_push();
    CHECK(!(last_push()->hu_errcode[0] & 0x0008u) && !(last_push()->mm_errcode & ERR_MM_SLAVE),
            "a failed outlet read reports no MCU bit on any product");
    telem_set(1, 0, MCU_NONE, MCU_NONE);
    push_reset();
    mm_periph_push();
    CHECK(!(last_push()->hu_errcode[0] & 0x0008u), "a missing MCU bus reports no MCU bit either");
    telem_set(1, HBOARD_COUNT, HOT_OUT, HOT_IN);

    g_ipc_asic_mm = (osif_ipc_t *)&tst_asic_tag;
    atomic_store(&g_asic_spawn_at, 1);
    tst_asic_connected = 0;
    push_reset();
    mm_periph_push();
    CHECK(last_push()->mm_errcode & ERR_MM_INTERNUCLEAR,
            "a started asic_miner that has not connected yet reports INTERNUCLEAR");
    tst_asic_connected = 1;
    push_reset();
    mm_periph_push();
    CHECK(!(last_push()->mm_errcode & ERR_MM_INTERNUCLEAR),
            "its first connection clears INTERNUCLEAR");
    tst_asic_connected = 0;
    push_reset();
    mm_periph_push();
    CHECK(last_push()->mm_errcode & ERR_MM_INTERNUCLEAR,
            "a later disconnect reports INTERNUCLEAR as a lost link");
    g_softonoff.api_softoff = 1;
    push_reset();
    mm_periph_push();
    CHECK(!(last_push()->mm_errcode & ERR_MM_INTERNUCLEAR),
            "a deliberately stopped ASIC is not waiting to connect");
    g_softonoff.api_softoff = 0;
    atomic_store(&g_asic_spawn_at, 0);
    push_reset();
    mm_periph_push();
    CHECK(last_push()->mm_errcode & ERR_MM_INTERNUCLEAR,
            "an ASIC that was never started reports only its link down");
    tst_asic_connected = 1;
    g_ipc_asic_mm      = NULL;

    return tst_done();
}
