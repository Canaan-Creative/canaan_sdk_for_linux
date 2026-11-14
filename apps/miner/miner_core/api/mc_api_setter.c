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

#include "mc_api_setter.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "jread.h"
#include "osif.h"

#define MC_API_SETTERS_MAX      32   /* registered setters, max count */
#define MC_API_RAW_MAX          4    /* registered raw setters, max count */
#define MC_API_DECL_MAX         16   /* declared setter attributes, max count */
#define MC_API_SET_REASON_CAP   128  /* setter Msg reason, max bytes */
#define MC_API_LOG_PREFIX_CAP   200  /* log prefix buffer, bytes */
#define MC_API_ASCSET_FIELD_CAP 4096 /* ascset subcommand or args buffer, bytes */
#define MC_API_RAW_TAIL_CAP     2200 /* raw setter tail buffer, bytes */

static struct {
    char             name[24];
    uint16_t         op;
    mc_api_setter_fn fn;
    void            *ctx;
} g_reg[MC_API_SETTERS_MAX];
static int g_n; /* registered setters, count */

static struct {
    char                 name[24];
    mc_api_setter_raw_fn fn;
    void                *ctx;
} g_raw[MC_API_RAW_MAX];
static int g_rn; /* registered raw setters, count */

static struct {
    char            name[24];
    unsigned        flags;      /* MC_API_SETTER_* */
    char            before[24]; /* help lists the setter before this name; empty: after the order */
    mc_api_reads_fn reads;
} g_decl[MC_API_DECL_MAX];
static int g_dn; /* declared setter attributes, count */

static _Thread_local int  g_forwarded; /* setter forwarded the request to mm_miner */
static _Thread_local int  g_noop;      /* setter left the request unparsed and did nothing */
static mc_api_secret_fn   g_secret;
static _Thread_local char g_log_reply[MC_API_SET_REASON_CAP];

/* subcommands whose arguments are not logged */
static const char *const g_secret_subcmds[] = {
    "password",
    "qr_auth",
    "activate",
};

/* help list order */
static const char *const g_help_order[] = {
    "ledmode",
    "ledset",
    "lcd",
    "nightlamp",
    "wallpaper",
    "workmode",
    "worklevel",
    "work_mode_lvl",
    "reboot",
    "filter-clean",
    "activate",
    "loop",
    "password",
    "qr_auth",
};

/* subcommands omitted from help */
static const char *const g_help_hidden[] = { "reboot_web" };

static int ctrl_char(char c)
{
    return (unsigned char)c < 0x20 || (unsigned char)c == 0x7f;
}

static int name_eq(const char *a, const char *b)
{
    return !strcasecmp(a, b);
}

static int name_in(const char *name, const char *const *set, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (name_eq(name, set[i]))
            return 1;
    return 0;
}

static int reg_find(const char *subcmd)
{
    if (!subcmd)
        return -1;
    for (int i = 0; i < g_n; i++)
        if (name_eq(g_reg[i].name, subcmd))
            return i;
    return -1;
}

static int raw_find(const char *subcmd)
{
    if (!subcmd)
        return -1;
    for (int i = 0; i < g_rn; i++)
        if (name_eq(g_raw[i].name, subcmd))
            return i;
    return -1;
}

static int decl_find(const char *subcmd)
{
    for (int i = 0; i < g_dn; i++)
        if (name_eq(g_decl[i].name, subcmd))
            return i;
    return -1;
}

static unsigned decl_flags(const char *subcmd)
{
    int i = decl_find(subcmd);
    return i < 0 ? 0 : g_decl[i].flags;
}

int mc_api_setter_register(const char *subcmd, uint16_t op, mc_api_setter_fn fn, void *ctx)
{
    if (!subcmd || !*subcmd || !fn)
        return -1;
    if (strlen(subcmd) >= sizeof g_reg[0].name)
        return -1;
    if (g_n >= MC_API_SETTERS_MAX)
        return -1;
    if (reg_find(subcmd) >= 0)
        return -1;
    strncpy(g_reg[g_n].name, subcmd, sizeof g_reg[0].name - 1);
    g_reg[g_n].op  = op;
    g_reg[g_n].fn  = fn;
    g_reg[g_n].ctx = ctx;
    g_n++;
    return 0;
}

