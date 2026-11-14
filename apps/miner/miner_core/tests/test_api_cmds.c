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

#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "mc_api_lcd.h"
#include "mc_api_identity.h"
#include "boardconf.h"
#include "core_board.h"
#include "tst_board_expect.h"
#include "tst_stats_json.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WANT_DESC "AvalonMiner 0.0.0" /* Description field expected in every reply STATUS section */
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            fails++; \
        } else { \
            printf("  ok: " __VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define SPEC_MAX      128 /* most members one stats JSON block is expected to hold */
#define VERSION_KEY_N ((int)(sizeof VERSION_KEYS / sizeof *VERSION_KEYS))
#define TEXT_FW       "FW[" FW_TYPE "] "
#define TEXT_PVER     "PVer[" MINER_PROTO_VER "] "

/* SSID with a backslash, a quote and a raw BEL byte */
#define HOSTILE_SSID \
    "my\\x4eet\"" \
    "\007" \
    "AP"

/* Model string carrying the API field and section separators */
#define HOSTILE_MODEL "Nano3s,MAC=deadbeef|FORGED,x=\"y\\z"
/* HOSTILE_MODEL as the API reports it, separators replaced by '?' */
#define HOSTILE_MODEL_SAFE "Nano3s?MAC=deadbeef?FORGED?x=?y?z"
/* Product string carrying the API field and section separators */
#define HOSTILE_PROD "Avalon,UPAPI=9|X"
/* HOSTILE_PROD as the API reports it, separators replaced by '?' */
#define HOSTILE_PROD_SAFE "Avalon?UPAPI=9?X"

/* Member a stats JSON block is expected to hold */
typedef struct {
    char key[SJ_KEY_CAP];
    char type; /* sj_type() letter */
    int  len;  /* array length, -1 when not an array */
} sj_spec;

/* json_str_value() results; JSTR_N is their count */
enum {
    JSTR_OK = 0,
    JSTR_NO_KEY,
    JSTR_RAW_CTRL,
    JSTR_BAD_ESCAPE,
    JSTR_UNTERMINATED,
    JSTR_OVERFLOW,
    JSTR_N
};

static int fails;

/* Text replies of fixed_stats() without TEXT_PVER as FNV-1a 64-bit digests, per descriptor and
 * CORE_PUB_LCD (the _lcdoff builds drop LcdOnoff/LcdSwitch/LcdBright from the reply) */
static const struct {
    const char *section;
    int         lcd;
    uint64_t    estats, estats_all, stats, litestats;
} TEXT_DIGEST[] = {
    { "NANO3S", 1, 0xeab52d111144b995ull, 0xf8b6b7810ef9f062ull, 0xf72f79fde82195b4ull,
            0x25dc8265d83c0c0cull },
    { "NANO3S", 0, 0xfb885f88dc3ec503ull, 0xe02f172d432f1698ull, 0x6c230ce8ac9cf1b6ull,
            0xe8f610aece9603adull },
    { "X3", 1, 0x860c93ccba865c15ull, 0x2f9c7c4df4d96835ull, 0xd88c61b5f2742534ull,
            0xd96c7da518364356ull },
    { "X3", 0, 0xcaed62f62af5147dull, 0x5f1cc970f2bba93dull, 0x6901e09e121436bcull,
            0xad3bd11637b756cfull },
};

/* Keys of the version reply, in reply order */
static const char *const VERSION_KEYS[] = { "AvalonMiner", "API", "PROD", "MODEL", "HWTYPE",
    "SWTYPE", "FWVERSION", "DNA", "MAC", "UPAPI" };

/* Text for each json_str_value() result */
static const char *const JSTR_WHY[JSTR_N] = {
    "ok",
    "the key is not in the reply",
    "a RAW control byte inside the string value",
    "a backslash escape json_escape cannot emit",
    "the string value never closes",
    "the decoded value does not fit the reader's buffer",
};

static mc_api_stats fixed_stats(void)
{
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.model        = "Nano3s";
    s.hwtype       = "N_MM1v1_X1";
    s.swtype       = "MM319";
    s.version      = "26070301_649070d";
    strcpy(s.mm_version, "26070302_abcdef0");
    s.dna             = "0201000012345678";
    s.mac             = "020000000001";
    s.when            = 4133982044u;
    s.elapsed         = 565;
    s.dev_elapsed     = 539.0;
    s.accepted        = 37;
    s.rejected        = 0;
    s.hw_errors       = 0;
    s.discarded       = 0;
    s.mhs_av          = 5693980.85;
    s.mhs_5m          = 5892960.0;
    s.diff_accepted   = 671744.0;
    s.best_share      = 296878;
    s.last_valid_work = 4133982044u;
    s.local_work      = 6693;
    s.ping_ms         = 47;
    s.workmode        = 2;
    s.memfree_kb      = 39004;
    s.mm_local_work   = 101467;
    s.ledmode         = 0;
    s.led_effect      = 1;
    s.led_bright      = 100;
    s.led_temper      = 50;
    s.led_rgb         = 255;
    s.led_runeffect   = 1;
    if (CORE_PUB_LCD) {
        s.lcd_onoff = 1;
        s.lcd_show  = 1;
    }
    if (CORE_PUB_AUDIO)
        s.volume = 100;
    s.max_pout          = 133;
    s.target_temp       = 90;
    s.cali_volt         = 3450;
    s.cali_pll_start    = 420;
    s.cali_pll_interval = 20;
    s.asc_count         = 1;
    s.pool_count        = 3;
    s.npools            = 1;
    s.diff1             = 749568.0;
    s.pool[0]           = (mc_api_pool){ .id = 0,
                  .url                       = "stratum+tcp://btc.f2pool.com:3333",
                  .status                    = "Alive",
                  .user                      = "poolworker.001",
                  .accepted                  = 37,
                  .rejected                  = 0,
                  .stratum_active            = 1,
                  .diff1_shares              = 749568.0,
                  .last_share_time           = 4133982044u };
    s.diff1_board[0]    = 749568.0;
    s.telem             = (mc_api_telem){ .valid = 1,
                    .in_work                     = 1,
                    .work_state                  = MC_WORK_WORK,
                    .freq_mhz                    = 416,
                    .ghs_mm                      = 6067.10,
                    .temp_max                    = 92,
                    .temp_avg                    = 89,
                    .board_temp                  = 68,
                    .ps                          = { 0, 0, 27581, 4, 0, 3574, 132 },
                    .fan_pct                     = 31,
                    .bin_grade                   = 47 };
    for (int f = 0; f < FAN_COUNT; f++)
        s.telem.fan_rpm[f] = 1460 + 10 * f;
    s.telem.hb_count = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        s.telem.hb_outlet[b] = 43;
        s.telem.hb_inlet[b]  = 41;
    }
    s.telem.board_n = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        s.telem.chip_n[b] = MINER_MAX_CHIPS;
        for (int c = 0; c < MINER_MAX_CHIPS; c++) {
            s.telem.chip_pass[b][c]  = 1000000;
            s.telem.chip_fail[b][c]  = 5000;
            s.telem.chip_pvt_t[b][c] = (int16_t)(b || c ? 89 : 92);
        }
    }
    s.activation         = 1;
    s.bootby_reason      = 0x03;
    s.bootby_sub         = 0;
    s.telem.ghs_spd_chip = 5000.0;
    s.block_hash         = "00000000000000000001cb4150b955d47fd97be3d968826bb45111a859f51935";
    s.net_difficulty     = 133869853540305.39;
    return s;
}

static int first_key_out_of_place(const char *from, const char *to, const char *pre,
        const char *post)
{
    const char *p = from;
    int         i;

    for (i = 0; i < VERSION_KEY_N; i++) {
        char        pat[48];
        const char *hit;

        snprintf(pat, sizeof pat, "%s%s%s", pre, VERSION_KEYS[i], post);
        hit = strstr(p, pat);
        if (!hit || hit >= to)
            return i;
        p = hit + strlen(pat);
    }
    return -1;
}

static int count_sub(const char *from, const char *to, const char *needle)
{
    size_t n    = strlen(needle);
    int    hits = 0;

    for (; from + n <= to; from++)
        if (!strncmp(from, needle, n))
            hits++;
    return hits;
}

static const char *jstr_why(int rc)
{
    return (rc >= 0 && rc < JSTR_N) ? JSTR_WHY[rc] : "UNKNOWN reader code";
}

