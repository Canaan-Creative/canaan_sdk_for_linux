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

#include "ctrl_args.h"
#include "ctrl_cmds.h"
#include "cali.h"
#include "led_fx.h"
#include <string.h>

#define SAT_INT_MAX 9999999 /* magnitude ctrl_scan_int_saturating() clamps to */

#define WORKMODE_SPAN_FITS(lo, hi) \
    _Static_assert(0 <= (lo) && (lo) <= (hi) && (hi) < CALI_MODE_MAX, \
            "BOARD_WORKMODE_RANGES span outside the cali store");
#define WORKMODE_SPAN(lo, hi) (v >= (lo) && v <= (hi)) ||

BOARD_WORKMODE_RANGES(WORKMODE_SPAN_FITS)

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return c;
}

static const char *skip_prefix_ci(const char *s, const char *kw)
{
    for (; *kw; s++, kw++) {
        if (ascii_lower(*s) != *kw)
            return NULL;
    }
    return s;
}

static int streq_ci(const char *s, const char *kw)
{
    const char *end = skip_prefix_ci(s, kw);

    return end && !*end;
}

static const char *skip_set_kw(const char *args)
{
    const char *p = skip_prefix_ci(args, "set");

    if (p && (*p == ',' || *p == '-'))
        return p + 1;
    return args;
}

static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

int ctrl_scan_dec(const char **pp, uint32_t max, uint32_t *out)
{
    const char *p = *pp;
    uint32_t    v = 0;

    if (*p < '0' || *p > '9')
        return -1;
    do {
        v = v * 10 + (uint32_t)(*p++ - '0');
        if (v > max)
            return -1;
    } while (*p >= '0' && *p <= '9');
    *out = v;
    *pp  = p;
    return 0;
}

int ctrl_scan_int(const char **pp, int *out)
{
    const char *p   = *pp;
    int         neg = 0;
    uint32_t    v;

    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if (ctrl_scan_dec(&p, 1000, &v) != 0)
        return -1;
    *out = neg ? -(int)v : (int)v;
    *pp  = p;
    return 0;
}

int ctrl_scan_int_saturating(const char **pp, int *out)
{
    const char *p   = skip_blanks(*pp);
    int         neg = 0;
    int         v   = 0;

    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > SAT_INT_MAX)
            v = SAT_INT_MAX;
        p++;
    }
    *out = neg ? -v : v;
    *pp  = p;
    return 0;
}

static int scan_u32(const char **pp, uint32_t *out)
{
    const char *p    = *pp;
    uint32_t    base = 10;
    uint32_t    v    = 0;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }

    const char *start = p;

    for (;; p++) {
        uint32_t d;

        if (*p >= '0' && *p <= '9')
            d = (uint32_t)(*p - '0');
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = (uint32_t)(*p - 'a' + 10);
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = (uint32_t)(*p - 'A' + 10);
        else
            break;
        if (v > (0xFFFFFFFFu - d) / base)
            return -1;
        v = v * base + d;
    }
    if (p == start)
        return -1;
    *out = v;
    *pp  = p;
    return 0;
}

static int scan_i64(const char **pp, int64_t *out)
{
    const char *p = *pp;
    int64_t     v = 0;

    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9') {
        if (v > (INT64_MAX - (*p - '0')) / 10)
            return -1;
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    *pp  = p;
    return 0;
}

static int scan_dotted_quad(const char **pp, uint32_t *out)
{
    const char *p    = *pp;
    uint32_t    addr = 0;

    for (int octet = 0; octet < 4; octet++) {
        const char *start;
        uint32_t    v;

        if (octet > 0) {
            if (*p != '.')
                return -1;
            p++;
        }
        start = p;
        if (ctrl_scan_dec(&p, 255, &v) != 0 || p - start > 3)
            return -1;
        addr = (addr << 8) | v;
    }
    *out = addr;
    *pp  = p;
    return 0;
}

static int scan_ipv4_text(const char **pp, char *out)
{
    const char *start = *pp;
    uint32_t    addr;

    if (scan_dotted_quad(pp, &addr) != 0)
        return -1;

    size_t n = (size_t)(*pp - start);

    if (n >= CTRL_IP_STRLEN)
        return -1;
    memcpy(out, start, n);
    out[n] = '\0';
    return 0;
}

static int workmode_allowed(int v)
{
    return BOARD_WORKMODE_RANGES(WORKMODE_SPAN) 0;
}

int ctrl_parse_workmode(const char *args, uint8_t *mode)
{
    const char *p;
    int         v;

    if (!args || !mode)
        return -1;
    p = skip_set_kw(args);
    if (ctrl_scan_int(&p, &v) != 0 || *p)
        return -1;
    if (!workmode_allowed(v))
        return -2;
    *mode = (uint8_t)v;
    return 0;
}