mc_api_setter_fn mc_api_setter_lookup(const char *subcmd, void **ctx_out)
{
    int i = reg_find(subcmd);
    if (i < 0)
        return NULL;
    if (ctx_out)
        *ctx_out = g_reg[i].ctx;
    return g_reg[i].fn;
}

int mc_api_setter_register_raw(const char *subcmd, mc_api_setter_raw_fn fn, void *ctx)
{
    if (!subcmd || !*subcmd || !fn)
        return -1;
    if (strlen(subcmd) >= sizeof g_raw[0].name)
        return -1;
    if (g_rn >= MC_API_RAW_MAX)
        return -1;
    if (raw_find(subcmd) >= 0)
        return -1;
    strncpy(g_raw[g_rn].name, subcmd, sizeof g_raw[0].name - 1);
    g_raw[g_rn].fn  = fn;
    g_raw[g_rn].ctx = ctx;
    g_rn++;
    return 0;
}

int mc_api_setter_declare(const char *subcmd, unsigned flags, const char *help_before,
        mc_api_reads_fn reads)
{
    if (!subcmd || !*subcmd)
        return -1;
    if (strlen(subcmd) >= sizeof g_decl[0].name)
        return -1;
    if (help_before && strlen(help_before) >= sizeof g_decl[0].before)
        return -1;
    if (decl_find(subcmd) >= 0)
        return 0;
    if (g_dn >= MC_API_DECL_MAX)
        return -1;
    strncpy(g_decl[g_dn].name, subcmd, sizeof g_decl[0].name - 1);
    g_decl[g_dn].flags = flags;
    if (help_before)
        strncpy(g_decl[g_dn].before, help_before, sizeof g_decl[0].before - 1);
    g_decl[g_dn].reads = reads;
    g_dn++;
    return 0;
}

static int list_add(char *buf, size_t cap, int n, const char *name)
{
    if ((size_t)n + 1 >= cap)
        return n;
    int w = snprintf(buf + n, cap - (size_t)n, "%s|", name);
    if (w < 0)
        return n;
    return (size_t)w < cap - (size_t)n ? n + w : (int)cap - 1;
}

static int in_order(const char *name)
{
    return name_in(name, g_help_order, sizeof g_help_order / sizeof g_help_order[0]);
}

static int help_shown(const char *name)
{
    return (reg_find(name) >= 0 || raw_find(name) >= 0) &&
            !name_in(name, g_help_hidden, sizeof g_help_hidden / sizeof g_help_hidden[0]) &&
            !(decl_flags(name) & MC_API_SETTER_HIDDEN);
}

static const char *help_anchor(const char *name)
{
    int i = decl_find(name);
    if (i < 0 || in_order(name) || !in_order(g_decl[i].before))
        return NULL;
    return g_decl[i].before;
}

static int help_extra(const char *name)
{
    return !in_order(name) && !help_anchor(name) && help_shown(name);
}

int mc_api_setter_list(char *buf, size_t cap)
{
    if (!buf || !cap)
        return 0;
    buf[0] = 0;
    int n  = 0;
    for (size_t k = 0; k < sizeof g_help_order / sizeof g_help_order[0]; k++) {
        for (int d = 0; d < g_dn; d++) {
            const char *anchor = help_anchor(g_decl[d].name);
            if (anchor && name_eq(anchor, g_help_order[k]) && help_shown(g_decl[d].name))
                n = list_add(buf, cap, n, g_decl[d].name);
        }
        if (help_shown(g_help_order[k]))
            n = list_add(buf, cap, n, g_help_order[k]);
    }
    for (int i = 0; i < g_n; i++)
        if (help_extra(g_reg[i].name))
            n = list_add(buf, cap, n, g_reg[i].name);
    for (int i = 0; i < g_rn; i++)
        if (help_extra(g_raw[i].name) && reg_find(g_raw[i].name) < 0)
            n = list_add(buf, cap, n, g_raw[i].name);
    return n;
}

void mc_api_setter_mark_forwarded(void)
{
    g_forwarded = 1;
}

void mc_api_setter_mark_noop(void)
{
    g_noop = 1;
}

void mc_api_setter_log_reply(const char *text)
{
    snprintf(g_log_reply, sizeof g_log_reply, "%s", text ? text : "");
}

void mc_api_setter_reset(void)
{
    g_n = 0;
    memset(g_reg, 0, sizeof g_reg);
    g_rn = 0;
    memset(g_raw, 0, sizeof g_raw);
    g_dn = 0;
    memset(g_decl, 0, sizeof g_decl);
    g_secret = NULL;
}