static int jstr_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int json_str_value(const char *reply, const char *key, char *out, size_t cap,
        const char **after)
{
    char        pat[32];
    const char *p;
    size_t      n = 0;

    out[0] = '\0';
    if (after)
        *after = NULL;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(reply, pat);
    if (!p)
        return JSTR_NO_KEY;
    p += strlen(pat);

    for (;;) {
        unsigned char c = (unsigned char)*p;

        if (c == '\0')
            return JSTR_UNTERMINATED;
        if (c == '"') {
            p++;
            break;
        }
        if (c < 0x20)
            return JSTR_RAW_CTRL;
        if (c == '\\') {
            if (p[1] == '"' || p[1] == '\\') {
                c = (unsigned char)p[1];
                p += 2;
            } else if (p[1] == 'u') {
                int v = 0, k;
                for (k = 0; k < 4; k++) {
                    int h = jstr_hex(p[2 + k]);
                    if (h < 0)
                        return JSTR_BAD_ESCAPE;
                    v = (v << 4) | h;
                }
                if (v > 0xff)
                    return JSTR_BAD_ESCAPE;
                c = (unsigned char)v;
                p += 6;
            } else {
                return JSTR_BAD_ESCAPE;
            }
        } else {
            p++;
        }
        if (n + 1 >= cap)
            return JSTR_OVERFLOW;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (after)
        *after = p;
    return JSTR_OK;
}

static int mm_id0_ssid(const char *mm, char *out, size_t cap)
{
    const char *a = strstr(mm, "SSID[");
    const char *b;
    size_t      n;

    out[0] = '\0';
    if (!a)
        return -1;
    a += 5;
    b = strstr(a, "] RSSI[");
    if (!b)
        return -1;
    n = (size_t)(b - a);
    if (n + 1 > cap)
        return -1;
    memcpy(out, a, n);
    out[n] = '\0';
    return 0;
}

static int spec_add(sj_spec *sp, int n, const char *key, char type, int len)
{
    if (n >= SPEC_MAX)
        return n;
    snprintf(sp[n].key, sizeof sp[n].key, "%s", key);
    sp[n].type = type;
    sp[n].len  = len;
    return n + 1;
}

static int sysinfo_spec(sj_spec *sp)
{
    char k[SJ_KEY_CAP];
    int  n = 0;

    n = spec_add(sp, n, "Ver", 's', -1);
    n = spec_add(sp, n, "LVer", 's', -1);
    n = spec_add(sp, n, "FW", 's', -1);
    n = spec_add(sp, n, "PVer", 's', -1);
    if (CORE_PUB_LED) {
        n = spec_add(sp, n, "LED", 's', -1);
        n = spec_add(sp, n, "LEDUser", 's', -1);
    }
    if (CORE_PUB_LCD) {
        n = spec_add(sp, n, "LcdOnoff", 'n', -1);
        n = spec_add(sp, n, "LcdSwitch", 'n', -1);
    }
    if (CORE_PUB_AUDIO)
        n = spec_add(sp, n, "Volume", 'n', -1);
    n = spec_add(sp, n, "DNA", 's', -1);
    n = spec_add(sp, n, "MEMFREE", 'n', -1);
    n = spec_add(sp, n, "PFCnt", 'n', -1);
    n = spec_add(sp, n, "NETFAIL", 'a', 8);
    n = spec_add(sp, n, "SYSTEMSTATU", 's', -1);
    n = spec_add(sp, n, "Elapsed", 'n', -1);
    n = spec_add(sp, n, "BOOTBY", 's', -1);
    n = spec_add(sp, n, "ITemp", 'n', -1);
    n = spec_add(sp, n, "OTemp", 'n', -1);
    for (int f = 0; f < FAN_COUNT; f++) {
        snprintf(k, sizeof k, "Fan%d", f + 1);
        n = spec_add(sp, n, k, 'n', -1);
    }
    n = spec_add(sp, n, "FanR", 'n', -1);
    n = spec_add(sp, n, "PS", 'a', 7);
    n = spec_add(sp, n, "Activation", 'n', -1);
    n = spec_add(sp, n, "PING", 'n', -1);
    n = spec_add(sp, n, "POOLS", 'n', -1);
    n = spec_add(sp, n, "SoftOFF", 'n', -1);
    return spec_add(sp, n, "ECMM", 'n', -1);
}

static int hashinfo_spec(sj_spec *sp, const mc_api_stats *s)
{
    const int hb_n = s->telem.hb_count < HBOARD_COUNT ? s->telem.hb_count : HBOARD_COUNT;
    char      k[SJ_KEY_CAP];
    int       n = 0;

    n = spec_add(sp, n, "LW", 'n', -1);
    n = spec_add(sp, n, "MH", 'a', HBOARD_COUNT);
    n = spec_add(sp, n, "DHW", 'n', -1);
    n = spec_add(sp, n, "HW", 'n', -1);
    n = spec_add(sp, n, "DH", 'n', -1);
    if (hb_n > 0) {
        n = spec_add(sp, n, "HBITemp", 'a', hb_n);
        n = spec_add(sp, n, "HBOTemp", 'a', hb_n);
    }
    n = spec_add(sp, n, "TMax", 'n', -1);
    n = spec_add(sp, n, "TAvg", 'n', -1);
    n = spec_add(sp, n, "TarT", 'n', -1);
    n = spec_add(sp, n, "GHSspd", 'n', -1);
    n = spec_add(sp, n, "DHspd", 'n', -1);
    n = spec_add(sp, n, "GHSmm", 'n', -1);
    n = spec_add(sp, n, "GHSavg", 'n', -1);
    n = spec_add(sp, n, "WU", 'n', -1);
    n = spec_add(sp, n, "Freq", 'n', -1);
    n = spec_add(sp, n, "MGHS", 'a', HBOARD_COUNT);
    n = spec_add(sp, n, "MTmax", 'a', HBOARD_COUNT);
    n = spec_add(sp, n, "MTavg", 'n', -1);
    n = spec_add(sp, n, "TA", 'n', -1);
    n = spec_add(sp, n, "Core", 's', -1);
    n = spec_add(sp, n, "BIN", 'n', -1);
    n = spec_add(sp, n, "ECHU", 'a', HBOARD_COUNT);
    for (int b = 0; b < HBOARD_COUNT; b++) {
        snprintf(k, sizeof k, "PLL%d", b);
        n = spec_add(sp, n, k, 'a', ASIC_PLL_POINTS);
        snprintf(k, sizeof k, "SF%d", b);
        n = spec_add(sp, n, k, 'a', ASIC_PLL_POINTS);
    }
    n = spec_add(sp, n, "CRC", 'a', HBOARD_COUNT);
    n = spec_add(sp, n, "COMCRC", 'a', HBOARD_COUNT);
    snprintf(k, sizeof k, "ATA%d", s->workmode);
    n = spec_add(sp, n, k, 's', -1);
    n = spec_add(sp, n, "WORKMODE", 'n', -1);
    n = spec_add(sp, n, "WORKLEVEL", 'n', -1);
    n = spec_add(sp, n, "MPO", 'n', -1);
    n = spec_add(sp, n, "CALIALL", 'n', -1);
    n = spec_add(sp, n, "ADJ", 'n', -1);
    if (s->adj != 1)
        n = spec_add(sp, n, "BAR", 'n', -1);
    return n;
}

static int hb_spec(sj_spec *sp, int b, int nc, int show_all)
{
    char k[SJ_KEY_CAP];
    int  n = 0;

    snprintf(k, sizeof k, "PVT_T%d", b);
    n = spec_add(sp, n, k, 'a', nc);
    snprintf(k, sizeof k, "PVT_V%d", b);
    n = spec_add(sp, n, k, 'a', nc);
    snprintf(k, sizeof k, "MW%d", b);
    n = spec_add(sp, n, k, 'a', nc);
    if (!show_all)
        return n;
    for (int l = 0; l < ASIC_PLL_POINTS; l++) {
        snprintf(k, sizeof k, "CF%d_%d", b, l);
        n = spec_add(sp, n, k, 'a', nc);
    }
    for (int l = 0; l < ASIC_PLL_POINTS; l++) {
        snprintf(k, sizeof k, "PLLCNT%d_%d", b, l);
        n = spec_add(sp, n, k, 'a', nc);
    }
    snprintf(k, sizeof k, "ERATIO%d", b);
    n = spec_add(sp, n, k, 'a', nc);
    snprintf(k, sizeof k, "C_%02d_00", b);
    n = spec_add(sp, n, k, 'a', nc);
    snprintf(k, sizeof k, "C_%02d_01", b);
    n = spec_add(sp, n, k, 'a', nc);
    snprintf(k, sizeof k, "GHSmm%02d", b);
    return spec_add(sp, n, k, 'a', nc);
}

static int lite_spec(sj_spec *sp)
{
    int n = 0;

    n = spec_add(sp, n, "LVer", 's', -1);
    n = spec_add(sp, n, "FW", 's', -1);
    n = spec_add(sp, n, "PVer", 's', -1);
    n = spec_add(sp, n, "STATE", 'n', -1);
    n = spec_add(sp, n, "MODE", 'n', -1);
    n = spec_add(sp, n, "LEVEL", 'n', -1);
    n = spec_add(sp, n, "GHSspd", 'n', -1);
    n = spec_add(sp, n, "ITemp", 'n', -1);
    n = spec_add(sp, n, "OTemp", 'n', -1);
    n = spec_add(sp, n, "Limitemp", 'n', -1);
    n = spec_add(sp, n, "FanR", 'n', -1);
    n = spec_add(sp, n, "Filter", 'n', -1);
    if (CORE_PUB_LED) {
        n = spec_add(sp, n, "LED", 's', -1);
        n = spec_add(sp, n, "LEDUser", 's', -1);
    }
    if (CORE_PUB_LCD) {
        n = spec_add(sp, n, "LcdOnoff", 'n', -1);
        n = spec_add(sp, n, "LcdSwitch", 'n', -1);
        n = spec_add(sp, n, "LcdBright", 'n', -1);
    }
    if (CORE_PUB_AUDIO)
        n = spec_add(sp, n, "Volume", 'n', -1);
    n = spec_add(sp, n, "Activation", 'n', -1);
    n = spec_add(sp, n, "ECHU", 'a', HBOARD_COUNT);
    n = spec_add(sp, n, "ECMM", 'n', -1);
    n = spec_add(sp, n, "SSID", 's', -1);
    n = spec_add(sp, n, "RSSI", 'n', -1);
    return spec_add(sp, n, "NetDevType", 'n', -1);
}

static int block_matches(const char *reply, const char *block, const sj_spec *sp, int n, char *why,
        size_t cap)
{
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    int         got = sj_keys(reply, block, keys, SJ_KEYS_MAX);

    snprintf(why, cap, "ok");
    if (got < 0) {
        snprintf(why, cap, "no block \"%s\"", block);
        return 0;
    }
    for (int i = 0; i < n || i < got; i++) {
        if (i >= n || i >= got || strcmp(keys[i], sp[i].key)) {
            snprintf(why, cap, "member %d is \"%s\", expected \"%s\"", i,
                    i < got ? keys[i] : "(none)", i < n ? sp[i].key : "(none)");
            return 0;
        }
        if (!sj_in(reply, sp[i].key, block) || sj_type(reply, sp[i].key) != sp[i].type ||
                (sp[i].len >= 0 && sj_array_len(reply, sp[i].key) != sp[i].len)) {
            snprintf(why, cap, "\"%s\" is type '%c' len %d, expected '%c' len %d", sp[i].key,
                    sj_type(reply, sp[i].key), sj_array_len(reply, sp[i].key), sp[i].type,
                    sp[i].len);
            return 0;
        }
    }
    return 1;
}

static int members_are(const char *obj, const char *const *want, int n)
{
    static char keys[SJ_KEYS_MAX][SJ_KEY_CAP];
    int         got = obj ? sj_members(obj, keys, NULL, SJ_KEYS_MAX) : -1;

    if (got != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(keys[i], want[i]))
            return 0;
    return 1;
}

static void check_estats_shape(const mc_api_stats *s, const char *what)
{
    static const char *const RECORD[]  = { "STATS", "ID", "Elapsed", "Calls", "Wait", "Max", "Min",
         "MMID0Summary", "HBinfo", "MM Count", "Nonce Mask" };
    static const char *const SUMMARY[] = { "Sysinfo", "Hashinfo" };
    static char              buf[MC_API_REPLY_CAP], text[MC_API_REPLY_CAP];
    static sj_spec           sp[SPEC_MAX];
    char                     why[128], hbk[HBOARD_COUNT][SJ_KEY_CAP];
    const char              *hbv[HBOARD_COUNT];
    int                      n;

    for (int b = 0; b < HBOARD_COUNT; b++) {
        snprintf(hbk[b], sizeof hbk[b], "HB_%d_Info", b);
        hbv[b] = hbk[b];
    }
    for (int show_all = 0; show_all <= 1; show_all++) {
        const char *sfx = show_all ? "|all" : "";

        mc_api_stats_(s, buf, sizeof buf, show_all, 0);
        CHECK(sj_ok(buf),
                "%s estats%s JSON is one well-formed document: STATS[0].MMID0Summary is "
                "an object, not a string, and no \"MM ID0\" remains",
                what, sfx);
        CHECK(members_are(sj_record(buf), RECORD, (int)(sizeof RECORD / sizeof *RECORD)),
                "%s estats%s STATS[0] keeps its record fields, MMID0Summary and HBinfo "
                "in the place of MM ID0",
                what, sfx);
        CHECK(members_are(sj_block_obj(buf, ""), SUMMARY, 2),
                "%s estats%s MMID0Summary holds Sysinfo then Hashinfo", what, sfx);
        CHECK(members_are(sj_member(sj_record(buf), "HBinfo"), hbv, HBOARD_COUNT),
                "%s estats%s HBinfo holds HB_0_Info..HB_%d_Info", what, sfx, HBOARD_COUNT - 1);
        n = sysinfo_spec(sp);
        CHECK(block_matches(buf, "Sysinfo", sp, n, why, sizeof why),
                "%s estats%s Sysinfo key order, types and sizes (%s)", what, sfx, why);
        n = hashinfo_spec(sp, s);
        CHECK(block_matches(buf, "Hashinfo", sp, n, why, sizeof why),
                "%s estats%s Hashinfo key order, types and sizes (%s)", what, sfx, why);
        for (int b = 0; b < HBOARD_COUNT; b++) {
            n = hb_spec(sp, b, s->telem.chip_n[b], show_all);
            CHECK(block_matches(buf, hbk[b], sp, n, why, sizeof why),
                    "%s estats%s %s key order, types and sizes (%s)", what, sfx, hbk[b], why);
        }
        mc_api_stats_text(s, text, sizeof text, show_all, 0);
        CHECK(sj_same_as_text(buf, text),
                "%s estats%s JSON carries every MM ID0 field of the text reply with the "
                "same value, and no other",
                what, sfx);
    }
    mc_api_stats_(s, buf, sizeof buf, 0, 1);
    mc_api_stats_text(s, text, sizeof text, 0, 1);
    CHECK(sj_ok(buf) && sj_same_as_text(buf, text),
            "%s stats JSON carries the same MMID0Summary as estats, every text field with "
            "its value",
            what);
}

static void check_stats_json_shape(void)
{
    static const char *const LITE_RECORD[] = { "MMID0Summary", "MM Count", "Nonce Mask" };
    static char              buf[MC_API_REPLY_CAP], text[MC_API_REPLY_CAP];
    static sj_spec           sp[SPEC_MAX];
    mc_api_stats             s = fixed_stats();
    char                     why[128];
    int                      n;

    check_estats_shape(&s, "fixed");
    s.telem.hb_count  = 0;
    s.adj             = 1;
    s.telem.board_n   = 1;
    s.telem.chip_n[0] = 2;
    for (int b = 1; b < HBOARD_COUNT; b++)
        s.telem.chip_n[b] = 0;
    check_estats_shape(&s, "no-MCU calibrated two-chip");

    s = fixed_stats();
    snprintf(s.ssid, sizeof s.ssid, "%s", "myssid");
    mc_api_litestats_(&s, buf, sizeof buf);
    CHECK(sj_ok(buf),
            "litestats JSON is one well-formed document: STATS[0].MMID0Summary is an "
            "object, not a string, and no \"MM ID0\" remains");
    CHECK(members_are(sj_record(buf), LITE_RECORD, 3),
            "litestats STATS[0] is MMID0Summary, MM Count, Nonce Mask");
    n = lite_spec(sp);
    CHECK(block_matches(buf, "", sp, n, why, sizeof why),
            "litestats MMID0Summary flat key order, types and sizes (%s)", why);
    mc_api_litestats_text(&s, text, sizeof text);
    CHECK(sj_same_as_text(buf, text),
            "litestats JSON carries every MM ID0 field of the text reply with the same value");
}

static uint64_t fnv64(const char *p)
{
    uint64_t h = 0xcbf29ce484222325ull;

    for (; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t text_digest(char *buf, const char *reply)
{
    char *p = strstr(buf, TEXT_FW TEXT_PVER);

    CHECK(p && !strstr(p + strlen(TEXT_FW TEXT_PVER), TEXT_PVER),
            "text %s carries " TEXT_PVER "once, right after FW", reply);
    if (p) {
        p += strlen(TEXT_FW);
        memmove(p, p + strlen(TEXT_PVER), strlen(p + strlen(TEXT_PVER)) + 1);
    }
    return fnv64(buf);
}

static void check_text_unchanged(void)
{
    static char  buf[MC_API_REPLY_CAP];
    mc_api_stats s = fixed_stats();
    int          i, n = (int)(sizeof TEXT_DIGEST / sizeof *TEXT_DIGEST);

    for (i = 0; i < n; i++)
        if (!strcmp(TEXT_DIGEST[i].section, BOARDCONF_SECTION) &&
                TEXT_DIGEST[i].lcd == CORE_PUB_LCD)
            break;
    CHECK(i < n, "descriptor [%s] LCD %d has a text reply digest", BOARDCONF_SECTION, CORE_PUB_LCD);
    if (i == n)
        return;
    mc_api_stats_text(&s, buf, sizeof buf, 0, 0);
    CHECK(text_digest(buf, "estats") == TEXT_DIGEST[i].estats,
            "text estats is byte for byte the same reply");
    mc_api_stats_text(&s, buf, sizeof buf, 1, 0);
    CHECK(text_digest(buf, "estats|all") == TEXT_DIGEST[i].estats_all,
            "text estats|all is byte for byte the same reply");
    mc_api_stats_text(&s, buf, sizeof buf, 0, 1);
    CHECK(text_digest(buf, "stats") == TEXT_DIGEST[i].stats,
            "text stats is byte for byte the same reply");
    mc_api_litestats_text(&s, buf, sizeof buf);
    CHECK(text_digest(buf, "litestats") == TEXT_DIGEST[i].litestats,
            "text litestats is byte for byte the same reply");
}

int main(void)
{
    static char  buf[MC_API_REPLY_CAP];
    mc_api_stats s = fixed_stats();
    const char  *vs, *ve;
    int          bad;

    mc_api_version(&s, buf, sizeof buf);
    vs = strstr(buf, "\"VERSION\":[{");
    ve = vs ? strstr(vs, "}]") : NULL;
    CHECK(vs && ve, "version JSON has a VERSION section");
    if (vs && ve) {
        bad = first_key_out_of_place(vs, ve, "\"", "\":");
        CHECK(bad < 0, "version JSON key set and order (first wrong: %s)",
                bad < 0 ? "none" : VERSION_KEYS[bad]);
        CHECK(count_sub(vs + strlen("\"VERSION\":[{"), ve, "\":") == VERSION_KEY_N,
                "version JSON carries exactly %d keys", VERSION_KEY_N);
    }
    CHECK(strstr(buf, "\"FWVERSION\":\"26070301_649070d\"") && !strstr(buf, "CGVERSION"),
            "version JSON carries FWVERSION and no CGVERSION");
    CHECK(!strstr(buf, "LVERSION") && !strstr(buf, "BVERSION") && !strstr(buf, "AMVERSION"),
            "version JSON drops the three removed version fields");
    CHECK(strstr(buf, "\"Msg\":\"AvalonMiner versions\",\"Description\":\"" WANT_DESC "\""),
            "version JSON STATUS Msg is \"AvalonMiner versions\" and Description "
            "is \"" WANT_DESC "\". VERSION_KEYS above pins the "
            "field KEYS; this pins the Msg");

    mc_api_version_text(&s, buf, sizeof buf);
    vs = strstr(buf, "|VERSION,");
    ve = vs ? strchr(vs + 1, '|') : NULL;
    CHECK(vs && ve, "version text has a VERSION section");
    if (vs && ve) {
        bad = first_key_out_of_place(vs, ve, ",", "=");
        CHECK(bad < 0, "version text key set and order (first wrong: %s)",
                bad < 0 ? "none" : VERSION_KEYS[bad]);
        CHECK(count_sub(vs + strlen("|VERSION,"), ve, "=") == VERSION_KEY_N,
                "version text carries exactly %d keys", VERSION_KEY_N);
    }
    CHECK(strstr(buf, ",FWVERSION=26070301_649070d,") && !strstr(buf, "CGVERSION"),
            "version text carries FWVERSION and no CGVERSION");
    CHECK(!strstr(buf, "LVERSION") && !strstr(buf, "BVERSION") && !strstr(buf, "AMVERSION"),
            "version text drops the three removed version fields");
    CHECK(strstr(buf, "Code=22,Msg=AvalonMiner versions,Description=" WANT_DESC "|"),
            "version text STATUS Msg is \"AvalonMiner versions\", in place before "
            "Description=" WANT_DESC);
    CHECK(strstr(buf, ",Description=" WANT_DESC "|VERSION,AvalonMiner=0.0.0,"),
            "version text Description and AvalonMiner field");

    mc_api_version(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":[{\"STATUS\":\"S\""), "version has STATUS S");
    CHECK(strstr(buf, "\"Description\":\"" WANT_DESC "\""),
            "version JSON Description is \"" WANT_DESC "\"");
    CHECK(strstr(buf, "\"AvalonMiner\":\"0.0.0\""),
            "version JSON AvalonMiner field carries the build semver");
    CHECK(strstr(buf, "\"PROD\":\"Avalon Nano3s\""), "version PROD");
    CHECK(strstr(buf, "\"MODEL\":\"Nano3s\""), "version MODEL");
    CHECK(strstr(buf, "\"HWTYPE\":\"N_MM1v1_X1\""), "version HWTYPE");
    CHECK(strstr(buf, "\"SWTYPE\":\"MM319\""), "version SWTYPE");
    CHECK(strstr(buf, "\"DNA\":\"0201000012345678\""), "version DNA");
    CHECK(strstr(buf, "\"API\":\"3.7\""), "version API const");
    CHECK(strstr(buf, "\"id\":1}"), "version id tail");

    {
        mc_api_stats hv = fixed_stats();
        static char  clean[MC_API_REPLY_CAP];
        static char  decoded[MC_API_MM_ID0_CAP];
        char         field[160];
        int          n_comma, n_bar, rc;

        mc_api_version_text(&hv, clean, sizeof clean);
        n_comma = count_sub(clean, clean + strlen(clean), ",");
        n_bar   = count_sub(clean, clean + strlen(clean), "|");

        hv.prod  = HOSTILE_PROD;
        hv.model = HOSTILE_MODEL;

        mc_api_version_text(&hv, buf, sizeof buf);
        CHECK(count_sub(buf, buf + strlen(buf), ",") == n_comma &&
                        count_sub(buf, buf + strlen(buf), "|") == n_bar,
                "a ',' or '|' in MODEL or PROD must add "
                "no field and no record to the version TEXT reply (%d commas / %d "
                "bars against the clean reply's %d / %d)",
                count_sub(buf, buf + strlen(buf), ","), count_sub(buf, buf + strlen(buf), "|"),
                n_comma, n_bar);
        vs = strstr(buf, "|VERSION,");
        ve = vs ? strchr(vs + 1, '|') : NULL;
        CHECK(vs && ve, "hostile MODEL/PROD: version text still has ONE VERSION section");
        if (vs && ve) {
            bad = first_key_out_of_place(vs, ve, ",", "=");
            CHECK(bad < 0, "...with the key set and order intact (first wrong: %s)",
                    bad < 0 ? "none" : VERSION_KEYS[bad]);
        }
        CHECK(!strstr(buf, ",MAC=deadbeef") && !strstr(buf, ",UPAPI=9"),
                "...so neither forged field is preceded by a separator: the bytes "
                "survive inside a value, which is not the same as being a field");
        CHECK(strstr(buf, ",PROD=" HOSTILE_PROD_SAFE ",MODEL=" HOSTILE_MODEL_SAFE ",HWTYPE=") !=
                        NULL,
                "...and both render scrubbed, in place, between their real neighbours");

        mc_api_version(&hv, buf, sizeof buf);
        rc = json_str_value(buf, "MODEL", field, sizeof field, NULL);
        CHECK(rc == JSTR_OK && !strcmp(field, HOSTILE_MODEL_SAFE),
                "version JSON: the hostile MODEL is a legal JSON string decoding to "
                "the scrubbed form (reader says: %s)",
                jstr_why(rc));
        rc = json_str_value(buf, "PROD", field, sizeof field, NULL);
        CHECK(rc == JSTR_OK && !strcmp(field, HOSTILE_PROD_SAFE),
                "...and so is PROD (reader says: %s)", jstr_why(rc));
        vs = strstr(buf, "\"VERSION\":[{");
        ve = vs ? strstr(vs, "}]") : NULL;
        CHECK(vs && ve, "hostile MODEL/PROD: version JSON still has ONE VERSION section");
        if (vs && ve) {
            bad = first_key_out_of_place(vs, ve, "\"", "\":");
            CHECK(bad < 0, "...with the key set and order intact (first wrong: %s)",
                    bad < 0 ? "none" : VERSION_KEYS[bad]);
            CHECK(count_sub(vs + strlen("\"VERSION\":[{"), ve, "\":") == VERSION_KEY_N,
                    "...and exactly %d keys: a '\"' in MODEL opens no new one", VERSION_KEY_N);
        }

        mc_api_stats_(&hv, buf, sizeof buf, 0, 0);
        rc = sj_str(buf, "Ver", decoded, sizeof decoded);
        CHECK(sj_ok(buf) && rc == 0 && !strcmp(decoded, HOSTILE_MODEL_SAFE "-26070301_649070d") &&
                        sj_has(buf, "Ver[" HOSTILE_MODEL_SAFE "-26070301_649070d] LVer["),
                "estats JSON: the same MODEL reaches Sysinfo.Ver scrubbed, as a legal "
                "JSON string followed by LVer (got \"%s\")",
                decoded);
        mc_api_stats_text(&hv, buf, sizeof buf, 0, 0);
        CHECK(strstr(buf, "Ver[" HOSTILE_MODEL_SAFE "-26070301_649070d] LVer[") != NULL,
                "estats TEXT: Ver[] carries no ',' or '|' either, so a MODEL cannot "
                "split the ','-separated MM ID0 record");

        hv.prod  = "Avalon Nano3s";
        hv.model = "Nano3s";
        mc_api_version_text(&hv, buf, sizeof buf);
        CHECK(!strcmp(buf, clean),
                "a clean MODEL/PROD still renders byte-identically: the scrub "
                "rewrites nothing that does not need it");
        mc_api_version(&hv, buf, sizeof buf);
        CHECK(strstr(buf, "\"PROD\":\"Avalon Nano3s\",\"MODEL\":\"Nano3s\"") != NULL,
                "...on the JSON path too");
    }

    mc_api_summary(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Msg\":\"Summary\""), "summary msg");
    CHECK(strstr(buf, "\"Elapsed\":565"), "summary elapsed");
    CHECK(strstr(buf, "\"Accepted\":37"), "summary accepted");
    CHECK(strstr(buf, "\"Rejected\":0"), "summary rejected");
    CHECK(strstr(buf, "\"Best Share\":296878"), "summary best share");

    mc_api_config(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"ASC Count\":1"), "config asc count");
    CHECK(strstr(buf, "\"Pool Count\":3"), "config pool count");
    CHECK(strstr(buf, "\"Device Code\":\"AVALON\""), "config device code");

    mc_api_coin(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Hash Method\":\"sha256\""), "coin hash method");

    mc_api_devdetails(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Code\":69"), "devdetails code 69");
    CHECK(strstr(buf, "\"Msg\":\"Device Details\""), "devdetails msg");
    CHECK(strstr(buf, "\"Name\":\"AVALON\""), "devdetails name");
    CHECK(strstr(buf, "\"Driver\":\"avalon\""), "devdetails driver");
    CHECK(strstr(buf, "\"DEVDETAILS\":0"), "devdetails index");

    mc_api_devdetails_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "Code=69"), "devdetails text code");
    CHECK(strstr(buf, "DEVDETAILS=0"), "devdetails text index");
    CHECK(strstr(buf, "Name=AVALON"), "devdetails text name");
    CHECK(strstr(buf, "Driver=avalon"), "devdetails text driver");
    CHECK(strstr(buf, "Device Path=|"), "devdetails text ends with pipe");

    mc_api_lcd(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Code\":125"), "lcd code 125");
    CHECK(strstr(buf, "\"Msg\":\"LCD\""), "lcd msg");
    CHECK(strstr(buf, "\"Elapsed\":565"), "lcd elapsed");
    CHECK(strstr(buf, "\"GHS av\":"), "lcd ghs av present");
    CHECK(strstr(buf, "\"Temperature\":92.00"), "lcd temperature from telemetry");
    CHECK(strstr(buf, "\"Current Pool\":\"stratum+tcp://btc.f2pool.com:3333\""),
            "lcd current pool url");
    CHECK(strstr(buf, "\"User\":\"poolworker.001\""), "lcd pool user");

    mc_api_lcd_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "Code=125"), "lcd text code");
    CHECK(strstr(buf, "LCD,Elapsed=565"), "lcd text elapsed");
    CHECK(strstr(buf, "Temperature=92.00"), "lcd text temperature from telemetry");
    CHECK(strstr(buf, "Current Pool=stratum+tcp://btc.f2pool.com:3333"), "lcd text pool url");
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=125,Msg=LCD,Description=" WANT_DESC
                  "|LCD,Elapsed=565,GHS av=5693.98,GHS 5m=5892.96,GHS 5s=0.00,"
                  "Temperature=92.00,Last Share Difficulty=0.00000000,"
                  "Last Share Time=4133982044,Best Share=296878,"
                  "Last Valid Work=4133982044,Found Blocks=0,"
                  "Current Pool=stratum+tcp://btc.f2pool.com:3333,User=poolworker.001|"),
            "lcd text wire form byte-identical for a metacharacter-free pool");
    {
        mc_api_stats e = fixed_stats();
        e.pool[0].url  = "stratum+tcp://ho,st|x:3333";
        e.pool[0].user = "wor,ker|01";
        mc_api_lcd_text(&e, buf, sizeof buf);
        CHECK(strstr(buf, "Current Pool=stratum+tcp://ho?st?x:3333,User=wor?ker?01|"),
                "lcd text scrubs the comma and pipe in Current Pool and User");
        CHECK(!strstr(buf, "ho,st") && !strstr(buf, "st|x") && !strstr(buf, "wor,ker") &&
                        !strstr(buf, "ker|01"),
                "lcd text emits no raw comma/pipe from the pool url or user");
    }

    {
        mc_api_stats np = fixed_stats();
        np.npools       = 0;
        mc_api_lcd(&np, buf, sizeof buf);
        CHECK(strstr(buf, "\"Current Pool\":\"none\",\"User\":\"\""),
                "lcd no-pool current pool defaults to none/empty");
        mc_api_lcd_text(&np, buf, sizeof buf);
        CHECK(strstr(buf, "Current Pool=none,User=|"),
                "lcd text no-pool current pool defaults to none/empty");
    }

    mc_api_devs(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Msg\":\"1 ASC(s)\""), "devs msg");
    CHECK(strstr(buf, "\"ASC\":0"), "devs asc0");
    CHECK(strstr(buf, "\"Name\":\"AVALON\""), "devs name");
    CHECK(strstr(buf, "\"Accepted\":37"), "devs accepted");
    CHECK(strstr(buf, "\"Temperature\":92.00"), "devs temp from telemetry");

    mc_api_pools(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"URL\":\"stratum+tcp://btc.f2pool.com:3333\""), "pools url");
    CHECK(strstr(buf, "\"Status\":\"Alive\""), "pools status");
    CHECK(strstr(buf, "\"User\":\"poolworker.001\",\"Last Share Time\":") &&
                    !strstr(buf, "\"Password\":"),
            "pools omits Password, User adjoins Last Share Time");
    CHECK(strstr(buf, "\"Stratum Active\":true"), "pools stratum active");
    CHECK(!strcmp(buf,
                  "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":4133982044,\"Code\":7,"
                  "\"Msg\":\"1 Pool(s)\",\"Description\":\"" WANT_DESC "\"}],"
                  "\"POOLS\":[{\"POOL\":0,\"URL\":\"stratum+tcp://btc.f2pool.com:3333\","
                  "\"Status\":\"Alive\",\"Priority\":0,\"Quota\":1,\"Long Poll\":\"N\","
                  "\"Getworks\":0,\"Accepted\":37,\"Rejected\":0,\"Works\":0,\"Discarded\":0,"
                  "\"Stale\":0,\"Get Failures\":0,\"Remote Failures\":0,"
                  "\"User\":\"poolworker.001\","
                  "\"Last Share Time\":4133982044,\"Diff1 Shares\":749568,"
                  "\"Proxy Type\":\"\",\"Proxy\":\"\",\"Difficulty Accepted\":0.00000000,"
                  "\"Difficulty Rejected\":0.00000000,\"Difficulty Stale\":0.00000000,"
                  "\"Last Share Difficulty\":0.00000000,\"Work Difficulty\":0.00000000,"
                  "\"Has Stratum\":true,\"Stratum Active\":true,"
                  "\"Stratum URL\":\"btc.f2pool.com\",\"Stratum Difficulty\":0.00000000,"
                  "\"Has Vmask\":true,\"Has GBT\":false,\"Best Share\":0,"
                  "\"Pool Rejected%\":0.0000,\"Pool Stale%\":0.0000,\"Bad Work\":0,"
                  "\"Current Block Height\":0,\"Current Block Version\":0}],\"id\":1}"),
            "pools wire form byte-identical for a metacharacter-free pool");
    {
        mc_api_stats e = fixed_stats();
        e.pool[0].url  = "stratum+tcp://b\\ad\"host:3333";
        e.pool[0].user = "wor\\ker\"01";
        mc_api_pools(&e, buf, sizeof buf);
        CHECK(strstr(buf, "\"URL\":\"stratum+tcp://b\\\\ad\\\"host:3333\","),
                "pools escapes the backslash and quote in URL");
        CHECK(!strstr(buf, "\"URL\":\"stratum+tcp://b\\ad\""),
                "pools emits no raw backslash/quote in URL");
        CHECK(strstr(buf, "\"User\":\"wor\\\\ker\\\"01\","),
                "pools escapes the backslash and quote in User");
        CHECK(!strstr(buf, "\"User\":\"wor\\ker\""), "pools emits no raw backslash/quote in User");
        CHECK(strstr(buf, "\"User\":\"wor\\\\ker\\\"01\",\"Last Share Time\":") &&
                        !strstr(buf, "\"Password\":"),
                "pools omits the Password key entirely");
        CHECK(strstr(buf, "\"Stratum URL\":\"b\\\\ad\\\"host\","),
                "pools escapes the host cut from the same URL");
        CHECK(!strstr(buf, "\"Stratum URL\":\"b\\ad\""),
                "pools emits no raw backslash/quote in Stratum URL");
    }

    {
        mc_api_stats l = fixed_stats();
        char         url[MC_API_POOL_URL_CAP], host[MC_API_POOL_URL_CAP - 14 - 5], want[640];
        snprintf(host, sizeof host, "%0*d", (int)MC_API_POOL_URL_CAP - 1 - 14 - 5, 0);
        snprintf(url, sizeof url, "stratum+tcp://%s:3333", host);
        CHECK(strlen(url) == MC_API_POOL_URL_CAP - 1, "fixture url is 256 chars");
        l.pool[0].url = url;
        mc_api_pools(&l, buf, sizeof buf);
        snprintf(want, sizeof want, "\"URL\":\"%s\",", url);
        CHECK(strstr(buf, want), "pools reads a 256-char url back whole");
        snprintf(want, sizeof want, "\"Stratum URL\":\"%s\",", host);
        CHECK(strstr(buf, want), "pools reads its host back whole");
        mc_api_pools_text(&l, buf, sizeof buf);
        snprintf(want, sizeof want, "URL=%s,Status=", url);
        CHECK(strstr(buf, want), "pools text reads a 256-char url back whole");
        snprintf(want, sizeof want, "Stratum URL=%s,", host);
        CHECK(strstr(buf, want), "pools text reads its host back whole");
        mc_api_lcd(&l, buf, sizeof buf);
        snprintf(want, sizeof want, "\"Current Pool\":\"%s\",", url);
        CHECK(strstr(buf, want) && strstr(buf, "}],\"id\":1}"),
                "lcd reads a 256-char url back whole");
        mc_api_lcd_text(&l, buf, sizeof buf);
        snprintf(want, sizeof want, "Current Pool=%s,", url);
        CHECK(strstr(buf, want), "lcd text reads a 256-char url back whole");
        char msg[MC_API_POOL_URL_CAP + 64];
        snprintf(msg, sizeof msg, "Pool 0:'%s' already disabled", url);
        mc_api_status_reply(0, buf, sizeof buf, 0, 'I', 50, msg);
        snprintf(want, sizeof want, "Msg=%s,", msg);
        CHECK(strstr(buf, want), "status text reply quotes a 256-char url whole");
        mc_api_status_reply(1, buf, sizeof buf, 0, 'I', 50, msg);
        snprintf(want, sizeof want, "\"Msg\":\"%s\",", msg);
        CHECK(strstr(buf, want), "status JSON reply quotes a 256-char url whole");
    }

    {
        mc_api_stats o = fixed_stats();
        static char  wide[128];
        memset(wide, '"', sizeof wide - 1);
        o.npools = 5;
        for (int i = 0; i < o.npools; i++)
            o.pool[i] = (mc_api_pool){ .id = i,
                .url                       = wide,
                .user                      = wide,
                .status                    = "Alive",
                .stratum_active            = 1,
                .getworks                  = 4294967295u,
                .accepted                  = 4294967295u,
                .rejected                  = 4294967295u,
                .works                     = 4294967295u,
                .get_failures              = 4294967295u,
                .remote_failures           = 4294967295u,
                .last_share_time           = 4294967295u,
                .bad_work                  = 4294967295u,
                .block_height              = 4294967295u,
                .block_version             = 4294967295u,
                .diff_accepted             = 1e18,
                .diff_rejected             = 1e18,
                .last_share_diff           = 1e18,
                .work_diff                 = 1e18,
                .diff1_shares              = 1e18,
                .best_share                = 1e18 };
        size_t n    = mc_api_pools(&o, buf, sizeof buf);
        int    recs = 0;
        for (const char *q = buf; (q = strstr(q, "{\"POOL\":")); q++)
            recs++;
        CHECK(n < sizeof buf && strstr(buf, "}],\"id\":1}"),
                "pools closes the POOLS array when the pool records overrun sec[] "
                "(5 pools x max-width url/user, len %zu, %d records)",
                n, recs);
        CHECK(recs >= 2 && strstr(buf, "\"POOLS\":[{\"POOL\":0,"),
                "pools still emits whole records up to the sec[] budget (%d)", recs);
    }

    mc_api_pools_text(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=7,Msg=1 Pool(s),Description=" WANT_DESC
                  "|POOL=0,URL=stratum+tcp://btc.f2pool.com:3333,Status=Alive,"
                  "Priority=0,Quota=1,Long Poll=N,Getworks=0,Accepted=37,Rejected=0,"
                  "Works=0,Discarded=0,Stale=0,Get Failures=0,Remote Failures=0,"
                  "User=poolworker.001,"
                  "Last Share Time=4133982044,Diff1 Shares=749568,"
                  "Proxy Type=,Proxy=,Difficulty Accepted=0.00000000,"
                  "Difficulty Rejected=0.00000000,Difficulty Stale=0.00000000,"
                  "Last Share Difficulty=0.00000000,Work Difficulty=0.00000000,"
                  "Has Stratum=true,Stratum Active=true,Stratum URL=btc.f2pool.com,"
                  "Stratum Difficulty=0.00000000,Has Vmask=true,Has GBT=false,"
                  "Best Share=0,Pool Rejected%=0.0000,Pool Stale%=0.0000,Bad Work=0,"
                  "Current Block Height=0,Current Block Version=0|"),
            "pools text wire form byte-identical for a metacharacter-free pool");
    {
        mc_api_stats e = fixed_stats();
        e.pool[0].url  = "stratum+tcp://ho,st|x:3333";
        e.pool[0].user = "wor,ker|01";
        mc_api_pools_text(&e, buf, sizeof buf);
        CHECK(strstr(buf, "URL=stratum+tcp://ho?st?x:3333,Status=Alive,"),
                "pools text scrubs the comma and pipe in URL");
        CHECK(!strstr(buf, "ho,st") && !strstr(buf, "st|x"),
                "pools text emits no raw comma/pipe from URL");
        CHECK(strstr(buf, "User=wor?ker?01,Last Share Time="),
                "pools text scrubs the comma and pipe in User");
        CHECK(!strstr(buf, "wor,ker") && !strstr(buf, "ker|01"),
                "pools text emits no raw comma/pipe from User");
        CHECK(strstr(buf, "Stratum URL=ho?st?x,Stratum Difficulty="),
                "pools text scrubs the host cut from the same URL");
    }

    {
        mc_api_stats np = fixed_stats();
        np.npools       = 0;
        mc_api_pools(&np, buf, sizeof buf);
        CHECK(strstr(buf, "\"STATUS\":\"E\"") && strstr(buf, "\"Code\":8") &&
                        strstr(buf, "\"Msg\":\"No pools\"") && !strstr(buf, "POOLS"),
                "pools JSON with 0 pools is E,8,No pools, no POOLS array");
        mc_api_pools_text(&np, buf, sizeof buf);
        CHECK(!strcmp(buf,
                      "STATUS=E,When=4133982044,Code=8,Msg=No pools,Description=" WANT_DESC "|"),
                "pools text with 0 pools is the E,8,No pools status line");
    }

    check_stats_json_shape();
    check_text_unchanged();

    mc_api_stats_(&s, buf, sizeof buf, 0, 0);
    CHECK(strstr(buf, "\"STATS\":"), "stats section");
    CHECK(strstr(buf,
                  "\"Code\":70,\"Msg\":\"AvalonMiner stats\","
                  "\"Description\":\"" WANT_DESC "\""),
            "estats JSON STATUS Msg is \"AvalonMiner stats\" at Code 70, and "
            "Description is \"" WANT_DESC "\"");

    {
        mc_api_stats sp      = fixed_stats();
        sp.npools            = 2;
        sp.pool[0].work_diff = 65536.0;
        sp.pool[0].tx_cnt    = 13;
        sp.pool[0].tx_bytes  = 1718;
        sp.pool[0].rx_cnt    = 30;
        sp.pool[0].rx_bytes  = 27159;
        sp.pool[1]           = (mc_api_pool){ .id = 1, .status = "Alive", .work_diff = 8192.0 };
        char pbuf[8192];
        mc_api_stats_(&sp, pbuf, sizeof pbuf, 0, 1);
        CHECK(strstr(pbuf, "\"STATS\":1,\"ID\":\"POOL0\""), "stats JSON POOL0 section");
        CHECK(strstr(pbuf, "\"STATS\":2,\"ID\":\"POOL1\""), "stats JSON POOL1 section");
        CHECK(strstr(pbuf, "\"Work Diff\":65536.00000000"), "stats JSON pool Work Diff");
        CHECK(strstr(pbuf,
                      "\"Pool Calls\":0,\"Pool Attempts\":0,\"Pool Wait\":0.000000,"
                      "\"Pool Max\":0.000000,\"Pool Min\":99999999.000000,\"Pool Av\":0.000000"),
                "stats JSON pool getwork-timing fields stay stock 0/99999999");
        CHECK(strstr(pbuf,
                      "\"Times Sent\":13,\"Bytes Sent\":1718,\"Times Recv\":30,"
                      "\"Bytes Recv\":27159,"
                      "\"Net Bytes Sent\":1718,\"Net Bytes Recv\":27159}"),
                "stats JSON POOL0 socket counters carry real traffic");
        CHECK(strstr(pbuf,
                      "\"Times Sent\":0,\"Bytes Sent\":0,\"Times Recv\":0,\"Bytes Recv\":0,"
                      "\"Net Bytes Sent\":0,\"Net Bytes Recv\":0}"),
                "stats JSON POOL1 (zero-traffic) socket counters read 0");
        mc_api_stats_text(&sp, pbuf, sizeof pbuf, 0, 1);
        CHECK(strstr(pbuf, "STATS=1,ID=POOL0,"), "stats text POOL0 section");
        CHECK(strstr(pbuf, "STATS=2,ID=POOL1,"), "stats text POOL1 section");
        CHECK(strstr(pbuf, "Work Diff=8192.00000000"), "stats text pool Work Diff");
        CHECK(strstr(pbuf,
                      "Times Sent=13,Bytes Sent=1718,Times Recv=30,Bytes Recv=27159,"
                      "Net Bytes Sent=1718,Net Bytes Recv=27159|"),
                "stats text POOL0 socket counters carry real traffic");
        CHECK(strstr(pbuf,
                      "Times Sent=0,Bytes Sent=0,Times Recv=0,Bytes Recv=0,"
                      "Net Bytes Sent=0,Net Bytes Recv=0|"),
                "stats text POOL1 (zero-traffic) socket counters read 0");
        mc_api_stats_text(&sp, pbuf, sizeof pbuf, 0, 0);
        CHECK(!strstr(pbuf, "ID=POOL0"), "estats text omits pool sections");
    }
    CHECK(sj_ok(buf), "estats JSON is one well-formed document with MMID0Summary an object");
    CHECK(sj_has(buf, "LVer[26070302_abcdef0]"), "estats LVer uses mm version");
    CHECK(!strstr(buf, "26070303_1234567"), "estats carries no asic version");
    CHECK(sj_has(buf, "Ver[Nano3s-26070301_649070d]"),
            "estats Ver preserves supplied software version");
    CHECK(sj_has(buf, "ITemp[-273]"), "estats ITemp is the -273 inlet sentinel");
    CHECK(sj_has(buf, "ITemp[-273] OTemp[68]"), "estats OTemp carries the board NTC");
    {
        char hbo_exp[96] = " HBOTemp[", hbi_exp[96] = " HBITemp[";
        for (int b = 0; b < HBOARD_COUNT; b++) {
            snprintf(hbo_exp + strlen(hbo_exp), sizeof hbo_exp - strlen(hbo_exp), "%s43",
                    b ? " " : "");
            snprintf(hbi_exp + strlen(hbi_exp), sizeof hbi_exp - strlen(hbi_exp), "%s41",
                    b ? " " : "");
        }
        strcat(hbo_exp, "]");
        strcat(hbi_exp, "]");
        CHECK(sj_has(buf, hbo_exp), "estats HBOTemp lists the outlet reading per board (\"%s\")",
                hbo_exp);
        CHECK(sj_has(buf, hbi_exp), "estats HBITemp lists the inlet reading per board (\"%s\")",
                hbi_exp);
        CHECK(sj_has(buf, "DH[0.498%] HBITemp[") && sj_has(buf, tst_fmt("%s TMax[92]", hbo_exp)),
                "estats HBITemp/HBOTemp sit between DH and TMax in Hashinfo, so no "
                "existing field moved");
        CHECK(sj_has(buf, tst_fmt("%s HBOTemp[", hbi_exp)),
                "estats HBITemp precedes HBOTemp, as in the API format");
        mc_api_stats z = s;
        static char  zb[MC_API_REPLY_CAP];
        z.telem.hb_count = 0;
        mc_api_stats_(&z, zb, sizeof zb, 0, 0);
        CHECK(sj_ok(zb) && sj_has(zb, "ITemp[-273] OTemp[68]"),
                "estats OTemp carries the board NTC even when no MCU answered");
        CHECK(!sj_key(zb, "HBOTemp"),
                "estats HBOTemp is absent on a product with no hashboard MCUs");
        CHECK(!sj_key(zb, "HBITemp"),
                "estats HBITemp is absent on a product with no hashboard MCUs");
        CHECK(sj_has(zb, "DH[0.498%] TMax[92]"),
                "...and with both absent TMax follows DH directly");
    }
    CHECK(sj_has(buf, "DNA[0201000012345678] MEMFREE[39004] PFCnt["),
            "estats DNA carries the snapshot identity and sits between the "
            "capability block and MEMFREE — the phone app reads DNA off this "
            "blob as well as off the version reply, and no assertion covered "
            "the estats copy");
    CHECK(sj_has(buf, "MEMFREE[39004]"), "estats MEMFREE carries /proc/meminfo MemAvailable in kB");
    CHECK(sj_has(buf, "TMax[92] TAvg[89]"), "estats chip temps");
    {
        char        span[64];
        const char *want_fans = tst_fmt("OTemp[68]%s FanR[31%%]", tst_fan_span(span, sizeof span));
        CHECK(sj_has(buf, "TAvg[89] TarT[90]") && sj_has(buf, want_fans),
                "estats fan fields: TarT after TAvg in Hashinfo, %s in Sysinfo", want_fans);
    }