_Static_assert(CALI_LEVEL_COUNT == 1,
        "worklevel accepts only level 0; widen it together with the cali store");

int ctrl_parse_worklevel(const char *args, int *level)
{
    const char *p;
    int         v;

    if (!args || !level)
        return -1;
    p = skip_set_kw(args);
    if (ctrl_scan_int(&p, &v) != 0 || *p || v != 0)
        return -1;
    *level = 0;
    return 0;
}

int ctrl_parse_mode_level(const char *args, uint8_t *mode, int *level)
{
    const char *p;
    int         a, b, m;

    if (!args || !mode || !level)
        return -1;
    p = skip_set_kw(args);
    if (ctrl_scan_int(&p, &a) != 0)
        return -1;
    if (*p != ',' && *p != '-')
        return -1;
    p++;
    if (ctrl_scan_int(&p, &b) != 0 || *p)
        return -1;

    if (a == 0 && workmode_allowed(b))
        m = b;
    else if (b == 0 && a > 0 && workmode_allowed(a))
        m = a;
    else
        return -1;
    *mode  = (uint8_t)m;
    *level = 0;
    return 0;
}

int ctrl_parse_lcd(const char *args, uint16_t *id, uint16_t *value)
{
    const char *p = args;
    uint32_t    lcd_id, val;

    if (!args || !id || !value)
        return -1;
    if (ctrl_scan_dec(&p, UINT16_MAX, &lcd_id) != 0 || *p != ':')
        return -1;
    p++;
    if (ctrl_scan_dec(&p, UINT16_MAX, &val) != 0 || *p)
        return -1;
    if (lcd_id == 0 && val > 1)
        return -1;
    *id    = (uint16_t)lcd_id;
    *value = (uint16_t)val;
    return 0;
}

int ctrl_parse_ledmode(const char *args, uint8_t *mode)
{
    const char *p = args;
    uint32_t    v;

    if (!args || !mode)
        return -1;
    if (ctrl_scan_dec(&p, 1, &v) != 0 || *p)
        return -1;
    *mode = (uint8_t)v;
    return 0;
}

int ctrl_parse_ledset(const char *args, uint8_t *effect, uint8_t *bright, uint32_t *temper,
        uint32_t *rgb)
{
    /* upper bound of each field: effect, brightness, temper, r, g, b */
    static const uint32_t max[] = { LED_FX_USER_MAX, 100, 0xFFFFu, 255, 255, 255 };
    uint32_t              f[sizeof max / sizeof max[0]];
    const char           *p = args;

    if (!args || !effect || !bright || !temper || !rgb)
        return -1;
    for (size_t i = 0; i < sizeof max / sizeof max[0]; i++) {
        if (i && *p++ != '-')
            return -1;
        if (ctrl_scan_dec(&p, max[i], &f[i]) != 0)
            return -1;
    }
    if (*p)
        return -1;
    *effect = (uint8_t)f[0];
    *bright = (uint8_t)f[1];
    *temper = f[2];
    *rgb    = (f[3] << 16) | (f[4] << 8) | f[5];
    return 0;
}

int ctrl_parse_nightlamp(const char *args, uint8_t *mode, uint32_t *duration_s)
{
    const char *p = args;
    uint32_t    m, d;

    if (!args || !mode || !duration_s)
        return -1;
    if (ctrl_scan_dec(&p, 1, &m) != 0 || *p++ != '-')
        return -1;
    if (ctrl_scan_dec(&p, 0xFFFFu, &d) != 0 || *p)
        return -1;
    *mode       = (uint8_t)m;
    *duration_s = d;
    return 0;
}

int ctrl_parse_ip(const char *args, ctrl_ip_t *out)
{
    const char *p;

    if (!args || !out)
        return -1;
    out->is_static = 0;
    out->ip[0]     = '\0';
    out->mask[0]   = '\0';
    out->gw[0]     = '\0';
    out->dns[0]    = '\0';

    if (streq_ci(args, "dhcp"))
        return 0;

    p = skip_prefix_ci(args, "static");
    if (!p || *p != ',')
        return -1;
    p++;
    if (scan_ipv4_text(&p, out->ip) != 0 || *p != ',')
        return -1;
    p++;
    if (scan_ipv4_text(&p, out->mask) != 0 || *p != ',')
        return -1;
    p++;
    if (scan_ipv4_text(&p, out->gw) != 0)
        return -1;
    if (*p == ',') {
        p++;
        if (scan_ipv4_text(&p, out->dns) != 0)
            return -1;
    }
    if (*p)
        return -1;
    out->is_static = 1;
    return 0;
}

int ctrl_ipv4_parse(const char *s, uint32_t *out)
{
    const char *p = s;
    uint32_t    v;

    if (!s || !out)
        return -1;
    if (!*s) {
        *out = 0;
        return 0;
    }
    if (scan_dotted_quad(&p, &v) != 0 || *p)
        return -1;
    *out = v;
    return 0;
}