void mc_api_setter_set_secret(mc_api_secret_fn fn)
{
    g_secret = fn;
}

const char *mc_api_args_redact(const char *args, char *out, size_t cap)
{
    size_t n = 0;

    if (!cap)
        return "";
    out[0] = 0;
    for (const char *f = args; f;) {
        const char *comma = strchr(f, ',');
        size_t      len   = comma ? (size_t)(comma - f) : strlen(f);
        int         hide  = len && g_secret && g_secret(f, len);
        int         w = snprintf(out + n, cap - n, "%.*s%s", hide ? 3 : (int)len, hide ? "xxx" : f,
                comma ? "," : "");
        if (w < 0 || (size_t)w >= cap - n)
            break;
        n += (size_t)w;
        f = comma ? comma + 1 : NULL;
    }
    for (size_t i = 0; i < n; i++)
        if (ctrl_char(out[i]))
            out[i] = '?';
    return out;
}

static int ascset_payload(const char *req, char *out, size_t cap)
{
    const char *body = req + strspn(req, " \t\r\n");
    size_t      n    = 0;

    if (*body == '{') {
        json_value       *root = json_parse(body);
        const json_value *par  = json_object_get(root, "parameter");
        int               rc   = -1;
        if (json_is(par, JSON_STRING)) {
            n  = strlen(json_string(par));
            rc = n < cap ? 0 : -2;
            if (rc == 0)
                memcpy(out, json_string(par), n + 1);
        }
        json_free(root);
        return rc;
    }
    const char *v = strchr(body, '|');
    if (!v)
        return -1;
    for (v++; v[n] && v[n] != '|' && !ctrl_char(v[n]); n++)
        if (n + 1 >= cap)
            return -2;
    memcpy(out, v, n);
    out[n] = 0;
    return 0;
}

static void payload_log_prefix(char *dst, size_t cap, const char *v)
{
    size_t n = 0, commas = 0;

    if (!cap)
        return;
    while (v[n] && n + 1 < cap && !ctrl_char(v[n])) {
        char ch = v[n];
        dst[n]  = (unsigned char)ch < 0x80 ? ch : '?';
        n++;
        if (ch == ',' && ++commas == 2)
            break;
    }
    dst[n] = 0;
}

static int raw_extract(const char *v, char *subcmd, size_t scap, char *tail, size_t tcap,
        int *asc_out)
{
    if (!subcmd || !tail || scap == 0 || tcap == 0)
        return -1;
    if (asc_out)
        *asc_out = 0;
    const char *p   = v;
    int         idx = 0;
    while (*p >= '0' && *p <= '9' && idx < 1000) {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != ',' || p == v)
        return -1;
    if (asc_out)
        *asc_out = idx;
    p++;

    const char *sstart = p;
    while (*p && *p != ',' && !ctrl_char(*p))
        p++;
    size_t sn = (size_t)(p - sstart);
    if (sn == 0)
        return -1;
    if (sn >= scap)
        return -2;
    memcpy(subcmd, sstart, sn);
    subcmd[sn] = 0;
    if (*p != ',') {
        tail[0] = 0;
        return 0;
    }
    p++;

    const char *tstart = p;
    while (*p && !ctrl_char(*p))
        p++;
    size_t tn = (size_t)(p - tstart);
    if (tn >= tcap)
        return -3;
    memcpy(tail, tstart, tn);
    tail[tn] = 0;
    return 0;
}

static int parse_payload(const char *v, char *subcmd, size_t scap, char *args, size_t acap,
        int *asc_out)
{
    const char *end = v + strlen(v);
    const char *c1  = memchr(v, ',', (size_t)(end - v));
    if (!c1)
        return -1;
    if (asc_out) {
        *asc_out        = 0;
        int         val = 0;
        const char *d   = v;
        while (d < c1 && *d >= '0' && *d <= '9' && val < 1000) {
            val = val * 10 + (*d - '0');
            d++;
        }
        if (d == c1 && d != v)
            *asc_out = val;
    }
    const char *sub    = c1 + 1;
    const char *c2     = memchr(sub, ',', (size_t)(end - sub));
    size_t      sublen = c2 ? (size_t)(c2 - sub) : (size_t)(end - sub);
    if (sublen == 0)
        return -1;
    for (size_t i = 0; i < sublen; i++)
        if (ctrl_char(sub[i]))
            return -1;
    if (sublen >= scap) {
        osif_log(OSIF_LOG_WARN, "API setting name is %zu bytes, at most %zu allowed", sublen,
                scap - 1);
        return -2;
    }
    memcpy(subcmd, sub, sublen);
    subcmd[sublen] = 0;

    if (c2) {
        const char *a  = c2 + 1;
        size_t      al = (size_t)(end - a);
        if (al >= acap) {
            osif_log(OSIF_LOG_WARN, "API setting %s: arguments are %zu bytes, at most %zu allowed",
                    subcmd, al, acap - 1);
            return -2;
        }
        memcpy(args, a, al);
        args[al] = 0;
    } else {
        args[0] = 0;
    }
    return 0;
}