#if defined(BOARD_X3_H)
    CHECK(!sj_key(buf, "Fan4"), "estats emits no field for a fan x3 does not have");
#elif defined(BOARD_NANO3S_H)
    CHECK(!sj_key(buf, "Fan2"), "estats emits exactly ONE fan field on a single-fan product");
#else
#error "no estats fan goldens for this board"
#endif
    CHECK(sj_has(buf, "PS[0 0 27581 4 0 3574 132]"), "estats PS rail");
    CHECK(sj_has(buf, "GHSspd[5000.00]"), "estats GHSspd = windowed chip spdlog rate");
    CHECK(sj_has(buf, "DHspd[0.498%]"), "estats DHspd is a 3-decimal percent");
    CHECK(sj_has(buf, "HW[0] DH[0.498%]"), "estats DH is a 3-decimal percent");
    {
        mc_api_stats np = fixed_stats();
        static char  dhb[MC_API_REPLY_CAP];
        for (int b = 0; b < HBOARD_COUNT; b++)
            for (int c = 0; c < MINER_MAX_CHIPS; c++)
                np.telem.chip_pass[b][c] = 0;
        mc_api_stats_(&np, dhb, sizeof dhb, 0, 0);
        CHECK(sj_has(dhb, "HW[0] DH[100.000%]"),
                "every chip failing SPDLOG reports the API DH of 100 percent");
        CHECK(sj_has(dhb, "DHspd[100.000%]"), "...and the same DHspd beside it");
        mc_api_stats bt = fixed_stats();
        memset(&bt.telem, 0, sizeof bt.telem);
        mc_api_stats_(&bt, dhb, sizeof dhb, 0, 0);
        CHECK(sj_has(dhb, "HW[0] DH[0.000%]"), "no chip telemetry yet reports DH of 0 percent");
        CHECK(sj_has(dhb, "DHspd[0.000%]"), "...and the same DHspd beside it");
    }
    {
        mc_api_stats h                                = fixed_stats();
        char         mh_exp[64 + 11 * HBOARD_COUNT]   = "LW[101467] MH[";
        char         echu_exp[64 + 11 * HBOARD_COUNT] = " ECHU[";
        uint32_t     tot                              = 0;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            h.hw_board[b]   = (uint32_t)(11 + b);
            h.hu_errcode[b] = (uint32_t)(21 + b);
            tot += h.hw_board[b];
            snprintf(mh_exp + strlen(mh_exp), sizeof mh_exp - strlen(mh_exp), "%s%u", b ? " " : "",
                    (unsigned)(11 + b));
            snprintf(echu_exp + strlen(echu_exp), sizeof echu_exp - strlen(echu_exp), "%s%u",
                    b ? " " : "", (unsigned)(21 + b));
        }
        h.hw_errors  = tot + 7;
        h.mm_errcode = 4294967295u;
        snprintf(mh_exp + strlen(mh_exp), sizeof mh_exp - strlen(mh_exp), "] DHW[0] HW[%u] DH[",
                (unsigned)tot);
        strcat(echu_exp, "]");
        static char hb[MC_API_REPLY_CAP];
        mc_api_stats_(&h, hb, sizeof hb, 0, 0);
        CHECK(sj_has(hb, mh_exp),
                "estats MH lists the per-board tally and HW is its sum, NOT the "
                "hw_errors scalar (\"%s\")",
                mh_exp);
        CHECK(sj_has(hb, tst_fmt("BIN[47]%s", echu_exp)) && sj_has(hb, "ECMM[4294967295]"),
                "estats ECHU lists the per-board code after BIN (\"%s\"), ECMM the MM "
                "code in full",
                echu_exp);
        char sb[4096];
        mc_api_summary_text(&h, sb, sizeof sb);
        char hwe_exp[64];
        snprintf(hwe_exp, sizeof hwe_exp, "Hardware Errors=%u,", (unsigned)(tot + 7));
        CHECK(strstr(sb, hwe_exp),
                "summary \"Hardware Errors\" is the scalar, every charge (\"%s\")", hwe_exp);
        mc_api_devs_text(&h, sb, sizeof sb);
        CHECK(strstr(sb, hwe_exp), "devs \"Hardware Errors\" is the same scalar (\"%s\")", hwe_exp);
    }
    CHECK(sj_has(buf, "GHSmm[6067.10]"), "estats GHSmm");
    CHECK(sj_has(buf, "GHSavg[5698.00]"), "estats GHSavg == WU * 2^32/1e9/60");
    CHECK(sj_has(buf, "WU[79600.14]"), "estats WU");
    {
        char sysstat[64];
        snprintf(sysstat, sizeof sysstat, "SYSTEMSTATU[Work: In Work, Hash Board: %d]",
                HBOARD_COUNT);
        CHECK(sj_has(buf, sysstat), "estats sysstatus");
    }
    CHECK(sj_has(buf, "Elapsed[565]"), "estats elapsed");
    CHECK(sj_has(buf, "BOOTBY[0x03.00000000]"), "estats bootby from snapshot");
    CHECK(sj_has(buf, "Elapsed[565] BOOTBY[0x03.00000000] ITemp[") && sj_index(buf, "LW") == 0,
            "estats bootby sits between Elapsed and ITemp in Sysinfo, LW opens Hashinfo");
    CHECK(sj_has(buf, "LW[101467]"), "estats LW is the MM-side count");
    CHECK(!sj_has(buf, "LW[6693]"),
            "estats LW is NOT the summary's Local Work: LW[] is the MM's "
            "per-board task_send_work() count, Local Work the core's own "
            "work-item counter");
    CHECK(sj_has(buf, "PING[47]"), "estats ping");
    CHECK(sj_has(buf, "PING[47] POOLS[0] SoftOFF[0]"),
            "estats POOLS sits between PING and SoftOFF, 0 while the active pool is alive");
    CHECK(sj_has(buf, "SoftOFF[0]"), "estats SoftOFF at rest (no fault)");
    {
        mc_api_stats pi = fixed_stats();
        pi.pool_idle    = 1;
        static char pib[MC_API_REPLY_CAP];
        mc_api_stats_(&pi, pib, sizeof pib, 0, 0);
        CHECK(sj_has(pib, "PING[47] POOLS[1] SoftOFF[0]"), "estats POOLS[1] without a live pool");
    }
    {
        mc_api_stats sf      = fixed_stats();
        sf.telem.softoff_rsn = MC_SOFTOFF_ASIC_OVERHOT;
        static char sfb[MC_API_REPLY_CAP];
        mc_api_stats_(&sf, sfb, sizeof sfb, 0, 0);
        CHECK(sj_has(sfb, "SoftOFF[3]"), "estats SoftOFF carries the reason code (ASIC_OVERHOT=3)");
        CHECK(!sj_has(sfb, "SoftOFF[0]"), "estats SoftOFF[0] gone when reason is non-zero");
    }
    CHECK(sj_has(buf, "WORKMODE[2]"), "estats workmode");
    {
        char want_ta[24];
        snprintf(want_ta, sizeof want_ta, "TA[%d]", HBOARD_COUNT * MAX_ASIC_ON_SINGLE_HASH);
        CHECK(sj_has(buf, want_ta), "estats chip count is machine-wide: %s", want_ta);
        CHECK(sj_has(buf, "BIN[47]"), "estats BIN from efuse");
        CHECK(sj_has(buf, tst_fmt("%s Core[A3197S] BIN[47] ECHU[", want_ta)),
                "estats BIN sits between Core and ECHU in Hashinfo");
    }
    CHECK(sj_has(buf, "Activation[1]"), "estats activation from ini");
    {
        mc_api_stats a0 = fixed_stats();
        a0.activation   = 0;
        mc_api_stats_(&a0, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "Activation[0]"), "estats activation=0");
    }
    CHECK(sj_has(buf, "MPO[133]"), "estats MPO from cali max_pout");
    CHECK(sj_has(buf, "TarT[90]"), "estats TarT from cali temp");
    if (!CORE_PUB_LED) {
        CHECK(!sj_key(buf, "LED") && !sj_key(buf, "LEDUser"),
                "estats emits no LED fields on a board without an LED strip");
    } else {
        CHECK(sj_has(buf, "LED[0-1]"), "estats LED[ledmode-runeffect]");
        CHECK(sj_has(buf, "LEDUser[1-100-50-0-0-255]"), "estats LEDUser six-field readback");
    }
    if (CORE_PUB_LED) {
        mc_api_stats d  = fixed_stats();
        d.ledmode       = 1;
        d.led_effect    = 3;
        d.led_runeffect = 0;
        d.led_bright    = 49;
        d.led_rgb       = 0x0F005Fu;
        mc_api_stats_(&d, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "LED[1-0]"), "estats LED carries the live effect");
        CHECK(sj_has(buf, "LEDUser[3-49-50-15-0-95]"), "estats LEDUser splits rgb per channel");
    }
    if (CORE_PUB_LED) {
        mc_api_stats n  = fixed_stats();
        n.led_runeffect = 6;
        mc_api_stats_(&n, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "LED[0-6]"), "estats LED shows the night lamp");
        CHECK(sj_has(buf, "LEDUser[1-100-50-0-0-255]"), "estats nightlamp leaves LEDUser alone");
    }
    {
        mc_api_stats fq = fixed_stats();
        mc_api_stats_(&fq, buf, sizeof buf, 0, 0);
#if defined(BOARD_X3_H)
        CHECK(sj_has(buf, "Freq[56.70]"), "estats Freq = GHSmm/all-cores (fractional)");
        fq.telem.ghs_mm = 38778.63;
        mc_api_stats_(&fq, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "GHSmm[38778.63] GHSavg[5698.00] WU[79600.14] Freq[362.39]"),
                "the x3 GHSmm/TA/Freq triple");
