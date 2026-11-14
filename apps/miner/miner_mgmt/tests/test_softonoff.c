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

#include "softonoff.h"
#include "enum_hold.h"
#include "softonoff_sched.h"
#include "boardconf.h"
#include <stdio.h>

#include "../../tests/include/tst_check.h"

_Static_assert(_Generic(&((softonoff_state_t *)0)->thermal_until, _Atomic uint64_t *: 1, default: 0),
        "thermal_until is written by the mm supervisor thread and read by "
        "the mm watchdog thread, so it must be _Atomic: a plain member "
        "gives the watchdog gate no happens-before edge to the softoff "
        "that is supposed to exempt it");
_Static_assert(_Generic(&((softonoff_state_t *)0)->api_softoff, _Atomic int *: 1, default: 0),
        "api_softoff is written by the mm supervisor thread and read by the "
        "mm watchdog thread, so it must be _Atomic — same reason as "
        "thermal_until above");

#define HOLD_MS  180000u /* Thermal softoff hold, ms */
#define TICK_MS  1000u   /* Model supervisor tick period, ms */
#define DUTY_PID (-1)    /* Fan duty marker: the fan follows the PID loop */

#if defined(BOARD_X3_H)
#define BOARD_NAME   "x3"
#define WANT_SOFTOFF 100 /* Expected thermal softoff fan duty, % */
#define WANT_MIN     20  /* Expected API softoff fan duty, % */
#define WANT_ENUM    80  /* Expected enumeration fan duty, %; 0 = no enumeration pin */
#elif defined(BOARD_NANO3S_H)
#define BOARD_NAME   "nano3s"
#define WANT_SOFTOFF 25 /* Expected thermal softoff fan duty, % */
#define WANT_MIN     10 /* Expected API softoff fan duty, % */
#define WANT_ENUM    0  /* Expected enumeration fan duty, %; 0 = no enumeration pin */
#else
#error "no softoff fan duty goldens for this board"
#endif

#define DUTY_SOFTOFF FAN_DUTY_SOFTOFF_PCT     /* Thermal softoff fan duty, % */
#define DUTY_MIN     FAN_DUTY_API_SOFTOFF_PCT /* API softoff fan duty, % */
#define DUTY_ENUM    FAN_DUTY_ENUM_PCT        /* Enumeration fan duty, % */
/* Expected fan duty while the chain enumerates */
#define DUTY_RESPAWN   (WANT_ENUM > 0 ? WANT_ENUM : DUTY_PID)
#define WANT_PIN_COUNT (WANT_ENUM > 0 ? 1 : 0) /* Expected enumeration pins per enumeration */

/* Model child state: not running, running, exited and not yet reaped */
enum { DOWN = 0, UP = 1, DYING = 2 };

/* Model supervisor state advanced by sup_tick() */
typedef struct {
    softonoff_state_t softonoff; /* Softoff state under test */
    enum_hold_t       enum_hold;
    soft_sched_t      sched;           /* Scheduled softon/softoff */
    int               overheat_req;    /* Pending thermal request; cleared once acted on */
    int               api_req;         /* Pending API request: 1 softon, -1 softoff, 0 none */
    int               core, asic;      /* core_miner and asic_miner state, DOWN/UP/DYING */
    int               rail;            /* Power rail: 1 on, 0 off */
    int               fan;             /* Fan duty, %, or DUTY_PID */
    int               asic_live;       /* 1 while asic_miner telemetry is live */
    int               respawns, reaps; /* Unintended exits respawned; exits reaped */
    int               ignored;         /* API requests softonoff_api_step() ignored */
    int               held, sched_skipped;
    /* Ticks the softoff fan hold held the fan; ticks the fan stepped */
    int fan_holds, fan_steps;
    /* Enumeration pins, releases, and ticks held at DUTY_ENUM */
    int enum_pins, enum_releases, enum_holds;
} sup_t;

static void asic_up(sup_t *s)
{
    enum_hold_rearm(&s->enum_hold);
    s->asic = UP;
}

static int sup_held_off(const sup_t *s)
{
    return s->softonoff.thermal_until || s->softonoff.api_softoff;
}