int mc_api_parse_ascset(const char *req, char *subcmd, size_t scap, char *args, size_t acap,
        int *asc_out)
{
    char pay[MC_API_ASCSET_FIELD_CAP];

    if (!req || !subcmd || !args || scap == 0 || acap == 0)
        return -1;
    int rc = ascset_payload(req, pay, sizeof pay);
    return rc ? rc : parse_payload(pay, subcmd, scap, args, acap, asc_out);
}

int mc_api_setter_always_reads(const char *args)
{
    (void)args;
    return 1;
}

int mc_api_setter_reads(const char *req)
{
    char subcmd[MC_API_ASCSET_FIELD_CAP], args[MC_API_ASCSET_FIELD_CAP];
    int  d;

    if (mc_api_parse_ascset(req, subcmd, sizeof subcmd, args, sizeof args, NULL) != 0)
        return 0;
    if (name_eq(subcmd, "help"))
        return 1;
    d = decl_find(subcmd);
    return d >= 0 && g_decl[d].reads && g_decl[d].reads(args);
}

void mc_api_msg_safe(char *msg, size_t msgcap)
{
    for (size_t i = 0; i < msgcap && msg[i]; i++) {
        char c = msg[i];
        if (c == '"' || c == '\\' || c == '|' || c == ',' || (unsigned char)c < 0x20 ||
                (unsigned char)c == 0x7f)
            msg[i] = '?';
    }
}

char mc_set_letter(mc_set_result r)
{
    if (r == MC_SET_OK)
        return 'S';
    if (r == MC_SET_INFO)
        return 'I';
    return 'E';
}

int mc_set_code(mc_set_result r)
{
    if (r == MC_SET_OK)
        return 119;
    if (r == MC_SET_INFO)
        return 118;
    return 120;
}

static mc_set_result finish(int asc, mc_set_result r, char *msg, size_t msgcap)
{
    if (!msgcap)
        return r;
    msg[msgcap - 1] = 0;
    if (r == MC_SET_OK) {
        snprintf(msg, msgcap, "ASC %d set OK", asc);
        return r;
    }
    mc_api_msg_safe(msg, msgcap);
    char reason[160];
    snprintf(reason, sizeof reason, "%s", msg);
    snprintf(msg, msgcap, "ASC %d set %s: %s", asc, r == MC_SET_INFO ? "info" : "failed", reason);
    return r;
}