#elif defined(BOARD_NANO3S_H)
        CHECK(sj_has(buf, "Freq[415.78]"), "estats Freq = GHSmm/all-cores (fractional)");
        CHECK(sj_has(buf, "GHSmm[6067.10] GHSavg[5698.00] WU[79600.14] Freq[415.78]"),
                "the nano3s capture's GHSmm/TA/Freq triple");
#else
#error "no estats Freq goldens for this board"
#endif
    }
    {
        mc_api_stats fq   = fixed_stats();
        fq.telem.ghs_mm   = 0;
        fq.telem.freq_mhz = 392;
        mc_api_stats_(&fq, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "Freq[392.00]"), "estats Freq falls back to freq_mhz");
    }

    {
        mc_api_stats a = fixed_stats();
        a.adj          = 2;
        a.bar          = 100;
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "ADJ[2] BAR[100]"),
                "estats ADJ = 2 with BAR once aging finished in this run");
        CHECK(sj_has(buf, "ATA2[133-90-3450-420-20]"), "estats ATA aging result tuple");
    }

    {
        mc_api_stats a = fixed_stats();
        a.adj          = 1;
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "ADJ[1]"), "estats ADJ = 1 when booted calibrated");
        CHECK(!sj_key(buf, "BAR"), "estats omits BAR when booted calibrated");
        CHECK(sj_has(buf, "ATA2[133-90-3450-420-20]"), "estats ATA renders when booted calibrated");
    }

    char want_crc0[32], want_comcrc0[32], want_crc7[32], want_comcrc3[32];
    tst_board_list(want_crc0, sizeof want_crc0, " CRC", 0);
    tst_board_list(want_comcrc0, sizeof want_comcrc0, " COMCRC", 0);
    tst_board_list(want_crc7, sizeof want_crc7, " CRC", 7);
    tst_board_list(want_comcrc3, sizeof want_comcrc3, " COMCRC", 3);
    {
        mc_api_stats a = fixed_stats();
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, want_crc0), "estats CRC per hash board");
        CHECK(sj_has(buf, want_comcrc0), "estats COMCRC per hash board");
        CHECK(sj_has(buf, "ATA2[]") && sj_type(buf, "ATA2") == 's',
                "estats ATA is the empty string while aging unfinished");
        CHECK(sj_has(buf, "ATA2[] WORKMODE[2]"), "estats WORKMODE tail follows the per-chip block");
    }

    {
        mc_api_stats a           = fixed_stats();
        a.telem.board_err_crc[0] = 7;
        a.telem.board_com_crc[0] = 3;
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, want_crc7), "estats CRC renders the board err_crc tally");
        CHECK(sj_has(buf, want_comcrc3), "estats COMCRC renders the board com_crc tally");
    }

    {
        mc_api_stats z  = fixed_stats();
        z.telem.board_n = 0;
        mc_api_stats_(&z, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, want_crc0),
                "a device with no board enumerated still carries one CRC slot per "
                "HBOARD_COUNT (\"%s\"), not an empty CRC[] sized by the live board "
                "count: one slot per fixed hashboard, 0 for a board with "
                "no counter",
                want_crc0);
        CHECK(sj_has(buf, want_comcrc0), "chipless estats keeps %s", want_comcrc0);
        CHECK(sj_index(buf, "DH") >= 0 && sj_index(buf, "DH") < sj_index(buf, "CRC") &&
                        sj_in(buf, "DH", "Hashinfo") &&
                        sj_has(buf, tst_fmt("%s %s ATA2[]", want_crc0, want_comcrc0)),
                "the two labels keep their place between DH and ATA in Hashinfo, so the "
                "slots are added inside CRC/COMCRC and no field already on the wire moves");
        mc_api_stats_text(&z, buf, sizeof buf, 0, 0);
        CHECK(strstr(buf, want_crc0) && strstr(buf, want_comcrc0),
                "text estats carries the same chipless CRC shape as JSON — both "
                "renderings come out of build_mm_id0");
        mc_api_stats_(&z, buf, sizeof buf, 1, 0);
        CHECK(sj_has(buf, want_crc0) && sj_has(buf, want_comcrc0),
                "estats|all too — CRC/COMCRC sit above the show_all split");
        mc_api_litestats_(&z, buf, sizeof buf);
        CHECK(sj_ok(buf) && !sj_key(buf, "CRC") && !sj_key(buf, "COMCRC") && !strstr(buf, "CRC"),
                "litestats carries no CRC at all, before or after");
    }

    {
        mc_api_stats z           = fixed_stats();
        z.telem.board_n          = 1;
        z.telem.board_err_crc[0] = 7;
        z.telem.board_com_crc[0] = 3;
        mc_api_stats_(&z, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, want_crc7) && sj_has(buf, want_comcrc3),
                "a partial enumeration reports the boards that did not answer as 0 "
                "rather than dropping their slots, so the slot index keeps meaning "
                "the board index (x3: one board of two)");
    }

    {
        mc_api_stats a = fixed_stats();
        a.caliall      = (1 << 1) | (1 << 2);
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "CALIALL[6]"), "estats CALIALL renders the per-mode cali bitmask");
    }

    {
        mc_api_stats a = fixed_stats();
        a.adj          = 0;
        a.bar          = 50;
        mc_api_stats_(&a, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "ADJ[0] BAR[50]"), "estats emits BAR = aging progress while unfinished");
    }

    {
        mc_api_stats z = fixed_stats();
        memset(&z.telem, 0, sizeof z.telem);
        z.telem.temp_max = z.telem.temp_avg = z.telem.board_temp = -273;
        mc_api_stats_(&z, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "SYSTEMSTATU[Work: In Init, Hash Board: 0]"), "estats init sysstatus");
        CHECK(sj_has(buf, "TMax[-273]"), "estats no-reading sentinel");

        char zeros[2 * ASIC_PLL_POINTS + 1];
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            zeros[2 * k]     = '0';
            zeros[2 * k + 1] = ' ';
        }
        zeros[2 * ASIC_PLL_POINTS - 1] = '\0';
        for (int b = 0; b < HBOARD_COUNT; b++) {
            char want[64];
            snprintf(want, sizeof want, " PLL%d[%s]", b, zeros);
            CHECK(sj_has(buf, want),
                    "no chip telemetry (no pool, ASIC in fault or soft-off, "
                    "telemetry past the 6 s window, board pre-network) still "
                    "carries %s — these labels loop over HBOARD_COUNT, never "
                    "over the live chip count, and a client that reads a "
                    "label out and indexes into it faults when the label is "
                    "ABSENT, not when it is empty",
                    want);
            snprintf(want, sizeof want, " SF%d[%s]", b, zeros);
            CHECK(sj_has(buf, want), "chipless estats keeps %s", want);
            snprintf(want, sizeof want, " PVT_T%d[]", b);
            CHECK(sj_has(buf, want),
                    "chipless estats keeps %s — empty bracket, the shape a "
                    "stock nano3s already ships for ATA2[]",
                    want);
            snprintf(want, sizeof want, " PVT_V%d[]", b);
            CHECK(sj_has(buf, want), "chipless estats keeps %s", want);
            snprintf(want, sizeof want, " MW%d[]", b);
            CHECK(sj_has(buf, want), "chipless estats keeps %s", want);
            snprintf(want, sizeof want, "PVT_T%d", b);
            CHECK(sj_type(buf, want) == 'a' && sj_array_len(buf, want) == 0,
                    "...as an empty JSON array, not a missing key or a string");
        }
        CHECK(sj_key(buf, "WORKMODE"), "chipless estats still reaches its tail");

        {
            char want[64];
            snprintf(want, sizeof want, " SF0[%s]", zeros);
            mc_api_stats_text(&z, buf, sizeof buf, 0, 0);
            CHECK(strstr(buf, want) && strstr(buf, " PVT_T0[]"),
                    "text estats carries the same chipless shape as JSON — both "
                    "renderings come out of build_mm_id0");
        }

        mc_api_stats_(&z, buf, sizeof buf, 1, 0);
        CHECK(!sj_key(buf, "SCANAGE"),
                "chipless estats|all still omits SCANAGE — no board enumerated, "
                "so there is no collector age to report");
        for (int b = 0; b < HBOARD_COUNT; b++) {
            char want[64];
            snprintf(want, sizeof want, " CF%d_0[]", b);
            CHECK(sj_has(buf, want), "chipless estats|all keeps %s", want);
            snprintf(want, sizeof want, " PLLCNT%d_0[]", b);
            CHECK(sj_has(buf, want), "chipless estats|all keeps %s", want);
            snprintf(want, sizeof want, " ERATIO%d[]", b);
            CHECK(sj_has(buf, want), "chipless estats|all keeps %s", want);
            snprintf(want, sizeof want, " C_%02d_00[]", b);
            CHECK(sj_has(buf, want), "chipless estats|all keeps %s", want);
            snprintf(want, sizeof want, " GHSmm%02d[]", b);
            CHECK(sj_has(buf, want), "chipless estats|all keeps %s", want);
        }
    }

    {
        mc_api_stats c           = fixed_stats();
        c.telem.board_n          = 1;
        c.telem.chip_n[0]        = 2;
        c.telem.chip_pvt_t[0][0] = 90;
        c.telem.chip_pvt_t[0][1] = 88;
        c.telem.chip_pvt_v[0][0] = 812;
        c.telem.chip_pvt_v[0][1] = 809;
        uint8_t  pc0[4] = { 2, 0, 0, 150 }, pc1[4] = { 1, 0, 1, 150 };
        uint16_t cf0[4] = { 100, 200, 300, 416 }, cf1[4] = { 100, 200, 300, 416 };
        for (int k = 0; k < 4; k++) {
            c.telem.chip_pllcnt[0][0][k] = pc0[k];
            c.telem.chip_pllcnt[0][1][k] = pc1[k];
            c.telem.chip_cf[0][0][k]     = cf0[k];
            c.telem.chip_cf[0][1][k]     = cf1[k];
        }
        c.telem.chip_pass[0][0] = 1000;
        c.telem.chip_fail[0][0] = 5;
        c.telem.chip_pass[0][1] = 800;
        c.telem.chip_fail[0][1] = 0;
        c.telem.chip_mw[0][0]   = 42;
        c.telem.chip_mw[0][1]   = 7;

        mc_api_stats_(&c, buf, sizeof buf, 0, 0);
        CHECK(sj_has(buf, "PVT_T0[ 90  88]"), "estats PVT_T per chip");
        CHECK(sj_has(buf, "PVT_V0[812 809]"), "estats PVT_V per chip");
        CHECK(sj_has(buf, "PLL0[3 0 1 300]"), "estats PLL summed per point");
        CHECK(sj_has(buf, "SF0[100 200 300 416]"), "estats SF = board asic-0 CF");
        CHECK(sj_has(buf, "MW0[42 7]"), "estats MW per-chip accepted count");
        CHECK(!sj_has_prefix(buf, "CF0_"), "base estats omits extended CF");
        CHECK(!sj_key(buf, "ERATIO0"), "base estats omits extended ERATIO");
        CHECK(!sj_key(buf, "SCANAGE"), "base estats omits SCANAGE");

        mc_api_stats_(&c, buf, sizeof buf, 1, 0);
        CHECK(sj_has(buf, "PVT_T0[ 90  88]"), "estats|all keeps base PVT_T");
        CHECK(!sj_key(buf, "SCANAGE"),
                "estats|all emits no SCANAGE, matching the "
                "API estats|all field set, which has no such field");
        {
            mc_api_stats sa = c;
            static char  sab[MC_API_REPLY_CAP];
            sa.telem.board_n = 0;
            mc_api_stats_(&sa, sab, sizeof sab, 1, 0);
            CHECK(sj_ok(sab) && !sj_key(sab, "SCANAGE"),
                    "...and no board enumerated means no SCANAGE at all, not "
                    "SCANAGE[0]");
        }
        CHECK(sj_has(buf, "CF0_3[416 416]"), "estats|all CF per point per chip");
        CHECK(sj_has(buf, "PLLCNT0_3[150 150]"), "estats|all PLLCNT per point per chip");
        CHECK(sj_has(buf, "ERATIO0[  0.50%   0.00%]"), "estats|all ERATIO fail/(pass+fail)");
        CHECK(sj_has(buf, "C_00_00[   1000     800]"), "estats|all C_ pass counts");
        CHECK(sj_has(buf, "C_00_01[      5       0]"), "estats|all C_ fail counts");
        CHECK(sj_has(buf, "GHSmm00[  62.60   62.80]"), "estats|all per-chip GHSmm");
        CHECK(sj_in(buf, "GHSmm00", "HB_0_Info") && sj_in(buf, "PVT_T0", "HB_0_Info") &&
                        sj_in(buf, "PLL0", "Hashinfo"),
                "estats|all per-chip lists sit in HBinfo.HB_0_Info, the board sums in "
                "Hashinfo");

        CHECK(mc_api_estats_all("estats|all") == 1, "estats_all text form");
        CHECK(mc_api_estats_all("{\"command\":\"estats\",\"parameter\":\"all\"}") == 1,
                "estats_all json form");
        CHECK(mc_api_estats_all("estats") == 0, "estats_all off by default");
    }

    {
        mc_api_stats r  = fixed_stats();
        r.rejected      = 3;
        r.diff_rejected = 24576.0;
        mc_api_summary(&r, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Difficulty Accepted\":671744.00000000,"
                      "\"Difficulty Rejected\":24576.00000000,"
                      "\"Difficulty Stale\":0.00000000,\"Best Share\":296878"),
                "summary Difficulty Rejected is the live figure, in place, 8-decimal");
        mc_api_devs(&r, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Difficulty Accepted\":671744.00000000,"
                      "\"Difficulty Rejected\":24576.00000000,"
                      "\"Last Share Difficulty\":0.00000000,"
                      "\"Last Valid Work\":4133982044,\"Device Hardware%\":0.0000"),
                "devs Difficulty Rejected is the live figure, in place, 8-decimal");
        mc_api_summary(&s, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Difficulty Accepted\":671744.00000000,"
                      "\"Difficulty Rejected\":0.00000000,"
                      "\"Difficulty Stale\":0.00000000,\"Best Share\":296878"),
                "summary zero-reject form unchanged from the literal");
        mc_api_devs(&s, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Difficulty Accepted\":671744.00000000,"
                      "\"Difficulty Rejected\":0.00000000,"
                      "\"Last Share Difficulty\":0.00000000,"
                      "\"Last Valid Work\":4133982044,\"Device Hardware%\":0.0000"),
                "devs zero-reject form unchanged from the literal");
    }

    {
        mc_api_summary(&s, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Total MH\":3217099180.2500,\"Work Utility\":79600.14,"
                      "\"Difficulty Accepted\":671744.00000000,"
                      "\"Difficulty Rejected\":0.00000000,"
                      "\"Difficulty Stale\":0.00000000,\"Best Share\":296878"),
                "summary Work Utility + Difficulty Stale at their API positions");
        CHECK(strstr(buf, "\"Work Utility\":79600.14") && strstr(buf, "\"Elapsed\":565"),
                "summary Work Utility = diff1/elapsed*60 (captured device reply 79543.99)");
        {
            mc_api_stats w = fixed_stats();
            w.elapsed      = 1130;
            static char wb[MC_API_REPLY_CAP];
            mc_api_summary(&w, wb, sizeof wb);
            CHECK(strstr(wb, "\"Work Utility\":39800.07,"), "summary Work Utility tracks elapsed");
            mc_api_stats_(&w, wb, sizeof wb, 0, 0);
            CHECK(sj_has(wb, "WU[39800.07]"),
                    "estats WU tracks elapsed identically (one shared definition)");
        }
        {
            mc_api_stats w = fixed_stats();
            w.elapsed      = 0;
            char wb[4096];
            mc_api_summary(&w, wb, sizeof wb);
            CHECK(strstr(wb, "\"Work Utility\":0.00,"),
                    "summary Work Utility is 0.00 before the first second elapses");
        }

        {
            mc_api_stats d            = fixed_stats();
            d.pool[0].last_share_diff = 131072.0;
            mc_api_devs(&d, buf, sizeof buf);
            CHECK(strstr(buf,
                          "\"Difficulty Rejected\":0.00000000,"
                          "\"Last Share Difficulty\":131072.00000000,"
                          "\"Last Valid Work\":4133982044,"
                          "\"Device Hardware%\":0.0000"),
                    "devs Last Share Difficulty + Last Valid Work at their API positions");
            mc_api_devs_text(&d, buf, sizeof buf);
            CHECK(strstr(buf,
                          "Difficulty Rejected=0.00000000,"
                          "Last Share Difficulty=131072.00000000,"
                          "Last Valid Work=4133982044,"
                          "Device Hardware%=0.0000"),
                    "text devs carries the same two fields in the same slot");
        }
        {
            mc_api_stats d            = fixed_stats();
            d.npools                  = 0;
            d.pool[0].last_share_diff = 131072.0;
            mc_api_devs(&d, buf, sizeof buf);
            CHECK(strstr(buf,
                          "\"Difficulty Rejected\":0.00000000,"
                          "\"Last Share Difficulty\":0.00000000,"),
                    "devs Last Share Difficulty is 0.00000000 with no pool");
        }
    }

    {
        mc_api_summary(&s, buf, sizeof buf);
        CHECK(strstr(buf, "\"Hardware Errors\":0,\"Utility\":3.93,\"Discarded\":0"),
                "summary Utility = accepted/elapsed*60, between Hardware Errors and Discarded");
        CHECK(!strstr(buf, "\"Utility\":3.929") && !strstr(buf, "\"Utility\":3.9,"),
                "summary Utility is %%.2f — not 1 or 3 decimals");
        mc_api_devs(&s, buf, sizeof buf);
        CHECK(strstr(buf, "\"Hardware Errors\":0,\"Utility\":4.12,\"Last Share Pool\":0"),
                "devs Utility in place, between Hardware Errors and Last Share Pool");
        CHECK(strstr(buf, "\"Utility\":4.12,") && strstr(buf, "\"Device Elapsed\":539"),
                "devs Utility uses the clock it reports as Device Elapsed");
        {
            mc_api_stats u = fixed_stats();
            u.accepted     = 74;
            char ub[4096];
            mc_api_summary(&u, ub, sizeof ub);
            CHECK(strstr(ub, "\"Utility\":7.86,"), "summary Utility doubles with accepted");
            u         = fixed_stats();
            u.elapsed = 1130;
            mc_api_summary(&u, ub, sizeof ub);
            CHECK(strstr(ub, "\"Utility\":1.96,"), "summary Utility halves with elapsed");
            u             = fixed_stats();
            u.elapsed     = 0;
            u.dev_elapsed = 0.0;
            mc_api_summary(&u, ub, sizeof ub);
            CHECK(strstr(ub, "\"Utility\":0.00,"),
                    "summary Utility is 0.00 before the first second elapses");
            mc_api_devs(&u, ub, sizeof ub);
            CHECK(strstr(ub, "\"Utility\":0.00,"), "devs Utility likewise at a zero device clock");
        }

        mc_api_summary(&s, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Network Blocks\":0,\"Total MH\":3217099180.2500,"
                      "\"Work Utility\":79600.14"),
                "summary Total MH = MHS av * Elapsed, between Network Blocks and Work Utility");
        CHECK(!strstr(buf, "\"Total MH\":3217099180.25,") &&
                        !strstr(buf, "\"Total MH\":3217099180.250000,"),
                "summary Total MH is %%.4f — not 2 or 6 decimals");
        {
            mc_api_stats m = fixed_stats();
            m.mhs_av       = 2846990.425;
            char mb[4096];
            mc_api_summary(&m, mb, sizeof mb);
            CHECK(strstr(mb, "\"Total MH\":1608549590.1250,"),
                    "summary Total MH halves with MHS av");
            m         = fixed_stats();
            m.elapsed = 1130;
            mc_api_summary(&m, mb, sizeof mb);
            CHECK(strstr(mb, "\"Total MH\":6434198360.5000,"),
                    "summary Total MH doubles with elapsed");
        }
        {
            char sb[4096], db[4096];
            mc_api_summary(&s, sb, sizeof sb);
            mc_api_devs(&s, db, sizeof db);
            CHECK(strstr(sb, "\"Total MH\":3217099180.2500,") &&
                            strstr(db, "\"Total MH\":3217099180.2500,"),
                    "summary and devs carry one Total MH, as in every capture");
        }

        {
            mc_api_stats w         = fixed_stats();
            w.npools               = 2;
            w.diff1                = 13504512.0;
            w.pool[0].diff1_shares = 13500416.0;
            w.pool[1]              = w.pool[0];
            w.pool[1].id           = 1;
            w.pool[1].diff1_shares = 4096.0;
            char wb[4096], pb[8192];
            mc_api_devs(&w, wb, sizeof wb);
            CHECK(strstr(wb,
                          "\"Total MH\":3217099180.2500,\"Diff1 Work\":13504512,"
                          "\"Difficulty Accepted\":671744.00000000"),
                    "devs Diff1 Work = the device diff-1 total, between Total MH and "
                    "Difficulty Accepted");
            CHECK(!strstr(wb, "\"Diff1 Work\":13504512.0") &&
                            !strstr(wb, "\"Diff1 Work\":13500416,"),
                    "devs Diff1 Work is integer-shaped (%%.0f) and is not just pool 0");
            mc_api_pools(&w, pb, sizeof pb);
            CHECK(strstr(pb, "\"Diff1 Shares\":13500416,") && strstr(pb, "\"Diff1 Shares\":4096,"),
                    "pools still emits the per-pool columns, unchanged");
            mc_api_devs_text(&w, wb, sizeof wb);
            CHECK(strstr(wb,
                          "Total MH=3217099180.2500,Diff1 Work=13504512,"
                          "Difficulty Accepted=671744.00000000"),
                    "text devs carries Total MH + Diff1 Work in the same slot");
            char         sb[4096], eb[MC_API_REPLY_CAP], eb2[MC_API_REPLY_CAP];
            mc_api_stats r = w;
            r.npools       = 1;
            mc_api_devs(&r, wb, sizeof wb);
            CHECK(strstr(wb, "\"Diff1 Work\":13504512,"),
                    "a removepool does not move Diff1 Work (device total 13504512)");
            mc_api_summary(&w, sb, sizeof sb);
            mc_api_summary(&r, wb, sizeof wb);
            {
                const char *a = strstr(sb, "\"Work Utility\":");
                const char *b = strstr(wb, "\"Work Utility\":");
                CHECK(a && b && !strncmp(a, b, 24), "...nor WU (%.24s vs %.24s)", a ? a : "-",
                        b ? b : "-");
            }
            mc_api_stats_(&w, eb, sizeof eb, 0, 0);
            mc_api_stats_(&r, eb2, sizeof eb2, 0, 0);
            {
                sj_val a, b;
                int    fa = sj_find(eb, "GHSavg", &a), fb = sj_find(eb2, "GHSavg", &b);
                CHECK(fa && fb && a.end - a.val == b.end - b.val &&
                                !memcmp(a.val, b.val, (size_t)(a.end - a.val)),
                        "...nor GHSavg (%.*s vs %.*s)", fa ? (int)(a.end - a.val) : 1,
                        fa ? a.val : "-", fb ? (int)(b.end - b.val) : 1, fb ? b.val : "-");
            }
        }
        {
            mc_api_stats w         = fixed_stats();
            w.npools               = 1;
            w.diff1                = 0.0;
            w.pool[0].diff1_shares = 13500416.0;
            char wb[4096];
            mc_api_devs(&w, wb, sizeof wb);
            CHECK(strstr(wb, "\"Diff1 Work\":0,\"Difficulty Accepted\":"),
                    "devs Diff1 Work zero form unchanged from the literal");
        }

        mc_api_summary_text(&s, buf, sizeof buf);
        CHECK(strstr(buf, "Hardware Errors=0,Utility=3.93,Discarded=0"),
                "text summary Utility in place");
        CHECK(strstr(buf, "Network Blocks=0,Total MH=3217099180.2500,Work Utility="),
                "text summary Total MH in place");
        mc_api_devs_text(&s, buf, sizeof buf);
        CHECK(strstr(buf, "Hardware Errors=0,Utility=4.12,Last Share Pool=0"),
                "text devs Utility in place");
    }

    mc_api_summary_text(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=11,Msg=Summary,Description=" WANT_DESC "|"
                  "SUMMARY,Elapsed=565,MHS av=5693980.85,MHS 5s=0.00,MHS 1m=0.00,"
                  "MHS 5m=5892960.00,MHS 15m=0.00,Found Blocks=0,Getworks=0,Accepted=37,"
                  "Rejected=0,Hardware Errors=0,Utility=3.93,Discarded=0,Stale=0,"
                  "Get Failures=0,Local Work=6693,Remote Failures=0,Network Blocks=0,"
                  "Total MH=3217099180.2500,Work Utility=79600.14,"
                  "Difficulty Accepted=671744.00000000,"
                  "Difficulty Rejected=0.00000000,Difficulty Stale=0.00000000,"
                  "Best Share=296878,"
                  "Device Hardware%=0.0000,Device Rejected%=0.0000,Pool Rejected%=0.0000,"
                  "Pool Stale%=0.0000,Last getwork=0|"),
            "text summary wire form");

    mc_api_devs_text(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=9,Msg=1 ASC(s),Description=" WANT_DESC "|"
                  "ASC=0,Name=AVALON,ID=0,Enabled=Y,Status=Alive,Temperature=92.00,"
                  "MHS av=5968644.12,MHS 5s=0.00,MHS 1m=0.00,MHS 5m=5892960.00,"
                  "MHS 15m=0.00,Accepted=37,Rejected=0,Hardware Errors=0,Utility=4.12,"
                  "Last Share Pool=0,Last Share Time=4133982044,Total MH=3217099180.2500,"
                  "Diff1 Work=749568,Difficulty Accepted=671744.00000000,"
                  "Difficulty Rejected=0.00000000,Last Share Difficulty=0.00000000,"
                  "Last Valid Work=4133982044,Device Hardware%=0.0000,"
                  "Device Rejected%=0.0000,Device Elapsed=539|"),
            "text devs wire form");

    mc_api_coin_text(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=78,Msg=AvalonMiner coin,Description=" WANT_DESC "|"
                  "COIN,Hash Method=sha256,Current Block Time=0.000000,Current Block Hash="
                  "00000000000000000001cb4150b955d47fd97be3d968826bb45111a859f51935,"
                  "LP=true,Network Difficulty=133869853540305.39062500|"),
            "text coin wire form");

    mc_api_config_text(&s, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "STATUS=S,When=4133982044,Code=33,Msg=AvalonMiner config,Description=" WANT_DESC
                  "|"
                  "CONFIG,ASC Count=1,PGA Count=0,Pool Count=3,Strategy=Failover,"
                  "Log Interval=5,Device Code=AVALON,OS=Linux,Hotplug=None|"),
            "text config wire form");

    {
        mc_api_stats r  = fixed_stats();
        r.rejected      = 3;
        r.diff_rejected = 24576.0;
        mc_api_summary_text(&r, buf, sizeof buf);
        CHECK(strstr(buf,
                      "Difficulty Accepted=671744.00000000,"
                      "Difficulty Rejected=24576.00000000,"
                      "Difficulty Stale=0.00000000,Best Share=296878"),
                "text summary carries the live Difficulty Rejected");
        mc_api_devs_text(&r, buf, sizeof buf);
        CHECK(strstr(buf,
                      "Difficulty Accepted=671744.00000000,"
                      "Difficulty Rejected=24576.00000000,"
                      "Last Share Difficulty=0.00000000,"
                      "Last Valid Work=4133982044,Device Hardware"),
                "text devs carries the live Difficulty Rejected");
    }

    {
        mc_api_stats f            = fixed_stats();
        f.npools                  = 2;
        f.pool[0].get_failures    = 34;
        f.pool[0].remote_failures = 2;
        f.pool[1]                 = (mc_api_pool){
                            .id = 1, .status = "Alive", .get_failures = 5, .remote_failures = 1
        };
        f.get_failures    = 39;
        f.remote_failures = 3;
        f.pool_failcnt    = 46;
        static char fb[MC_API_REPLY_CAP];
        mc_api_summary(&f, fb, sizeof fb);
        CHECK(strstr(fb,
                      "\"Discarded\":0,\"Stale\":0,\"Get Failures\":39,"
                      "\"Local Work\":6693,\"Remote Failures\":3,"
                      "\"Network Blocks\":0"),
                "summary Get/Remote Failures are live, at their API positions");
        CHECK(!strstr(fb, "\"Get Failures\":34") && !strstr(fb, "\"Get Failures\":0"),
                "summary Get Failures is the device sum, not pool 0 and not the literal");
        mc_api_summary_text(&f, fb, sizeof fb);
        CHECK(strstr(fb,
                      "Discarded=0,Stale=0,Get Failures=39,Local Work=6693,"
                      "Remote Failures=3,Network Blocks=0"),
                "text summary carries both counters in the same two slots");
        mc_api_pools(&f, fb, sizeof fb);
        CHECK(strstr(fb, "\"Get Failures\":34,\"Remote Failures\":2,\"User\"") &&
                        strstr(fb, "\"Get Failures\":5,\"Remote Failures\":1,\"User\""),
                "pools still emits the per-pool halves the device sum is built from");
        mc_api_stats_(&f, fb, sizeof fb, 0, 0);
        CHECK(sj_has(fb, "MEMFREE[39004] PFCnt[46] NETFAIL["),
                "estats PFCnt carries pool_failcnt (not Get Failures),"
                " between MEMFREE and NETFAIL");
        mc_api_stats_(&s, fb, sizeof fb, 0, 0);
        CHECK(sj_has(fb, "MEMFREE[39004] PFCnt[0] NETFAIL["),
                "estats PFCnt zero form unchanged from the literal");
    }

    {
        mc_api_stats pr = fixed_stats();
        pr.npools       = 2;
        pr.pool[0].prio = 1;
        pr.pool[1]      = (mc_api_pool){ .id = 1, .prio = 0, .status = "Alive" };
        char prb[8192];
        mc_api_pools(&pr, prb, sizeof prb);
        CHECK(strstr(prb, "{\"POOL\":0,") && strstr(prb, "\"Priority\":1,") &&
                        strstr(prb, "{\"POOL\":1,") && strstr(prb, "\"Priority\":0,") &&
                        strstr(prb, "\"Priority\":1,") < strstr(prb, "{\"POOL\":1,"),
                "pools JSON Priority is the pool priority, not the pool index");
        mc_api_pools_text(&pr, prb, sizeof prb);
        CHECK(strstr(prb, "|POOL=0,") && strstr(prb, "|POOL=1,") &&
                        strstr(strstr(prb, "|POOL=0,"), "Priority=1,") &&
                        strstr(strstr(prb, "|POOL=0,"), "Priority=1,") < strstr(prb, "|POOL=1,") &&
                        strstr(strstr(prb, "|POOL=1,"), "Priority=0,"),
                "pools text Priority is the pool priority, not the pool index");
    }

    {
        mc_api_stats nf = fixed_stats();
        static char  nfb[MC_API_REPLY_CAP];
        for (int i = 0; i < 8; i++)
            nf.netfail_time[i] = (uint32_t)(11 + i);
        nf.dhw = 12345;
        mc_api_stats_(&nf, nfb, sizeof nfb, 0, 0);
        CHECK(sj_has(nfb, "NETFAIL[11 12 13 14 15 16 17 18] SYSTEMSTATU["),
                "estats NETFAIL renders all EIGHT slots, space-separated, and ends "
                "where SYSTEMSTATU begins. Every other snapshot in this file leaves "
                "netfail_time[] zeroed, so `NETFAIL[0 0 0 0 0 0 0 0]` matched a "
                "producer that emitted any number of zeroes, in any order");
        CHECK(sj_has(nfb, "DHW[12345] HW["),
                "estats DHW carries the duplicate-nonce tally. It was only ever "
                "asserted as DHW[0], which a producer hardcoding 0 also satisfies");
    }

    {
        mc_api_stats nb = fixed_stats();
        nb.net_blocks   = 7;
        char nbb[8192];
        mc_api_summary(&nb, nbb, sizeof nbb);
        CHECK(strstr(nbb, "\"Remote Failures\":0,\"Network Blocks\":7,\"Total MH\""),
                "summary Network Blocks is the live count, at its API position");
        CHECK(!strstr(nbb, "\"Network Blocks\":0"), "summary Network Blocks is not the literal 0");
        mc_api_summary_text(&nb, nbb, sizeof nbb);
        CHECK(strstr(nbb, "Remote Failures=0,Network Blocks=7,Total MH="),
                "text summary Network Blocks in the same slot");
    }

    {
        mc_api_stats cn = fixed_stats();
        cn.block_time   = 592.181022;
        mc_api_coin(&cn, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Hash Method\":\"sha256\",\"Current Block Time\":592.181022,"
                      "\"Current Block Hash\":\"000000000000000000"
                      "01cb4150b955d47fd97be3d968826bb45111a859f51935\","
                      "\"LP\":true,\"Network Difficulty\":133869853540305.39062500}"),
                "coin carries the captured block, difficulty and time, in order");
        mc_api_coin_text(&cn, buf, sizeof buf);
        CHECK(strstr(buf,
                      "COIN,Hash Method=sha256,Current Block Time=592.181022,"
                      "Current Block Hash=000000000000000000"
                      "01cb4150b955d47fd97be3d968826bb45111a859f51935,LP=true,"
                      "Network Difficulty=133869853540305.39062500|"),
                "text coin carries the same three in the same three slots");
        mc_api_stats nj   = fixed_stats();
        nj.block_hash     = NULL;
        nj.net_difficulty = 0.0;
        nj.block_time     = 0.0;
        mc_api_coin(&nj, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Current Block Time\":0.000000,\"Current Block Hash\":\"\","
                      "\"LP\":true,\"Network Difficulty\":0.00000000}"),
                "coin before the first notify keeps the shape the literals emitted");
    }

    {
        mc_api_stats z   = fixed_stats();
        z.npools         = 0;
        z.block_hash     = "000000000000000000000000000000000000";
        z.net_difficulty = 18446744073709551616.0;
        z.block_time     = 0.0;
        mc_api_coin(&z, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Current Block Hash\":\"000000000000000000"
                      "000000000000000000\",\"LP\":false,"
                      "\"Network Difficulty\":18446744073709551616.00000000}"),
                "coin with 0 pools is LP=false with the 36-zero hash and 2^64 diff");
        mc_api_coin_text(&z, buf, sizeof buf);
        CHECK(strstr(buf,
                      "Current Block Hash=000000000000000000"
                      "000000000000000000,LP=false,"
                      "Network Difficulty=18446744073709551616.00000000|"),
                "text coin with 0 pools carries LP=false and the same sentinels");
    }

    {
        mc_api_stats iv = fixed_stats();
        memset(&iv.telem, 0, sizeof iv.telem);
        iv.telem.temp_max = iv.telem.temp_avg = iv.telem.board_temp = -273;
        mc_api_devs(&iv, buf, sizeof buf);
        CHECK(strstr(buf,
                      "\"Enabled\":\"Y\",\"Status\":\"Initialising\","
                      "\"Temperature\":0.00"),
                "devs Status is Initialising before the device reports in");
        mc_api_devs_text(&iv, buf, sizeof buf);
        CHECK(strstr(buf, "Enabled=Y,Status=Initialising,Temperature=0.00"),
                "text devs Status likewise, in the same slot");
        mc_api_devs(&s, buf, sizeof buf);
        CHECK(strstr(buf, "\"Enabled\":\"Y\",\"Status\":\"Alive\",\"Temperature\":92.00"),
                "devs Status is Alive once telemetry is fresh, as the literal read");
        mc_api_stats nw     = fixed_stats();
        nw.telem.in_work    = 0;
        nw.telem.work_state = MC_WORK_INIT;
        mc_api_devs(&nw, buf, sizeof buf);
        CHECK(strstr(buf, "\"Status\":\"Alive\""),
                "devs Status stays Alive when the chain is up but not hashing");
        mc_api_stats_(&nw, buf, sizeof buf, 0, 0);
        {
            char ss[64];
            snprintf(ss, sizeof ss, "SYSTEMSTATU[Work: In Init, Hash Board: %d]", HBOARD_COUNT);
            CHECK(sj_has(buf, ss),
                    "...while SYSTEMSTATU still reports In Init for the same snapshot");
        }
    }

    {
        mc_api_stats dv = fixed_stats();
        dv.dev_elapsed  = 269.5;
        char db[4096];
        mc_api_devs(&dv, db, sizeof db);
        CHECK(strstr(db, "\"MHS av\":11937288.24,"),
                "devs MHS av is Total MH over the device clock, and doubles with it");
        CHECK(strstr(db, "\"Utility\":8.24,"), "devs Utility doubles with it too");
        CHECK(strstr(db, "\"Device Elapsed\":270}"),
                "Device Elapsed is the rounded device clock, as the API format prints it");
        CHECK(strstr(db, "\"Total MH\":3217099180.2500,") && strstr(db, "\"MHS 5m\":5892960.00,"),
                "...while Total MH and the rolling windows keep the session clock");
        mc_api_summary(&dv, db, sizeof db);
        CHECK(strstr(db, "\"MHS av\":5693980.85,") && strstr(db, "\"Utility\":3.93,") &&
                        strstr(db, "\"Elapsed\":565,"),
                "the summary is untouched by the device clock");
    }

    {
        mc_api_stats lv    = fixed_stats();
        lv.last_valid_work = 4133982050u;
        mc_api_devs(&lv, buf, sizeof buf);
        CHECK(strstr(buf, "\"Last Share Pool\":0,\"Last Share Time\":4133982044,"),
                "devs Last Share Time is the pool-confirmed share's clock");
        CHECK(strstr(buf, "\"Last Valid Work\":4133982050,\"Device Hardware%\":"),
                "...and Last Valid Work the later diff-1 one, in its own slot");
        mc_api_devs_text(&lv, buf, sizeof buf);
        CHECK(strstr(buf, "Last Share Time=4133982044,") &&
                        strstr(buf, "Last Valid Work=4133982050,"),
                "text devs carries the same two clocks");
        mc_api_lcd(&lv, buf, sizeof buf);
        CHECK(strstr(buf, "\"Last Share Time\":4133982044,") &&
                        strstr(buf, "\"Last Valid Work\":4133982050,"),
                "lcd carries the same two clocks");
    }

    {
        mc_api_stats mb = fixed_stats();
        static char  mbuf[MC_API_REPLY_CAP];
        mc_api_stats_(&mb, mbuf, sizeof mbuf, 0, 0);
#if defined(BOARD_X3_H)
        CHECK(sj_has(mbuf, "MGHS[5698.00 0.00] MTmax[") && sj_has(mbuf, "Activation[1]"),
                "MGHS lists every board, credit on board 0 only");
        CHECK(sj_has(mbuf, "MTmax[92 89] MTavg[89]") && sj_type(mbuf, "MTavg") == 'n',
                "MTmax lists every board's own max; MTavg stays a scalar");
#elif defined(BOARD_NANO3S_H)
        CHECK(sj_has(mbuf,
                      "GHSavg[5698.00] WU[79600.14] Freq[415.78] MGHS[5698.00] "
                      "MTmax[92] MTavg[89] TA[12]") &&
                        sj_array_len(mbuf, "MGHS") == 1 && sj_array_len(mbuf, "MTmax") == 1 &&
                        sj_has(mbuf, "Activation[1]"),
                "one board: MGHS/MTmax are one-element arrays holding the values of the "
                "scalars they replace");
#else
#error "no MGHS/MTmax goldens for this board"
#endif
        for (int b = 0; b < HBOARD_COUNT; b++)
            mb.diff1_board[b] = 749568.0 / HBOARD_COUNT;
        mc_api_stats_(&mb, mbuf, sizeof mbuf, 0, 0);
#if defined(BOARD_X3_H)
        CHECK(sj_has(mbuf, "MGHS[2849.00 2849.00]"),
                "an even split renders as equal halves of GHSavg");
#elif defined(BOARD_NANO3S_H)
        CHECK(sj_has(mbuf, "MGHS[5698.00]"), "one board holds the whole split");
#else
#error "no MGHS split goldens for this board"
#endif
        mc_api_stats mt  = fixed_stats();
        mt.telem.board_n = HBOARD_COUNT;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            mt.telem.chip_n[b]        = 2;
            mt.telem.chip_pvt_t[b][0] = (int16_t)(60 + b);
            mt.telem.chip_pvt_t[b][1] = (int16_t)(70 + b);
        }
        mc_api_stats_(&mt, mbuf, sizeof mbuf, 0, 0);
