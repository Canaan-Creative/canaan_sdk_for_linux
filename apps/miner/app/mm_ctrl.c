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

#include "mm_internal.h"
#include "mm_ctrl.h"
#include "ctrl_args.h"
#include "miner_ipc.h"
#include "mm_display.h"
#include "mm_led.h"
#include "mm_net.h"
#include "mm_tz.h"
#include "mm_ui_glue.h"
#include "osif.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CTRL_REBOOT_DELAY_SEC 2 /* delay when the request gives none, s */

#define CTRL_ROUTE_ROW(e, n, p, c)   [CTRL_OP_##e] = CTRL_ROUTE_##e,
#define CTRL_PAYLOAD_ROW(e, n, p, c) [CTRL_OP_##e] = #p,

static mc_set_result route_led(uint16_t op, const mc_ctrl_cmd *c);
static mc_set_result route_filter_clean(uint16_t op, const mc_ctrl_cmd *c);
static mc_set_result route_lcd(uint16_t op, const mc_ctrl_cmd *c);
static mc_set_result route_settz(uint16_t op, const mc_ctrl_cmd *c);
static mc_set_result route_wp_reload(uint16_t op, const mc_ctrl_cmd *c);
static mc_set_result route_reboot(uint16_t op, const mc_ctrl_cmd *c);

/*
 * Pending reboot. g_reboot_at is a CLOCK_MONOTONIC deadline in ms, 0 when
 * none; g_reboot_gen lets the sleeping thread of a superseded request see
 * that it no longer owns the deadline.
 */
static pthread_mutex_t g_reboot_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_reboot_at;
static uint16_t        g_reboot_gen;
static uint8_t         g_reboot_reason;
static uint32_t        g_reboot_sub;
static int             g_reboot_took_over;

/* refusal reason of the command mm_ctrl_cmd() is applying, enum mc_ctrl_why */
static uint16_t g_ctrl_why;
static int64_t  g_ctrl_arg;

/* Op-indexed handler table; CTRL_OP_NONE has none. */
static const ctrl_apply_fn g_ctrl_route[CTRL_OP_COUNT] = { CTRL_CMD_LIST(CTRL_ROUTE_ROW) };

static const char *const g_ctrl_payload[CTRL_OP_COUNT] = { CTRL_CMD_LIST(CTRL_PAYLOAD_ROW) };

static mc_set_result route_led(uint16_t op, const mc_ctrl_cmd *c)
{
    char args[48];
    switch (op) {
    case CTRL_OP_LEDMODE:
        snprintf(args, sizeof args, "%u", c->v.ledmode.mode);
        break;
    case CTRL_OP_LEDSET:
        snprintf(args, sizeof args, "%u-%u-%u-%u-%u-%u", c->v.ledset.effect, c->v.ledset.bright,
                c->v.ledset.temper, c->v.ledset.r, c->v.ledset.g, c->v.ledset.b);
        break;
    default:
        snprintf(args, sizeof args, "%u-%u", c->v.nightlamp.mode,
                (unsigned)c->v.nightlamp.duration_s);
        break;
    }
    return mm_led_apply(op, args);
}

static mc_set_result route_filter_clean(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    (void)c;
    filtermesh_clean();
    return MC_SET_OK;
}

static mc_set_result route_lcd(uint16_t op, const mc_ctrl_cmd *c)
{
    uint16_t id, value;

    (void)op;
    if (ctrl_parse_lcd(c->v.text, &id, &value) != 0)
        return MC_SET_ERR_ARG;
    return mm_display_lcd((uint16_t)id, (uint16_t)value) == 0 ? MC_SET_OK : MC_SET_ERR_HW;
}

static mc_set_result route_settz(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    return mm_apply_settz(c->v.text);
}

static mc_set_result route_wp_reload(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    (void)c;
    mm_ui_wallpaper_reload();
    return MC_SET_OK;
}