static size_t ip_octet_to_dec(unsigned v, char *out)
{
    char   tmp[3];
    size_t n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

void ctrl_ipv4_fmt(uint32_t v, char *out, size_t cap)
{
    char   buf[CTRL_IP_STRLEN];
    size_t n = 0;

    if (!out || cap == 0)
        return;
    for (int shift = 24; shift >= 0; shift -= 8) {
        n += ip_octet_to_dec((v >> shift) & 0xff, buf + n);
        if (shift > 0)
            buf[n++] = '.';
    }
    if (n > cap - 1)
        n = cap - 1;
    memcpy(out, buf, n);
    out[n] = '\0';
}

static int ctrl_parse_softsched(const char *args, int *immediate, uint8_t *valid, int64_t *ts)
{
    const char *p;
    int64_t     a, b;

    if (!args || !immediate || !valid || !ts)
        return -1;
    p = skip_set_kw(args);
    if (!*p || streq_ci(p, "now")) {
        *immediate = 1;
        return 0;
    }
    *immediate = 0;
    if (ascii_lower(p[0]) == 't' && p[1] == ':')
        p += 2;

    if (scan_i64(&p, &a) != 0)
        return -1;
    if (!*p) {
        *valid = 1;
        *ts    = a;
        return 0;
    }
    if (*p != ',' && *p != '-')
        return -1;
    p++;
    if (scan_i64(&p, &b) != 0 || *p)
        return -1;
    if (a > 1)
        return -1;
    *valid = (uint8_t)a;
    *ts    = b;
    return 0;
}

static void copy_text(char *dst, const char *args)
{
    size_t i = 0;

    if (args) {
        for (; args[i] && i < CTRL_TEXT_LEN - 1; i++)
            dst[i] = args[i];
    }
    dst[i] = '\0';
}

static int parse_reboot_cmd(const char *args, mc_ctrl_cmd *out)
{
    const char *p = args ? args : "";
    uint32_t    reason, sub = 0, delay = 0;

    if (scan_u32(&p, &reason) != 0 || reason > 0xff)
        return -1;
    if (*p == ',' || *p == '-') {
        p++;
        if (scan_u32(&p, &sub) != 0)
            return -1;
    }
    if (*p == ',' || *p == '-') {
        p++;
        if (ctrl_scan_dec(&p, CTRL_REBOOT_DELAY_MAX, &delay) != 0)
            return -1;
    }
    if (*p)
        return -1;
    out->v.reboot.reason  = (uint8_t)reason;
    out->v.reboot.sub     = sub;
    out->v.reboot.delay_s = (uint16_t)delay;
    return 0;
}

int ctrl_parse_cmd(uint16_t op, const char *args, mc_ctrl_cmd *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    out->op = op;

    switch (op) {
    case CTRL_OP_WORKMODE:
        return ctrl_parse_workmode(args, &out->v.workmode.mode);
    case CTRL_OP_LEDMODE:
        return ctrl_parse_ledmode(args, &out->v.ledmode.mode);
    case CTRL_OP_LEDSET: {
        uint8_t  effect, bright;
        uint32_t temper, rgb;

        if (ctrl_parse_ledset(args, &effect, &bright, &temper, &rgb) != 0)
            return -1;
        out->v.ledset.effect = effect;
        out->v.ledset.bright = bright;
        out->v.ledset.temper = (uint16_t)temper;
        out->v.ledset.r      = (uint8_t)(rgb >> 16);
        out->v.ledset.g      = (uint8_t)(rgb >> 8);
        out->v.ledset.b      = (uint8_t)rgb;
        return 0;
    }
    case CTRL_OP_NIGHTLAMP:
        return ctrl_parse_nightlamp(args, &out->v.nightlamp.mode, &out->v.nightlamp.duration_s);
    case CTRL_OP_SOFTON:
    case CTRL_OP_SOFTOFF: {
        int immediate;

        if (ctrl_parse_softsched(args, &immediate, &out->v.soft.valid, &out->v.soft.ts) != 0)
            return -1;
        out->v.soft.immediate = immediate;
        return 0;
    }
    case CTRL_OP_LCD: {
        uint16_t id, value;

        if (ctrl_parse_lcd(args, &id, &value) != 0 || strlen(args) >= CTRL_TEXT_LEN)
            return -1;
        copy_text(out->v.text, args);
        return 0;
    }
    case CTRL_OP_FILTER_CLEAN:
    case CTRL_OP_SETTZ:
        if (args && strlen(args) >= CTRL_TEXT_LEN)
            return -1;
        copy_text(out->v.text, args);
        return 0;
    case CTRL_OP_WP_RELOAD:
        return 0;
    case CTRL_OP_REBOOT:
        return parse_reboot_cmd(args, out);
    default:
        return CTRL_PLUGIN_PARSE(op, args, out);
    }
}