mc_set_result mc_api_setter_run(const char *req, const mc_api_stats *s, char *msg, size_t msgcap)
{
    size_t reqlen = req ? strlen(req) : 0;

    if (msgcap)
        msg[0] = 0;

    size_t rcap = msgcap > MC_API_SET_REASON_CAP ? (size_t)MC_API_SET_REASON_CAP : msgcap;

    osif_log(OSIF_LOG_DBG, "API setting request received (%zu bytes)", reqlen);

    char pay[MC_API_ASCSET_FIELD_CAP];
    int  drc = req ? ascset_payload(req, pay, sizeof pay) : -1;
    if (drc != 0)
        pay[0] = 0;

    {
        char rsub[24], peek[1];
        int  rasc = 0;
        int  prc  = -1;
        if (g_rn > 0 && drc == 0)
            prc = raw_extract(pay, rsub, sizeof rsub, peek, sizeof peek, &rasc);
        if (prc == 0 || prc == -3) {
            int ri = raw_find(rsub);
            if (ri >= 0) {
                char tail[MC_API_RAW_TAIL_CAP];
                int  trc = raw_extract(pay, rsub, sizeof rsub, tail, sizeof tail, &rasc);
                if (trc != 0) {
                    if (rcap)
                        snprintf(msg, rcap, "parameter too long");
                    osif_log(OSIF_LOG_WARN,
                            "API setting %s (unit %d) refused: arguments "
                            "longer than %zu bytes",
                            rsub, rasc, sizeof tail - 1);
                    return finish(rasc, MC_SET_ERR_ARG, msg, rcap);
                }
                osif_log(OSIF_LOG_DBG,
                        "API setting %s (unit %d) handled by its own "
                        "parser, %zu bytes of arguments",
                        rsub, rasc, strlen(tail));
                return finish(rasc, g_raw[ri].fn(tail, rasc, s, msg, rcap, g_raw[ri].ctx), msg,
                        rcap);
            }
            osif_log(OSIF_LOG_DBG,
                    "API setting %s (unit %d) uses the standard "
                    "argument parser",
                    rsub, rasc);
        }
    }

    char subcmd[MC_API_ASCSET_FIELD_CAP], args[MC_API_ASCSET_FIELD_CAP];
    char shown[MC_API_ARGS_LOG_CAP];
    int  asc = 0;
    int  prc = drc ? drc : parse_payload(pay, subcmd, sizeof subcmd, args, sizeof args, &asc);
    if (prc != 0) {
        char prefix[MC_API_LOG_PREFIX_CAP];
        payload_log_prefix(prefix, sizeof prefix, pay);
        if (rcap)
            snprintf(msg, rcap, prc == -2 ? "parameter too long" : "missing subcommand");
        osif_log(OSIF_LOG_WARN,
                "API setting request refused: %s (request %zu bytes, "
                "limits: subcommand %zu, arguments %zu; starts \"%s\")",
                prc == -2 ? "parameter too long" : "missing subcommand", reqlen, sizeof subcmd - 1,
                sizeof args - 1, mc_api_args_redact(prefix, shown, sizeof shown));
        return finish(asc, MC_SET_ERR_ARG, msg, rcap);
    }
    int i = reg_find(subcmd);
    if (i < 0 && !name_eq(subcmd, "help"))
        osif_log(OSIF_LOG_DBG, "API setting %s for unit %d, unknown, arguments hidden (%zu bytes)",
                subcmd, asc, strlen(args));
    else if (name_in(subcmd, g_secret_subcmds,
                     sizeof g_secret_subcmds / sizeof g_secret_subcmds[0]) ||
            (decl_flags(subcmd) & MC_API_SETTER_SECRET))
        osif_log(OSIF_LOG_DBG,
                "API setting %s for unit %d, arguments hidden "
                "(%zu bytes)",
                subcmd, asc, strlen(args));
    else
        osif_log(OSIF_LOG_DBG,
                "API setting %s for unit %d, arguments (%zu "
                "bytes): %s",
                subcmd, asc, strlen(args), mc_api_args_redact(args, shown, sizeof shown));
    if (name_eq(subcmd, "help")) {
        char list[512];
        mc_api_setter_list(list, sizeof list);
        if (msgcap)
            snprintf(msg, msgcap, "ASC %d set info: help|%s", asc, list);
        return MC_SET_INFO;
    }
    if (i < 0) {
        if (rcap && snprintf(msg, rcap, "Unknown option: %s", subcmd) < 0)
            msg[0] = '\0';
        osif_log(OSIF_LOG_WARN, "API setting %s is not supported on this device", subcmd);
        return finish(asc, MC_SET_ERR_UNSUP, msg, rcap);
    }
    g_forwarded = 0;
    g_noop      = 0;
    mc_set_result rr =
            finish(asc, g_reg[i].fn(g_reg[i].op, args, s, msg, rcap, g_reg[i].ctx), msg, rcap);
    if (!g_noop) {
        char logged[MC_API_SET_MSG_CAP];
        snprintf(logged, sizeof logged, "%s", msgcap ? msg : "");
        if (g_log_reply[0] && rcap) {
            snprintf(logged, rcap, "%s", g_log_reply);
            finish(asc, rr, logged, rcap);
        }
        osif_log(OSIF_LOG_OP, "API setting %s %s (code %d: %s)", subcmd,
                mc_set_letter(rr) == 'E' ? "refused" :
                        g_forwarded      ? "forwarded to mm_miner" :
                                           "accepted",
                mc_set_code(rr), logged);
    }
    g_log_reply[0] = 0;
    return rr;
}