static void *mm_reboot_thread(void *arg)
{
    uint16_t        gen = (uint16_t)(uintptr_t)arg;
    uint64_t        at;
    uint8_t         reason;
    uint32_t        sub;
    int             took_over;
    struct timespec ts;

    pthread_mutex_lock(&g_reboot_lock);
    at = g_reboot_gen == gen ? g_reboot_at : 0;
    pthread_mutex_unlock(&g_reboot_lock);
    if (!at)
        return NULL;
    ts.tv_sec  = (time_t)(at / 1000u);
    ts.tv_nsec = (long)(at % 1000u) * 1000000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;

    pthread_mutex_lock(&g_reboot_lock);
    if (!g_reboot_at || g_reboot_gen != gen) {
        pthread_mutex_unlock(&g_reboot_lock);
        return NULL;
    }
    reason    = g_reboot_reason;
    sub       = g_reboot_sub;
    took_over = g_reboot_took_over;
    pthread_mutex_unlock(&g_reboot_lock);

    if (mm_system_reboot(reason, sub) != 0) {
        pthread_mutex_lock(&g_reboot_lock);
        if (g_reboot_gen == gen)
            g_reboot_at = 0;
        pthread_mutex_unlock(&g_reboot_lock);
        osif_log(OSIF_LOG_ERR, "Reboot on request failed; request dropped");
        if (took_over)
            osif_log(OSIF_LOG_WARN,
                    "The later reboot request it superseded "
                    "is dropped as well");
    }
    return NULL;
}

static mc_set_result route_reboot(uint16_t op, const mc_ctrl_cmd *c)
{
    unsigned  wait = c->v.reboot.delay_s ? c->v.reboot.delay_s : CTRL_REBOOT_DELAY_SEC;
    uint64_t  at, prev_at;
    uint16_t  gen, prev_gen;
    uint8_t   prev_reason;
    uint32_t  prev_sub;
    int       prev_took_over;
    pthread_t t;

    (void)op;
    if (c->v.reboot.delay_s > CTRL_REBOOT_DELAY_MAX)
        return MC_SET_ERR_ARG;
    at = osif_now_ms() + (uint64_t)wait * 1000u;

    pthread_mutex_lock(&g_reboot_lock);
    if (g_reboot_at && g_reboot_at <= at) {
        pthread_mutex_unlock(&g_reboot_lock);
        osif_log(OSIF_LOG_INFO, "Reboot in %u s ignored: an earlier reboot is already scheduled",
                wait);
        return MC_SET_OK;
    }
    prev_at            = g_reboot_at;
    prev_gen           = g_reboot_gen;
    prev_reason        = g_reboot_reason;
    prev_sub           = g_reboot_sub;
    prev_took_over     = g_reboot_took_over;
    g_reboot_took_over = prev_at != 0;
    g_reboot_at        = at;
    g_reboot_gen = gen = (uint16_t)(prev_gen + 1);
    g_reboot_reason    = c->v.reboot.reason;
    g_reboot_sub       = c->v.reboot.sub;
    pthread_mutex_unlock(&g_reboot_lock);

    mm_cfg_flush();
    osif_log(OSIF_LOG_INFO, "reboot requested by API, rebooting in %u s (reason code 0x%02x)", wait,
            c->v.reboot.reason);
    if (pthread_create(&t, NULL, mm_reboot_thread, (void *)(uintptr_t)gen) != 0) {
        pthread_mutex_lock(&g_reboot_lock);
        if (g_reboot_gen == gen) {
            g_reboot_at        = prev_at;
            g_reboot_gen       = prev_gen;
            g_reboot_reason    = prev_reason;
            g_reboot_sub       = prev_sub;
            g_reboot_took_over = prev_took_over;
        }
        pthread_mutex_unlock(&g_reboot_lock);
        osif_log(OSIF_LOG_ERR, "Reboot not scheduled: thread start failed");
        return MC_SET_ERR_HW;
    }
    pthread_detach(t);
    return MC_SET_OK;
}

mc_set_result mm_ctrl_apply(const mc_ctrl_cmd *c)
{
    if (c->op < CTRL_OP_COUNT && g_ctrl_route[c->op])
        return g_ctrl_route[c->op](c->op, c);
    return MC_SET_ERR_UNSUP;
}

static void scrub(char *sd, size_t cap)
{
    sd[cap - 1] = 0;
    for (size_t i = 0; i < cap && sd[i]; i++)
        if ((unsigned char)sd[i] < 0x20 || (unsigned char)sd[i] == 0x7f)
            sd[i] = '?';
}