static void sup_tick(sup_t *s, uint64_t now)
{
    if (s->core == DYING || s->asic == DYING) {
        int is_asic = (s->core != DYING);
        s->reaps++;
        if (softonoff_intentional_exit(&s->softonoff, is_asic)) {
            if (is_asic)
                s->asic = DOWN;
            else
                s->core = DOWN;
        } else {
            s->respawns++;
            if (is_asic)
                asic_up(s);
            else
                s->core = UP;
        }
        return;
    }

    softonoff_action pa = softonoff_thermal_step(&s->softonoff, s->overheat_req, now, HOLD_MS);
    if (pa != SOFTONOFF_NONE)
        s->overheat_req = 0;
    if (pa == SOFTONOFF_THERMAL_OFF) {
        if (s->asic == UP)
            s->asic = DYING;
        s->rail = 0;
        s->fan  = DUTY_SOFTOFF;
    } else if (pa == SOFTONOFF_THERMAL_TO_SOFTOFF || pa == SOFTONOFF_THERMAL_RESUME) {
        if (s->core == DOWN)
            s->core = UP;
        if (pa == SOFTONOFF_THERMAL_TO_SOFTOFF) {
            s->fan = DUTY_MIN;
        } else {
            s->fan  = DUTY_PID;
            s->rail = 1;
            if (s->asic == DOWN)
                asic_up(s);
        }
    }

    enum_hold_action ea = enum_hold_step(&s->enum_hold, s->asic_live, DUTY_ENUM);
    if (ea == ENUM_HOLD_PIN) {
        s->enum_pins++;
        s->fan = DUTY_ENUM;
    } else if (ea == ENUM_HOLD_RELEASE) {
        s->enum_releases++;
        s->fan = DUTY_PID;
    }

    if (softonoff_fan_hold(&s->softonoff, 0))
        s->fan_holds++;
    else if (enum_hold_active(&s->enum_hold, sup_held_off(s))) {
        s->enum_holds++;
        s->fan = DUTY_ENUM;
    } else
        s->fan_steps++;
    {
        soft_sched_action a =
                soft_sched_due(&s->sched, (int64_t)(now / 1000), !s->softonoff.api_softoff);
        if (a == SOFT_SCHED_ON && s->softonoff.thermal_until)
            s->sched_skipped++;
        else if (a == SOFT_SCHED_ON)
            s->api_req = 1;
        else if (a == SOFT_SCHED_OFF)
            s->api_req = -1;
    }

    if (s->api_req) {
        int want_on         = s->api_req > 0;
        s->api_req          = 0;
        softonoff_action ap = softonoff_api_step(&s->softonoff, want_on);
        if (ap == SOFTONOFF_API_IGNORED) {
            s->ignored++;
        } else if (ap == SOFTONOFF_API_OFF_HELD) {
            s->held++;
        } else if (ap == SOFTONOFF_API_OFF) {
            if (s->asic == UP)
                s->asic = DYING;
            s->rail = 0;
            s->fan  = DUTY_MIN;
        } else if (ap == SOFTONOFF_API_ON) {
            s->fan  = DUTY_PID;
            s->rail = 1;
            if (s->asic == DOWN)
                asic_up(s);
        }
    }
}

static sup_t sup_new(void)
{
    sup_t s = { 0 };
    s.core  = UP;
    asic_up(&s);
    s.rail            = 1;
    s.fan             = DUTY_PID;
    s.asic_live       = 1;
    s.enum_hold.phase = ENUM_HOLD_PHASE_NONE;
    return s;
}

static sup_t sup_boot(void)
{
    sup_t s = sup_new();
    enum_hold_rearm(&s.enum_hold);
    s.asic_live = 0;
    return s;
}

static uint64_t sup_run(sup_t *s, uint64_t now, uint64_t ms)
{
    uint64_t end = now + ms;
    for (; now < end; now += TICK_MS)
        sup_tick(s, now);
    return now;
}