#if defined(BOARD_X3_H)
        CHECK(sj_has(mbuf, "MTmax[70 71] MTavg[89]"), "each board reports its own hottest chip");
#elif defined(BOARD_NANO3S_H)
        CHECK(sj_has(mbuf, "MTmax[70] MTavg[89]"), "the one board reports its own max");
#else
#error "no per-board MTmax goldens for this board"
#endif
        mc_api_stats mz = fixed_stats();
        memset(&mz.telem, 0, sizeof mz.telem);
        mz.telem.temp_max = mz.telem.temp_avg = -273;
        mc_api_stats_(&mz, mbuf, sizeof mbuf, 0, 0);
#if defined(BOARD_X3_H)
        CHECK(sj_has(mbuf, "MTmax[-273 -273] MTavg[-273]"),
                "with no chip telemetry every board is the sentinel");
#elif defined(BOARD_NANO3S_H)
        CHECK(sj_has(mbuf, "MTmax[-273] MTavg[-273]"),
                "with no chip telemetry the board is the sentinel");
#else
#error "no sentinel MTmax goldens for this board"
#endif
    }

    {
        mc_api_stats st = fixed_stats();
        st.stale        = 7;
        char sb[4096];
        mc_api_summary(&st, sb, sizeof sb);
        CHECK(strstr(sb, "\"Discarded\":0,\"Stale\":7,\"Get Failures\":0,"),
                "summary Stale is live, between Discarded and Get Failures");
        mc_api_summary_text(&st, sb, sizeof sb);
        CHECK(strstr(sb, "Discarded=0,Stale=7,Get Failures=0,"),
                "text summary Stale in the same slot");
    }

    {
        mc_api_stats st = fixed_stats();
        char         sb[4096];
        st.diff_accepted = 6144.0;
        st.diff_rejected = 1024.0;
        st.diff_stale    = 1024.0;
        mc_api_summary(&st, sb, sizeof sb);
        CHECK(strstr(sb,
                      "\"Difficulty Rejected\":1024.00000000,"
                      "\"Difficulty Stale\":1024.00000000,"),
                "summary Difficulty Stale is the live figure, in place");
        CHECK(strstr(sb, "\"Pool Rejected%\":12.5000,\"Pool Stale%\":12.5000,"),
                "...and both summary pool percentages are over accepted + "
                "rejected + stale difficulty");
        mc_api_summary_text(&st, sb, sizeof sb);
        CHECK(strstr(sb,
                      "Difficulty Rejected=1024.00000000,"
                      "Difficulty Stale=1024.00000000,"),
                "...text summary Difficulty Stale");
        CHECK(strstr(sb, "Pool Rejected%=12.5000,Pool Stale%=12.5000,"),
                "...and text summary pool percentages");
    }

    {
        mc_api_stats st = fixed_stats();
        static char  pb[8192];
        st.pool[0].diff_accepted = 6144.0;
        st.pool[0].diff_rejected = 1024.0;
        st.pool[0].stale         = 2;
        st.pool[0].diff_stale    = 1024.0;
        mc_api_pools(&st, pb, sizeof pb);
        CHECK(strstr(pb, "\"Discarded\":0,\"Stale\":2,\"Get Failures\":"),
                "pools Stale is the pool's own, between Discarded and Get Failures");
        CHECK(strstr(pb, "\"Difficulty Stale\":1024.00000000,"),
                "...and so is its Difficulty Stale");
        CHECK(strstr(pb, "\"Pool Rejected%\":12.5000,\"Pool Stale%\":12.5000,"),
                "...and both pool percentages are over accepted + rejected + "
                "stale difficulty");
        mc_api_pools_text(&st, pb, sizeof pb);
        CHECK(strstr(pb, "Discarded=0,Stale=2,Get Failures="), "text pools Stale in the same slot");
        CHECK(strstr(pb, "Difficulty Stale=1024.00000000,"), "...text Difficulty Stale");
        CHECK(strstr(pb, "Pool Rejected%=12.5000,Pool Stale%=12.5000,"),
                "...and text pool percentages");
    }

    mc_api_cmd_check("{\"command\":\"check\",\"parameter\":\"version\"}", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Code\":72"), "check json version code 72");
    CHECK(strstr(buf, "\"Exists\":\"Y\""), "check json version exists");
    CHECK(strstr(buf, "\"Access\":\"Y\""), "check json version access");
    CHECK(strstr(buf, "\"Msg\":\"Check command\",\"Description\":\"" WANT_DESC "\""),
            "check JSON builds its OWN STATUS record rather than calling "
            "mc_api_status_elem, so the product name in it is a SECOND copy of "
            "the string the rename touched, and nothing asserted it");

    mc_api_cmd_check("{\"command\":\"check\",\"parameter\":\"bogus\"}", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Code\":72"), "check json bogus code 72");
    CHECK(strstr(buf, "\"Exists\":\"N\""), "check json bogus not exists");
    CHECK(strstr(buf, "\"Access\":\"N\""), "check json bogus no access");

    mc_api_cmd_check("{\"command\":\"check\",\"parameter\":\"VeRsIoN\"}", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Exists\":\"Y\""), "check json VeRsIoN exists");
    CHECK(strstr(buf, "\"Access\":\"Y\""), "check json VeRsIoN access");

    mc_api_cmd_check("{\"command\":\"check\",\"parameter\":\"BoGuS\"}", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Exists\":\"N\""), "check json BoGuS not exists");
    CHECK(strstr(buf, "\"Access\":\"N\""), "check json BoGuS no access");

    mc_api_cmd_check("{\"command\":\"check\"}", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Code\":71"), "check json no-param code 71");
    CHECK(strstr(buf, "\"STATUS\":\"E\""), "check json no-param status E");

    mc_api_cmd_check_text("check|version", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Code=72"), "check text version code 72");
    CHECK(strstr(buf, "Exists=Y"), "check text version exists");
    CHECK(strstr(buf, "|CHECK,Exists=Y,Access=Y|"),
            "check text emits the CHECK section label before Exists");

    mc_api_cmd_check_text("check|version\n", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Exists=Y"), "check text version with trailing LF");

    mc_api_cmd_check_text("check|VeRsIoN", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Exists=Y"), "check text VeRsIoN exists");

    mc_api_cmd_check_text("check|BoGuS", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Exists=N"), "check text BoGuS not exists");

    mc_api_cmd_check_text("check|bogus", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Code=72"), "check text bogus code 72");
    CHECK(strstr(buf, "Exists=N"), "check text bogus not exists");
    CHECK(strstr(buf, "|CHECK,Exists=N,Access=N|"),
            "check text bogus emits the CHECK section label");

    mc_api_cmd_check_text("check", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Code=71"), "check text no-param code 71");
    CHECK(strstr(buf, "STATUS=E"), "check text no-param status E");
    CHECK(!strstr(buf, "CHECK,"), "check text no-param emits no CHECK section");

    {
        mc_api_stats live = fixed_stats();
        if (CORE_PUB_LCD) {
            live.lcd_onoff = 0;
            live.lcd_show  = 3;
        }
        if (CORE_PUB_AUDIO) {
            live.volume = 25;
        }
        mc_api_stats_(&live, buf, sizeof buf, 0, 0);
        if (CORE_PUB_LCD) {
            CHECK(sj_has(buf, "LcdOnoff[0] LcdSwitch[3]"), "estats JSON live LCD state");
        } else {
            CHECK(!sj_key(buf, "LcdOnoff") && !sj_key(buf, "LcdSwitch"),
                    "estats JSON emits neither LcdOnoff nor LcdSwitch on a board with "
                    "CAP_LCD 0 — the capability gate had a positive arm only, so a "
                    "board that grew the fields back would have failed nothing here");
        }
        if (CORE_PUB_AUDIO) {
            CHECK(sj_has(buf, "Volume[25]"), "estats JSON live volume");
        } else {
            CHECK(!sj_key(buf, "Volume"), "estats JSON emits no Volume on a board with CAP_BEEP 0");
        }
        mc_api_stats_text(&live, buf, sizeof buf, 0, 0);
        if (CORE_PUB_LCD) {
            CHECK(strstr(buf, "LcdOnoff[0] LcdSwitch[3]") != NULL, "estats text live LCD state");
        } else {
            CHECK(strstr(buf, "LcdOnoff[") == NULL && strstr(buf, "LcdSwitch[") == NULL,
                    "estats text likewise emits neither LcdOnoff nor LcdSwitch");
        }
        if (CORE_PUB_AUDIO) {
            CHECK(strstr(buf, "Volume[25]") != NULL, "estats text live volume");
        } else {
            CHECK(strstr(buf, "Volume[") == NULL, "estats text likewise emits no Volume");
        }
        CHECK(strstr(buf, "Msg=AvalonMiner stats,Description=" WANT_DESC "|") != NULL,
                "estats text STATUS Msg is \"AvalonMiner stats\", in place before "
                "Description — the text twin of the JSON pin above");
    }
    {
        mc_api_stats ls = fixed_stats();
        snprintf(ls.ssid, sizeof ls.ssid, "%s", "myssid");
        ls.rssi        = -47;
        ls.net_devtype = 1;
        ls.filtermesh  = 5;
        if (CORE_PUB_LCD) {
            ls.lcd_bright = 80;
        }
        char want_echu[32];
        tst_board_list(want_echu, sizeof want_echu, "ECHU", 0);
        mc_api_litestats_text(&ls, buf, sizeof buf);
        CHECK(strstr(buf,
                      tst_fmt("LVer[26070302_abcdef0] "
                              "FW[%s] " TEXT_PVER "STATE[1] MODE[2] LEVEL[0] GHSspd[5000.000000] "
                              "ITemp[-273] OTemp[68] Limitemp[0] FanR[31%%] Filter[5] %s%s%s"
                              "Activation[1] %s ECMM[0] SSID[myssid] "
                              "RSSI[-47] NetDevType[1]",
                              FW_TYPE, CORE_PUB_LED ? "LED[0-1] LEDUser[1-100-50-0-0-255] " : "",
                              CORE_PUB_LCD ? "LcdOnoff[1] LcdSwitch[1] LcdBright[80] " : "",
                              CORE_PUB_AUDIO ? "Volume[100] " : "", want_echu)),
                "litestats byte-aligns to the oracle 24-key order");
        CHECK(!strstr(buf, "BVer[") && !strstr(buf, "MVer["),
                "litestats emits neither BVer nor MVer");
        CHECK(!strstr(buf, "POOLS["), "litestats carries no POOLS");
        CHECK(strstr(buf, "NetDevType[1],MM Count=1,Nonce Mask=25|"),
                "litestats MM ID0 renders whole and the record closes it");
        CHECK(strstr(buf, WANT_DESC "|MM ID0="),
                "litestats text drops the STATS envelope: MM ID0 follows the pipe");
        CHECK(strstr(buf, "Code=70,Msg=AvalonMiner stats,Description=" WANT_DESC "|"),
                "litestats text shares estats' Code 70 / \"AvalonMiner stats\" "
                "STATUS record with Description=" WANT_DESC);
        CHECK(!strstr(buf, "STATS="), "litestats text carries no STATS record envelope");
        CHECK(!strstr(buf, "Ver[Nano3s-"), "litestats omits the fat Ver[model-ver]");
        CHECK(!strstr(buf, "SYSTEMSTATU"), "litestats omits SYSTEMSTATU");
        CHECK(!strstr(buf, " PS["), "litestats omits the PS rail block");
        CHECK(!strstr(buf, "GHSavg["), "litestats omits GHSavg");
        CHECK(!strstr(buf, "PVT_T"), "litestats omits the per-chip block");
        CHECK(!strstr(buf, "WORKMODE["), "litestats omits the estats work-mode tail");
        CHECK(strstr(buf, "SSID[myssid] RSSI[-47] NetDevType[1]"),
                "litestats carries the mm_periph wifi identity/signal");
        {
            static char lt[MC_API_REPLY_CAP];
            mc_api_litestats_text(&ls, lt, sizeof lt);
            mc_api_litestats_(&ls, buf, sizeof buf);
            CHECK(sj_ok(buf) && sj_same_as_text(buf, lt),
                    "litestats JSON carries every MM ID0 field of the text reply with the "
                    "same value, and no other");
        }
        CHECK(strstr(buf, "\"STATS\":[{\"MMID0Summary\":{"),
                "litestats JSON opens the STATS section with the MMID0Summary object");
        CHECK(sj_index(buf, "LVer") == 0 &&
                        sj_has(buf, "LVer[26070302_abcdef0] " TEXT_FW TEXT_PVER "STATE[1]"),
                "litestats JSON keeps LVer first, PVer right after FW, flat under MMID0Summary");
        CHECK(strstr(buf, "\"MM Count\":1,\"Nonce Mask\":25}]"),
                "litestats JSON section closes with MM Count/Nonce Mask only");
        CHECK(!strstr(buf, "\"Elapsed\""), "litestats JSON drops the Elapsed envelope field");
        CHECK(!strstr(buf, "\"ID\":\"AVALON0\""),
                "litestats JSON drops the ID/AVALON0 envelope field");
        mc_api_stats_text(&ls, buf, sizeof buf, 0, 0);
        CHECK(strstr(buf, WANT_DESC "|STATS=0,ID=AVALON0,Elapsed="),
                "estats text KEEPS the STATS envelope (only litestats drops it)");
        mc_api_stats_(&ls, buf, sizeof buf, 0, 0);
        CHECK(strstr(buf, "\"STATS\":0,\"ID\":\"AVALON0\",\"Elapsed\""),
                "estats JSON KEEPS the STATS envelope (only litestats drops it)");
        {
            char ss2[80];
            snprintf(ss2, sizeof ss2, "SYSTEMSTATU[Work: In Work, Hash Board: %d]", HBOARD_COUNT);
            CHECK(sj_has(buf, ss2), "estats unchanged alongside the new litestats branch");
        }
        CHECK(sj_has(buf, "GHSspd[5000.00]"),
                "estats still emits its own %%.2f GHSspd, not the litestats %%f");
    }

    {
        mc_api_stats      hs = fixed_stats();
        static char       jfield[MC_API_MM_ID0_CAP], tfield[MC_API_MM_ID0_CAP];
        static const char AFTER_SSID[] = ",\"RSSI\":-47,\"NetDevType\":1}";
        const char       *after;
        int               rc, rcj, rct;

        snprintf(hs.ssid, sizeof hs.ssid, "%s", HOSTILE_SSID);
        hs.rssi        = -47;
        hs.net_devtype = 1;

        mc_api_litestats_(&hs, buf, sizeof buf);
        CHECK(sj_ok(buf),
                "litestats JSON with an SSID carrying a wpa \\xNN sequence, a raw quote "
                "and a control byte is still one well-formed document, MMID0Summary "
                "an object");
        rc = json_str_value(buf, "SSID", jfield, sizeof jfield, &after);
        CHECK(rc == JSTR_OK, "...and MMID0Summary.SSID is a legal JSON string (reader says: %s)",
                jstr_why(rc));
        CHECK(after && !strncmp(after, AFTER_SSID, sizeof AFTER_SSID - 1),
                "...and it closes on the quote the builder meant: the very next "
                "bytes are %s, so an SSID quote did not terminate the value early",
                AFTER_SSID);
        rcj = sj_str(buf, "SSID", tfield, sizeof tfield);
        CHECK(rc == JSTR_OK && rcj == 0 && !strcmp(jfield, hs.ssid) && !strcmp(tfield, hs.ssid),
                "...and decoding it gives back the raw ssid BYTE FOR BYTE (%d raw "
                "bytes in, %d out) — asserted as a round trip, not against a "
                "literal, because a half-escape satisfies a literal",
                (int)strlen(hs.ssid), (int)strlen(jfield));

        mc_api_litestats_text(&hs, buf, sizeof buf);
        CHECK(strstr(buf, "SSID[" HOSTILE_SSID "] RSSI[") != NULL,
                "litestats TEXT carries the SSID bytes verbatim — the app decodes "
                "the wpa \\xNN form itself, so escaping this path would break every "
                "non-ASCII SSID on the device list");
        CHECK(!strstr(buf, "\\\\x4e") && !strstr(buf, "\\u0007") && !strstr(buf, "\\\""),
                "...and specifically NONE of the three JSON escapes appears in it: "
                "no doubled backslash, no \\u00xx, no backslash-quote");

        snprintf(hs.ssid, sizeof hs.ssid, "%s", "plainAP");
        mc_api_litestats_(&hs, buf, sizeof buf);
        CHECK(sj_has(buf, "SSID[plainAP] RSSI[") && strstr(buf, "\"SSID\":\"plainAP\","),
                "a plain-ASCII SSID passes through the JSON path untouched — "
                "json_escape rewrites nothing that does not need it");
        rcj = sj_str(buf, "SSID", jfield, sizeof jfield);
        mc_api_litestats_text(&hs, buf, sizeof buf);
        rct = mm_id0_ssid(buf, tfield, sizeof tfield);
        CHECK(rcj == 0 && rct == 0 && !strcmp(jfield, tfield) && !strcmp(jfield, "plainAP"),
                "...and the two paths agree on it byte for byte (JSON \"%s\" vs "
                "text \"%s\"): the split changes the hostile case only",
                jfield, tfield);
    }

    {
        mc_api_stats  id = { 0 };
        mc_api_id_buf ib;
        const char   *want_model = !strcmp(BOARDCONF_SECTION, "NANO3S") ? "Nano3s" :
                  !strcmp(BOARDCONF_SECTION, "X3")                      ? "X3" :
                                                                          NULL;
        static char   vbuf[MC_API_REPLY_CAP];
        char          vwant[128];
        mc_api_identity_fill(&id, &ib);
        CHECK(id.prod && !strcmp(id.prod, PROD_NAME), "identity prod = " PROD_NAME);
        CHECK(id.model && !strcmp(id.model, MODEL), "identity model = " MODEL);
        CHECK(want_model != NULL,
                "descriptor [%s] has no pinned MODEL literal here — add it from that "
                "product's own source, never by guessing",
                BOARDCONF_SECTION);
        CHECK(want_model && id.model && !strcmp(want_model, id.model),
                "descriptor [%s] MODEL is exactly \"%s\" — the phone app matches "
                "MODEL= exactly when provisioning over BLE and by prefix when "
                "adding a device by IP, so a rename "
                "breaks the two paths independently and silently",
                BOARDCONF_SECTION, want_model ? want_model : "");
        if (want_model) {
            mc_api_version(&id, vbuf, sizeof vbuf);
            snprintf(vwant, sizeof vwant, "\"MODEL\":\"%s\"", want_model);
            CHECK(strstr(vbuf, vwant) != NULL, "...and the version JSON the app parses carries %s",
                    vwant);
            mc_api_version_text(&id, vbuf, sizeof vbuf);
            snprintf(vwant, sizeof vwant, ",MODEL=%s,", want_model);
            CHECK(strstr(vbuf, vwant) != NULL, "...and the version text reply carries %s", vwant);
        }
        CHECK(id.mac && strlen(id.mac) >= 12, "identity mac non-empty");
        CHECK(id.dna && strlen(id.dna) == 16, "identity dna 16 hex");
    }

    printf("%s: %d failure(s) [%s]\n", fails ? "FAILED" : "PASSED", fails, BOARDCONF_SECTION);
    return fails ? 1 : 0;
}