static int ctrl_cmd_is_text(uint16_t op)
{
    return op < CTRL_OP_COUNT && g_ctrl_payload[op] && !strcmp(g_ctrl_payload[op], "text");
}

static int ctrl_take_cmd(const void *data, int len, mc_ctrl_cmd *out)
{
    if (!data || len != (int)sizeof(mc_ctrl_cmd))
        return 0;

    memcpy(out, data, sizeof *out);
    if (ctrl_cmd_is_text(out->op))
        scrub(out->v.text, sizeof out->v.text);
    return 1;
}

static int ctrl_describe(const mc_ctrl_cmd *c, char *buf, size_t cap)
{
    int r;

    switch (c->op) {
    case CTRL_OP_WORKMODE:
        snprintf(buf, cap, "mode %u level %d", c->v.workmode.mode, (int)c->v.workmode.level);
        break;
    case CTRL_OP_LEDMODE:
        snprintf(buf, cap, "%u", c->v.ledmode.mode);
        break;
    case CTRL_OP_LEDSET:
        snprintf(buf, cap, "%u-%u-%u-%u-%u-%u", c->v.ledset.effect, c->v.ledset.bright,
                c->v.ledset.temper, c->v.ledset.r, c->v.ledset.g, c->v.ledset.b);
        break;
    case CTRL_OP_NIGHTLAMP:
        snprintf(buf, cap, "%u-%u", c->v.nightlamp.mode, (unsigned)c->v.nightlamp.duration_s);
        break;
    case CTRL_OP_SOFTON:
    case CTRL_OP_SOFTOFF:
        if (c->v.soft.immediate)
            snprintf(buf, cap, "now");
        else
            snprintf(buf, cap, "at %lld", (long long)c->v.soft.ts);
        break;
    case CTRL_OP_REBOOT:
        if (c->v.reboot.delay_s)
            snprintf(buf, cap, "in %u s, reason 0x%02x", c->v.reboot.delay_s, c->v.reboot.reason);
        else
            snprintf(buf, cap, "default delay, reason 0x%02x", c->v.reboot.reason);
        break;
    default:
        r = CTRL_PLUGIN_DESCRIBE(c, buf, cap);
        if (r == CTRL_DESCRIBE_LOGGED)
            return 0;
        if (!r)
            snprintf(buf, cap, "%.*s", CTRL_TEXT_LEN, c->v.text);
        break;
    }
    return 1;
}

static const char *ctrl_result_word(int32_t result)
{
    switch (result) {
    case MC_SET_OK:
        return "done";
    case MC_SET_ERR_ARG:
        return "refused: invalid argument";
    case MC_SET_ERR_UNSUP:
        return "refused: not supported";
    case MC_SET_ERR_HW:
        return "failed";
    case MC_SET_INFO:
        return "answered";
    case MC_SET_ERR_STATE:
        return "refused: not in this state";
    default:
        return "unknown result";
    }
}

void mm_ctrl_refuse(uint16_t why, int64_t arg)
{
    g_ctrl_why = why;
    g_ctrl_arg = arg;
}

void mm_ctrl_cmd(const void *data, int len)
{
    mc_ctrl_cmd cmd;
    mc_ctrl_ack ack = { 0 };
    char        what[96];

    if (!ctrl_take_cmd(data, len, &cmd))
        return;

    mm_ctrl_refuse(MC_CTRL_WHY_NONE, 0);
    ack.result = MC_SET_ERR_UNSUP;
    if (ctrl_cmd_available(cmd.op))
        ack.result = mm_ctrl_apply(&cmd);
    ack.seq = cmd.seq;
    ack.why = g_ctrl_why;
    ack.arg = g_ctrl_arg;
    if (ctrl_describe(&cmd, what, sizeof what))
        osif_log(OSIF_LOG_OP, "control command %s (%s): %s", ctrl_cmd_name(cmd.op), what,
                ctrl_result_word(ack.result));
    mm_periph_push();
    osif_ipc_send(g_ipc_core_mm, MSG_CTRL_ACK, &ack, (int)sizeof ack);
}