int main(void)
{
    {
        sup_t    s = sup_new();
        uint64_t t = sup_run(&s, TICK_MS, 600000);
        (void)t;
        CHECK(s.core == UP && s.asic == UP && s.rail == 1 && s.fan == DUTY_PID,
                "healthy: both children up, rail up, fan on the PID");
        CHECK(s.fan_holds == 0, "healthy: the fan gate never holds");
        CHECK(s.respawns == 0 && s.reaps == 0, "healthy: nothing is respawned");
        CHECK(s.enum_pins == 0 && s.enum_holds == 0,
                "healthy: the enum pin is not armed once the chain is reporting");
    }

    {
        printf("board: %s (enum %d%%, softoff %d%%, min %d%%)\n", BOARD_NAME, DUTY_ENUM,
                DUTY_SOFTOFF, DUTY_MIN);
        CHECK(DUTY_ENUM == WANT_ENUM, "%s enum-pin duty is %d%%, want %d%%", BOARD_NAME, DUTY_ENUM,
                WANT_ENUM);
        CHECK(DUTY_SOFTOFF == WANT_SOFTOFF, "%s thermal soft-off pin is %d%%, want %d%%",
                BOARD_NAME, DUTY_SOFTOFF, WANT_SOFTOFF);
        CHECK(DUTY_MIN == WANT_MIN, "%s fan min is %d%%, want %d%%", BOARD_NAME, DUTY_MIN,
                WANT_MIN);
    }
    {
        sup_t    s = sup_boot();
        uint64_t t = TICK_MS;
        sup_tick(&s, t);
        CHECK(s.enum_pins == WANT_PIN_COUNT && s.fan == DUTY_RESPAWN,
                "%s: enumeration pins the fan on the first iteration (%d pin(s), "
                "duty %d)",
                BOARD_NAME, s.enum_pins, s.fan);

        t = sup_run(&s, t + TICK_MS, 600000);
        CHECK(s.enum_pins == WANT_PIN_COUNT && s.enum_releases == 0,
                "%s: ...pinned once, released never while the chain is silent", BOARD_NAME);
        CHECK(s.fan == DUTY_RESPAWN, "%s: ...and the duty stays there — no timeout", BOARD_NAME);
        CHECK(s.enum_holds == (WANT_ENUM > 0 ? 601 : 0),
                "%s: ...with the fan rule actuating it every tick (holds=%d), since "
                "a blind board would otherwise only HOLD its old duty",
                BOARD_NAME, s.enum_holds);

        s.asic_live = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.enum_releases == WANT_PIN_COUNT && s.fan == DUTY_PID,
                "%s: the heartbeat releases the pin to the PID (releases=%d)", BOARD_NAME,
                s.enum_releases);
        t = sup_run(&s, t + TICK_MS, 600000);
        CHECK(s.enum_pins == WANT_PIN_COUNT && s.enum_releases == WANT_PIN_COUNT &&
                        s.fan == DUTY_PID,
                "%s: ...once, and it does not re-arm itself while the child lives", BOARD_NAME);
    }
    {
        sup_t s    = sup_new();
        s.asic     = DYING;
        uint64_t t = TICK_MS;
        sup_tick(&s, t);
        CHECK(s.respawns == 1 && s.asic == UP, "a crashed ASIC child is respawned");
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.enum_pins == WANT_PIN_COUNT && s.enum_releases == 0 && s.fan == DUTY_RESPAWN,
                "%s: the dead child's stale heartbeat does NOT release the pin", BOARD_NAME);
        t = sup_run(&s, t + TICK_MS, 10000);
        CHECK(s.enum_releases == 0, "%s: ...for as long as it lingers", BOARD_NAME);
        s.asic_live = 0;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.enum_releases == 0, "%s: ...and a 0 alone releases nothing", BOARD_NAME);
        s.asic_live = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.enum_releases == WANT_PIN_COUNT && s.fan == DUTY_PID,
                "%s: the first heartbeat AFTER that 0 is this child's — released", BOARD_NAME);
    }
    {
        sup_t    s = sup_boot();
        uint64_t t = TICK_MS;
        for (int i = 0; i < 5; i++) {
            sup_tick(&s, t);
            t += TICK_MS;
            s.asic = DYING;
            sup_tick(&s, t);
            t += TICK_MS;
        }
        CHECK(s.respawns == 5 && s.enum_releases == 0,
                "an enum that never succeeds: 5 respawns, 0 releases");
        CHECK(s.enum_pins == (WANT_ENUM > 0 ? 5 : 0) && s.fan == DUTY_RESPAWN,
                "%s: ...and the pin is re-armed by every spawn (pins=%d)", BOARD_NAME, s.enum_pins);
    }
    {
        sup_t    s = sup_boot();
        uint64_t t = TICK_MS;
        sup_tick(&s, t);
        CHECK(s.fan == DUTY_RESPAWN, "%s: pinned for enum...", BOARD_NAME);
        s.api_req = -1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.fan == DUTY_MIN && !enum_hold_active(&s.enum_hold, sup_held_off(&s)),
                "...a soft-off mid-enum REPLACES the pin with the fan policy's min and the enum "
                "gate stands down");
        int holds = s.enum_holds;
        t         = sup_run(&s, t + TICK_MS, 60000);
        CHECK(s.enum_holds == holds && s.fan == DUTY_MIN,
                "...and stays down for the whole soft-off (the soft-off's own fan gate wins)");
        s.api_req = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.fan == DUTY_PID,
                "...a soft-on releases to the PID for one tick "
                "(the API block runs AFTER the rule tick)");
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.fan == DUTY_RESPAWN,
                "%s: ...and the re-enumeration pins it again on the next iteration", BOARD_NAME);
    }
    {
        sup_t    s = sup_boot();
        uint64_t t = TICK_MS;
        sup_tick(&s, t);
        s.overheat_req = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.fan == DUTY_SOFTOFF && !enum_hold_active(&s.enum_hold, sup_held_off(&s)),
                "a thermal trip mid-enum REPLACES the pin with the thermal soft-off's own duty");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        CHECK(s.core == UP && s.asic == DYING && s.rail == 0 && s.fan == DUTY_SOFTOFF &&
                        s.softonoff.thermal_until == t + HOLD_MS,
                "thermal trip: the ASIC child is killed, rail off, fan pinned, hold "
                "armed — and core_miner is LEFT RUNNING, so :4028 keeps answering "
                "and reports the over-hot soft-off");
        CHECK(s.overheat_req == 0, "...and the request is consumed");

        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.core == UP && s.asic == DOWN && s.respawns == 0,
                "...the ASIC child is not respawned during the hold (killed on "
                "purpose) and core_miner never left");

        s.asic_live      = 0;
        int steps_before = s.fan_steps, holds_before = s.fan_holds;
        t = sup_run(&s, t + TICK_MS, HOLD_MS - 10000);
        CHECK(s.core == UP && s.rail == 0, "...and nothing restarts early");
        CHECK(s.fan_steps == steps_before && s.fan_holds > holds_before && s.fan == DUTY_SOFTOFF,
                "...and the fan gate HOLDS for every tick of the hold (%d holds, %d "
                "steps), so the thermal soft-off's %d%% pin is what the board runs at for the "
                "whole "
                "%u ms — the stale-telemetry fail-safe never gets to command "
                "FAN_DUTY_MAX_PCT over it",
                s.fan_holds - holds_before, s.fan_steps - steps_before, DUTY_SOFTOFF,
                (unsigned)HOLD_MS);

        t = sup_run(&s, t, 12000);
        CHECK(s.core == UP && s.asic == UP && s.rail == 1 && s.fan == DUTY_RESPAWN,
                "hold expires with no soft-off: rail up, the ASIC child back, fan to "
                "the PID — or to the enum pin, on a board that has one, since the "
                "respawned child re-enumerates the chain");
        CHECK(s.softonoff.thermal_until == 0, "...and the hold is cleared");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        t              = sup_run(&s, t + TICK_MS, 60000);
        s.overheat_req = 1;
        t              = sup_run(&s, t, 10000);
        CHECK(s.softonoff.thermal_until != 0 && s.overheat_req == 1,
                "a hot sample mid-hold neither re-trips nor is consumed");
        t = sup_run(&s, t, HOLD_MS);
        CHECK(s.softonoff.thermal_until == 0 && s.overheat_req == 0 && s.rail == 1,
                "...it is DISCARDED at expiry: the miner resumes, flag cleared");
        s.overheat_req = 1;
        sup_tick(&s, t);
        CHECK(s.softonoff.thermal_until != 0, "...and a fresh hot sample trips again");
    }

    {
        sup_t    s = sup_new();
        uint64_t t = 1000;
        s.api_req  = -1;
        sup_tick(&s, t);
        CHECK(s.softonoff.api_softoff == 1 && s.asic == DYING && s.core == UP && s.rail == 0 &&
                        s.fan == DUTY_MIN,
                "API soft-off: ASIC child only, rail off, fan to min, :4028 stays up");

        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.asic == DOWN && s.respawns == 0, "...the stopped ASIC child is not respawned");

        s.asic_live = 0;
        t           = sup_run(&s, t + TICK_MS, 3600000);
        CHECK(s.fan_holds > 0 && s.fan == DUTY_MIN,
                "...and the fan gate holds the soft-off's min duty for the whole soft-off");

        s.core = DYING;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.core == UP && s.respawns == 1,
                "core_miner dying during a soft-off IS respawned (we did not kill it)");

        s.api_req = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.softonoff.api_softoff == 0 && s.asic == UP && s.rail == 1 && s.fan == DUTY_PID,
                "API soft-on: rail up, ASIC child back, fan released to the PID");
    }

    {
        sup_t    s = sup_new();
        uint64_t t = 1000;
        s.api_req  = -1;
        sup_tick(&s, t);
        t += TICK_MS;
        sup_tick(&s, t);
        s.overheat_req = 1;
        s.asic_live    = 0;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.softonoff.thermal_until != 0 && s.softonoff.api_softoff == 1,
                "a thermal trip on top of a soft-off: BOTH latches now hold");
        CHECK(s.core == UP, "...and the thermal path leaves core_miner alone");

        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.core == UP && s.respawns == 0,
                "...core_miner is still up in the hold, so :4028 can report the "
                "over-hot soft-off and take the soft-on that ends it");
        CHECK(s.fan_holds > 0, "...the fan gate holds (the soft-off latch is up)");

        t = sup_run(&s, t + TICK_MS, HOLD_MS + 2 * TICK_MS);
        CHECK(s.softonoff.thermal_until == 0, "hold expires...");
        CHECK(s.softonoff.api_softoff == 1, "...THE SOFT-OFF SURVIVES the thermal trip");
        CHECK(s.asic == DOWN && s.rail == 0,
                "...so the ASIC child and the rail STAY DOWN rather than "
                "restarting a user soft-offed miner");
        CHECK(s.core == UP,
                "...and core_miner never went away, so :4028 answered "
                "throughout and can soft-on");
        CHECK(s.fan == DUTY_MIN,
                "...and the duty goes from the overheat pin back to the soft-off's min");

        s.api_req = 1;
        t += TICK_MS;
        sup_tick(&s, t);
        CHECK(s.softonoff.api_softoff == 0 && s.asic == UP && s.rail == 1 && s.fan == DUTY_PID,
                "...a soft-on after the hold releases the soft-off normally");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        t = sup_run(&s, t + TICK_MS, 30000);

        s.api_req = -1;
        t         = sup_run(&s, t, TICK_MS);
        CHECK(s.held == 1 && s.ignored == 0 && s.softonoff.api_softoff == 1 &&
                        s.softonoff.thermal_until != 0,
                "an immediate soft-off inside a thermal hold is LATCHED, the hold "
                "keeps running");
        CHECK(s.rail == 0 && s.fan == DUTY_SOFTOFF,
                "...and actuates nothing: rail stays off, fan stays on the hold's pin");

        s.api_req = 1;
        t         = sup_run(&s, t, TICK_MS);
        CHECK(s.ignored == 1 && s.softonoff.api_softoff == 1 && s.softonoff.thermal_until != 0 &&
                        s.rail == 0,
                "an immediate soft-on inside the hold is still IGNORED: the latch "
                "stays and the hold is not cut short");

        t = sup_run(&s, t, HOLD_MS);
        CHECK(s.softonoff.thermal_until == 0 && s.softonoff.api_softoff == 1 && s.rail == 0 &&
                        s.asic == DOWN && s.fan == DUTY_MIN,
                "...the hold ends into the API soft-off: rail and ASIC child stay "
                "down, fan to the soft-off's min");
        t = sup_run(&s, t, 600000);
        CHECK(s.rail == 0 && s.asic == DOWN, "...and mining never resumes on its own");

        s.api_req = 1;
        t         = sup_run(&s, t, TICK_MS);
        CHECK(s.softonoff.api_softoff == 0 && s.asic == UP && s.rail == 1 && s.fan == DUTY_PID,
                "...a soft-on after the hold resumes mining");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        soft_sched_arm(&s.sched, 0, 1, 60);
        t = sup_run(&s, t + TICK_MS, 70000);
        CHECK(s.softonoff.thermal_until != 0 && s.sched.off_valid == 0 && s.held == 1 &&
                        s.softonoff.api_softoff == 1,
                "a scheduled soft-off due inside a thermal hold is consumed and "
                "LATCHED, the hold keeps running");
        t = sup_run(&s, t, HOLD_MS);
        CHECK(s.softonoff.thermal_until == 0 && s.softonoff.api_softoff == 1 && s.rail == 0 &&
                        s.asic == DOWN,
                "...the hold ends into the soft-off: the miner does not resume");
        t = sup_run(&s, t, 600000);
        CHECK(s.rail == 0 && s.asic == DOWN, "...and stays down");
        s.api_req = 1;
        t         = sup_run(&s, t, TICK_MS);
        CHECK(s.softonoff.api_softoff == 0 && s.rail == 1 && s.asic == UP,
                "...until a soft-on resumes mining");
    }

    {
        sup_t    s = sup_new();
        uint64_t t = 1000;
        s.api_req  = -1;
        sup_tick(&s, t);
        s.overheat_req = 1;
        t              = sup_run(&s, t + TICK_MS, TICK_MS);
        soft_sched_arm(&s.sched, 1, 1, 60);
        t = sup_run(&s, t, 70000);
        CHECK(s.softonoff.thermal_until != 0 && s.sched.on_valid == 0 && s.sched_skipped == 1 &&
                        s.softonoff.api_softoff == 1 && s.rail == 0,
                "a scheduled soft-on due inside a thermal hold is still dropped: "
                "the hold is not cut short and the soft-off stays");
        t = sup_run(&s, t, HOLD_MS);
        CHECK(s.softonoff.api_softoff == 1 && s.rail == 0 && s.asic == DOWN,
                "...and the hold ends into the soft-off");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        soft_sched_arm(&s.sched, 0, 1, 200);
        t = sup_run(&s, t + TICK_MS, HOLD_MS + 30000);
        CHECK(s.softonoff.api_softoff == 1 && s.asic == DOWN && s.rail == 0,
                "a schedule due after the hold expires fires and soft-offs the miner");
    }

    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        uint64_t expiry = s.softonoff.thermal_until;
        t               = sup_run(&s, t + TICK_MS, expiry - t - TICK_MS);
        CHECK(s.softonoff.thermal_until != 0, "one tick before expiry: still holding");
        s.api_req = -1;
        sup_tick(&s, expiry);
        CHECK(s.softonoff.thermal_until == 0 && s.softonoff.api_softoff == 1 && s.ignored == 0,
                "a press on the expiry tick applies: the thermal block releases first");
        CHECK(s.rail == 0 && s.fan == DUTY_MIN,
                "...so the resume is immediately soft-offed again, at the soft-off's min duty");
    }
    {
        sup_t    s     = sup_new();
        uint64_t t     = 1000;
        s.overheat_req = 1;
        sup_tick(&s, t);
        uint64_t expiry = s.softonoff.thermal_until;
        t               = sup_run(&s, t + TICK_MS, expiry - t - 2 * TICK_MS);
        s.api_req       = -1;
        sup_tick(&s, expiry - TICK_MS);
        CHECK(s.held == 1 && s.softonoff.api_softoff == 1 && s.ignored == 0,
                "the same press one tick earlier is latched (thermal still holds)");
        sup_tick(&s, expiry);
        CHECK(s.softonoff.thermal_until == 0 && s.rail == 0 && s.fan == DUTY_MIN,
                "...and the expiry tick keeps the miner soft-offed");
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(!softonoff_intentional_exit(&p, 0) && !softonoff_intentional_exit(&p, 1),
                "no latch: any child exit is a crash -> respawn");
        p.api_softoff = 1;
        CHECK(softonoff_intentional_exit(&p, 1), "in soft-off: the ASIC child exit is intentional");
        CHECK(!softonoff_intentional_exit(&p, 0), "in soft-off: a core_miner exit is NOT");
        p = (softonoff_state_t){ .thermal_until = 12345 };
        CHECK(softonoff_intentional_exit(&p, 1),
                "thermal hold: the ASIC child was killed on purpose");
        CHECK(!softonoff_intentional_exit(&p, 0),
                "thermal hold: a core_miner exit is NOT — the thermal soft-off does not kill it, "
                "so "
                "an exit during the hold is a CRASH and must be respawned, or :4028 "
                "stays dead for the rest of the hold");
        p.api_softoff = 1;
        CHECK(softonoff_intentional_exit(&p, 1) && !softonoff_intentional_exit(&p, 0),
                "both latches: the ASIC child is intentional, core_miner is still a crash");
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(!softonoff_fan_hold(&p, 0), "mining: the fan rule runs (PID + fail-safe)");
        p.api_softoff = 1;
        CHECK(softonoff_fan_hold(&p, 0), "in soft-off: hold the pinned duty, re-arm the grace");
        p = (softonoff_state_t){ .thermal_until = 1 };
        CHECK(softonoff_fan_hold(&p, 0),
                "a thermal hold DOES gate the fan rule: the thermal soft-off stops the ASIC child "
                "too, so its absent heartbeat is not a stall either, and the "
                "fail-safe would otherwise command FAN_DUTY_MAX_PCT over "
                "FAN_DUTY_SOFTOFF_PCT — on nano3s, where the pin is 25%%, the duty "
                "would go to 100%% mid-hold; only on x3 are the two constants "
                "both 100");
        p.api_softoff = 1;
        CHECK(softonoff_fan_hold(&p, 0), "...and a soft-off underneath it gates too");
        p = (softonoff_state_t){ .start_wait = 1 };
        CHECK(softonoff_fan_hold(&p, 0),
                "cold-start start-wait: no ASIC was spawned, so hold the pinned duty "
                "(same class as api_softoff — the absent heartbeat is not a stall)");

        for (unsigned i = 0; i < 16; i++) {
            int fault_off   = (i & 8) ? 1 : 0;
            p               = (softonoff_state_t){ 0 };
            p.thermal_until = (i & 1) ? 1234 : 0;
            p.api_softoff   = (i & 2) ? 1 : 0;
            p.start_wait    = (i & 4) ? 1 : 0;
            int want        = (i != 0);
            int hold        = softonoff_fan_hold(&p, fault_off);
            int stopped = (softonoff_children_stopped(&p, fault_off) & SOFTONOFF_CHILD_ASIC) != 0;
            CHECK(hold == want && stopped == want,
                    "latches thermal=%d api=%d start_wait=%d fault=%d: fan gate %d and "
                    "ASIC-stopped %d, want %d for both — one predicate, so a latch "
                    "kind cannot reach one and miss the other",
                    !!(i & 1), !!(i & 2), !!(i & 4), fault_off, hold, stopped, want);
        }
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(softonoff_children_stopped(&p, 0) == 0,
                "mining, nothing softed off -> no child is stopped, so every "
                "watchdog gate bit has to be earned");

        p = (softonoff_state_t){ .api_softoff = 1 };
        CHECK(softonoff_children_stopped(&p, 0) == SOFTONOFF_CHILD_ASIC,
                "the user's softoff stops asic_miner ONLY — core_miner and :4028 "
                "stay up so the softon can arrive over them, so core_miner is "
                "still supervised");

        p = (softonoff_state_t){ 0 };
        CHECK(softonoff_children_stopped(&p, 1) == SOFTONOFF_CHILD_ASIC,
                "a fault softoff (hash fault / power fault) stops asic_miner ONLY "
                "and refuses to respawn it — same scope as the user's softoff, so "
                "a board that never enumerates sits in soft-off instead of reboot-looping");

        p = (softonoff_state_t){ .thermal_until = 1 };
        CHECK(softonoff_children_stopped(&p, 0) == SOFTONOFF_CHILD_ASIC,
                "the thermal softoff stops asic_miner ONLY — core_miner stays up "
                "so :4028 reports the over-hot trip, so NO softoff kind stops "
                "core_miner and a core_miner that wedges inside a thermal hold is "
                "still detected and still starves the gate");

        p = (softonoff_state_t){ .thermal_until = 1, .api_softoff = 1 };
        CHECK(softonoff_children_stopped(&p, 1) == SOFTONOFF_CHILD_ASIC,
                "...and overlapping softoffs are a UNION: the widest scope in "
                "force wins, never the last one set — which is still the ASIC bit "
                "alone, because every kind has the same scope today");

        p = (softonoff_state_t){ .start_wait = 1 };
        CHECK(softonoff_children_stopped(&p, 0) == SOFTONOFF_CHILD_ASIC,
                "the cold-start delay stops asic_miner ONLY — core_miner stays up "
                "through it, and the ASIC bit keeps the watchdog feed gate "
                "satisfied so the absent child never starves the watchdog");
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(softonoff_api_step(&p, 1) == SOFTONOFF_NONE && p.api_softoff == 0,
                "soft-on while already mining: nothing");
        CHECK(softonoff_api_step(&p, 0) == SOFTONOFF_API_OFF && p.api_softoff == 1,
                "soft-off stops the asic child");
        CHECK(softonoff_api_step(&p, 0) == SOFTONOFF_NONE && p.api_softoff == 1,
                "soft-off while already in soft-off: nothing (and still off)");
        CHECK(softonoff_api_step(&p, 1) == SOFTONOFF_API_ON && p.api_softoff == 0,
                "soft-on releases");

        p = (softonoff_state_t){ .thermal_until = 1 };
        CHECK(softonoff_api_step(&p, 1) == SOFTONOFF_API_IGNORED && p.api_softoff == 0,
                "thermal hold: soft-on ignored");
        CHECK(softonoff_api_step(&p, 0) == SOFTONOFF_API_OFF_HELD && p.api_softoff == 1,
                "thermal hold: soft-off latched without actuation");
        CHECK(softonoff_api_step(&p, 0) == SOFTONOFF_NONE && p.api_softoff == 1,
                "thermal hold: soft-off while already latched: nothing");
        CHECK(softonoff_api_step(&p, 1) == SOFTONOFF_API_IGNORED && p.api_softoff == 1,
                "thermal hold: soft-on ignored, latch kept");
        CHECK(softonoff_thermal_step(&p, 0, 1, 0) == SOFTONOFF_THERMAL_TO_SOFTOFF,
                "...the hold ends into the soft-off");
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(softonoff_api_check(&p, 0, 1) == SOFTONOFF_NONE &&
                        softonoff_api_check(&p, 0, 0) == SOFTONOFF_API_OFF,
                "the check answers what the step would do while mining");
        CHECK(p.api_softoff == 0, "...and changes nothing");

        p.api_softoff = 1;
        CHECK(softonoff_api_check(&p, 0, 1) == SOFTONOFF_API_ON &&
                        softonoff_api_check(&p, 0, 0) == SOFTONOFF_NONE,
                "the check answers what the step would do in an API soft-off");
        CHECK(softonoff_api_check(&p, 1, 1) == SOFTONOFF_API_ON,
                "a soft-on after a soft-off releases a latched fault too");

        p.api_softoff = 0;
        CHECK(softonoff_api_check(&p, 1, 1) == SOFTONOFF_API_FAULT_HELD,
                "a soft-on alone during a fault stop is a distinct verdict: the fault stays");
        CHECK(softonoff_api_check(&p, 1, 0) == SOFTONOFF_API_OFF,
                "a soft-off during a fault stop is applied (the first half of the release)");
        CHECK(softonoff_api_step(&p, 1) == SOFTONOFF_NONE && p.api_softoff == 0,
                "the step itself never sees the fault: a lone soft-on does nothing");

        p.thermal_until = 5000;
        CHECK(softonoff_api_check(&p, 0, 1) == SOFTONOFF_API_IGNORED &&
                        softonoff_api_check(&p, 1, 1) == SOFTONOFF_API_IGNORED,
                "a soft-on inside a thermal hold is IGNORED, as the step does");
        CHECK(softonoff_api_check(&p, 0, 0) == SOFTONOFF_API_OFF_HELD && p.api_softoff == 0,
                "a soft-off inside a thermal hold is OFF_HELD, as the step does, and "
                "the check latches nothing");
    }

    {
        softonoff_state_t p = { 0 };
        CHECK(softonoff_thermal_step(&p, 1, 1000, 0) == SOFTONOFF_THERMAL_OFF && p.thermal_until,
                "hold_ms 0 still arms a hold (the expiry test is >=, not >)");
        CHECK(softonoff_thermal_step(&p, 0, 1000, 0) == SOFTONOFF_THERMAL_RESUME,
                "...and releases on the same ms");
    }

    if (fails) {
        fprintf(stderr, "test_softonoff: %d FAIL\n", fails);
        return 1;
    }
    printf("test_softonoff: all pass\n");
    return 0;
}
