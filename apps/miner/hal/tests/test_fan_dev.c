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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fan_dev.h"
#include "board_tables.h"
#include "boardconf.h"
#include "pwm_sys.h"
#include "tst_tach.h"

#undef PWM_ROOT
#define PWM_ROOT TST_PWM_ROOT /* sysfs PWM root overridden to the test's TST_PWM_ROOT */

#define FAN_PERIOD_NS 40000 /* expected PWM period, ns */

#if defined(BOARD_X3_H)
#if FAN_PWM_ACTIVE_LOW
#define BOARD_NAME         "x3 (3 fans: PWM1 + PWM4 + PWM5, active-low)"
#define WANT_ACTIVE_LOW    1     /* 1 when the fan PWM is expected active low */
#define WANT_NS_AT_60PCT   16000 /* expected duty_cycle at 60 percent duty, ns */
#define WANT_PCT_AT_6600NS 84    /* expected duty read back from a 6600 ns duty_cycle, percent */
#else                            /* FAN_PWM_ACTIVE_LOW */
#define BOARD_NAME         "x3 (3 fans: PWM1 + PWM4 + PWM5)"
#define WANT_ACTIVE_LOW    0     /* 1 when the fan PWM is expected active low */
#define WANT_NS_AT_60PCT   24000 /* expected duty_cycle at 60 percent duty, ns */
#define WANT_PCT_AT_6600NS 16    /* expected duty read back from a 6600 ns duty_cycle, percent */
#endif                           /* FAN_PWM_ACTIVE_LOW */
#define WANT_MIN_PCT     20      /* expected lowest fan duty, percent */
#define WANT_MAX_PCT     100     /* expected highest fan duty, percent */
#define WANT_STARTUP_PCT 60      /* expected startup fan duty, percent */
#define WANT_TACH_PPR    4       /* expected tachometer pulses per revolution */
#define WANT_RPM_AT_43HZ 645     /* expected speed at 43 Hz tachometer input, RPM */
#define WANT_RPM_AT_32HZ 480     /* expected speed at 32 Hz tachometer input, RPM */
#elif defined(BOARD_NANO3S_H)
#define BOARD_NAME         "nano3s (1 fan: PWM3)"
#define WANT_MIN_PCT       10    /* expected lowest fan duty, percent */
#define WANT_MAX_PCT       100   /* expected highest fan duty, percent */
#define WANT_STARTUP_PCT   25    /* expected startup fan duty, percent */
#define WANT_ACTIVE_LOW    0     /* 1 when the fan PWM is expected active low */
#define WANT_NS_AT_60PCT   24000 /* expected duty_cycle at 60 percent duty, ns */
#define WANT_PCT_AT_6600NS 16    /* expected duty read back from a 6600 ns duty_cycle, percent */
#define WANT_TACH_PPR      3     /* expected tachometer pulses per revolution */
#define WANT_RPM_AT_43HZ   860   /* expected speed at 43 Hz tachometer input, RPM */
#define WANT_RPM_AT_32HZ   640   /* expected speed at 32 Hz tachometer input, RPM */
#else
#error "no fan goldens for this board"
#endif

#define WANT_N      ((int)(sizeof want / sizeof *want))
#define IDLE_N      ((int)(sizeof idle / sizeof *idle))
#define WANT_TACH_N ((int)(sizeof want_tach / sizeof *want_tach))
/* expected duty_cycle in ns for a duty of x10 tenths of a percent, polarity applied */
#define WANT_NS_X10(x10) (((WANT_ACTIVE_LOW) ? 1000 - (x10) : (x10)) * FAN_PERIOD_NS / 1000)

#include "../platform/linux/pwm_sys.c"

#define open tst_tach_open /* routes timer.c tachometer opens to the test nodes */
#include "../platform/linux/timer.c"
#undef open

_Static_assert(WANT_NS_X10(600) == WANT_NS_AT_60PCT, "polarity literal");

#if defined(BOARD_X3_H)
/* PWM channels the board drives, relative to PWM_ROOT */
static const char *const want[] = { "pwmchip0/pwm1", "pwmchip3/pwm1", "pwmchip3/pwm2" };
/* PWM channels the board must leave untouched, NULL = none */
static const char *const idle[] = { "pwmchip0/pwm0", "pwmchip0/pwm2", "pwmchip3/pwm0" };
/* tachometer device paths the board declares */
static const char *const want_tach[] = { "/dev/timer0", "/dev/timer1", "/dev/timer3" };
#else  /* BOARD_X3_H */
/* PWM channels the board drives, relative to PWM_ROOT */
static const char *const want[] = { "pwmchip3/pwm0" };
/* PWM channels the board must leave untouched, NULL = none */
static const char *const idle[] = { "pwmchip0/pwm2" };
/* tachometer device paths the board declares */
static const char *const want_tach[] = { "/dev/timer5" };
#endif /* BOARD_X3_H */

static const char *const root = PWM_ROOT;

static void md(const char *rel)
{
    char p[192];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    int rc = mkdir(p, 0755);
    assert(rc == 0);
}
static void wr(const char *rel, const char *content)
{
    char p[192];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    assert(f);
    if (content)
        fputs(content, f);
    fclose(f);
}
static int rdint(const char *chan, const char *attr)
{
    char p[192];
    snprintf(p, sizeof p, "%s/%s/%s", root, chan, attr);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    int v;
    int got = fscanf(f, "%d", &v);
    fclose(f);
    return got == 1 ? v : -1;
}

static void rdstr(const char *rel, char *buf, size_t n)
{
    char p[192];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    assert(f);
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = '\0';
}

static void mk_chip(int n, const char *dt_addr)
{
    char rel[64], lp[192], tgt[64];
    snprintf(rel, sizeof rel, "pwmchip%d", n);
    md(rel);
    snprintf(lp, sizeof lp, "%s/pwmchip%d/device", root, n);
    snprintf(tgt, sizeof tgt, "soc/%s.pwm", dt_addr);
    int slrc = symlink(tgt, lp);
    assert(slrc == 0);
    snprintf(rel, sizeof rel, "pwmchip%d/export", n);
    wr(rel, "");
    snprintf(rel, sizeof rel, "pwmchip%d/unexport", n);
    wr(rel, "");
}
static void mk_chan(int chip, int ch)
{
    char rel[64];
    snprintf(rel, sizeof rel, "pwmchip%d/pwm%d", chip, ch);
    md(rel);
    char a[96];
    snprintf(a, sizeof a, "%s/period", rel);
    wr(a, "");
    snprintf(a, sizeof a, "%s/duty_cycle", rel);
    wr(a, "");
    snprintf(a, sizeof a, "%s/enable", rel);
    wr(a, "");
}

static void each_driven_duty_is(int ns, const char *what)
{
    for (int i = 0; i < WANT_N; i++) {
        int d = rdint(want[i], "duty_cycle");
        if (d != ns)
            fprintf(stderr, "  FAIL: %s: %s duty_cycle=%d, want %d\n", what, want[i], d, ns);
        assert(d == ns);
    }
}

int main(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root, TST_TACH_ROOT);
    int clean = system(cmd);
    assert(clean == 0);
    int rootrc = mkdir(root, 0755);
    assert(rootrc == 0);
    int tachrc = mkdir(TST_TACH_ROOT, 0755);
    assert(tachrc == 0);

    mk_chip(0, "9140a000");
    mk_chip(3, "9140a040");
    for (int ch = 0; ch < 3; ch++) {
        mk_chan(0, ch);
        mk_chan(3, ch);
    }

    printf("board: %s\n", BOARD_NAME);

    {
        char rel[96], buf[32];
        snprintf(rel, sizeof rel, "%s/period", want[0]);
        wr(rel, "40000");
        snprintf(rel, sizeof rel, "%s/duty_cycle", want[0]);
        wr(rel, "6600");
        snprintf(rel, sizeof rel, "%s/enable", want[0]);
        wr(rel, "1");

        assert(fan_dev_get_duty_pct() == WANT_PCT_AT_6600NS);
        assert(fan_dev_last_duty_x10() < 0);

        char chip_export[64];
        snprintf(chip_export, sizeof chip_export, "%.*s/export",
                (int)(strchr(want[0], '/') - want[0]), want[0]);
        rdstr(chip_export, buf, sizeof buf);
        assert(buf[0] == '\0');
        snprintf(rel, sizeof rel, "%s/duty_cycle", want[0]);
        rdstr(rel, buf, sizeof buf);
        assert(strcmp(buf, "6600") == 0);

        fan_dev_set_duty_x10(WANT_MAX_PCT * 10);
        rdstr(rel, buf, sizeof buf);
        assert(strcmp(buf, "6600") == 0);
        assert(fan_dev_last_duty_x10() == WANT_MAX_PCT * 10);
    }

    assert(fan_dev_init() == 0);
    assert(fan_dev_last_duty_x10() == WANT_STARTUP_PCT * 10);
    for (int i = 0; i < WANT_N; i++) {
        int per = rdint(want[i], "period");
        int en  = rdint(want[i], "enable");
        if (per != FAN_PERIOD_NS || en != 1)
            fprintf(stderr, "  FAIL: %s not brought up (period=%d enable=%d)\n", want[i], per, en);
        assert(per == FAN_PERIOD_NS);
        assert(en == 1);
    }
    each_driven_duty_is(WANT_NS_X10(WANT_STARTUP_PCT * 10), "startup duty");

    for (int i = 0; i < IDLE_N; i++) {
        if (!idle[i])
            continue;
        if (rdint(idle[i], "period") != -1 || rdint(idle[i], "duty_cycle") != -1 ||
                rdint(idle[i], "enable") != -1)
            fprintf(stderr, "  FAIL: %s driven but not declared by this board\n", idle[i]);
        assert(rdint(idle[i], "period") == -1);
        assert(rdint(idle[i], "duty_cycle") == -1);
        assert(rdint(idle[i], "enable") == -1);
    }

    fan_dev_set_duty_x10(600);
    each_driven_duty_is(WANT_NS_AT_60PCT, "duty 60%");
    assert(fan_dev_get_duty_pct() == 60);
    assert(fan_dev_last_duty_x10() == 600);

    fan_dev_set_duty_x10(0);
    each_driven_duty_is(WANT_NS_X10(WANT_MIN_PCT * 10), "clamped to min");
    assert(fan_dev_get_duty_pct() == WANT_MIN_PCT);

    fan_dev_set_duty_x10(5000);
    each_driven_duty_is(WANT_NS_X10(WANT_MAX_PCT * 10), "clamped to max");
    assert(fan_dev_get_duty_pct() == WANT_MAX_PCT);

    fan_dev_stop();
    each_driven_duty_is(WANT_NS_X10(0), "stopped below the min clamp");
    assert(fan_dev_get_duty_pct() == 0);
    assert(fan_dev_last_duty_x10() == 0);
    for (int i = 0; i < WANT_N; i++)
        assert(rdint(want[i], "enable") == 1);

    fan_dev_set_duty_x10(600);
    char vis0[192], vis3[192], hid0[192], hid3[192];
    snprintf(vis0, sizeof vis0, "%s/pwmchip0", root);
    snprintf(vis3, sizeof vis3, "%s/pwmchip3", root);
    snprintf(hid0, sizeof hid0, "%s/hidden0", root);
    snprintf(hid3, sizeof hid3, "%s/hidden3", root);
    int hide0 = rename(vis0, hid0);
    assert(hide0 == 0);
    int hide3 = rename(vis3, hid3);
    assert(hide3 == 0);
    assert(fan_dev_init() < 0);
    assert(fan_dev_get_duty_pct() < 0);
    fan_dev_set_duty_x10(500);
    int show0 = rename(hid0, vis0);
    assert(show0 == 0);
    int show3 = rename(hid3, vis3);
    assert(show3 == 0);
    each_driven_duty_is(WANT_NS_AT_60PCT, "unresolved fan_dev writes nothing");

    {
        if (board_fan_count != (size_t)WANT_TACH_N)
            fprintf(stderr, "  FAIL: %d tach node(s) declared, want %d\n", (int)board_fan_count,
                    WANT_TACH_N);
        assert(board_fan_count == (size_t)WANT_TACH_N);
        for (int i = 0; i < WANT_TACH_N; i++) {
            const char *row = board_fans[i].tach_node;
            if (!row || strcmp(row, want_tach[i]) != 0)
                fprintf(stderr, "  FAIL: fan %d tach node = %s, want %s\n", i, row ? row : "(none)",
                        want_tach[i]);
            assert(row && strcmp(row, want_tach[i]) == 0);
        }
    }

    _Static_assert(WANT_RPM_AT_43HZ == 43 * 60 / WANT_TACH_PPR, "43 Hz literal");
    _Static_assert(WANT_RPM_AT_32HZ == 32 * 60 / WANT_TACH_PPR, "32 Hz literal");
    {
        static char tach[WANT_TACH_N][256];
        for (int i = 0; i < WANT_TACH_N; i++)
            snprintf(tach[i], sizeof tach[i], "%s/tach%d", TST_TACH_ROOT, i);
        for (int i = 0; i < WANT_TACH_N; i++)
            assert(fan_dev_get_rpm(i) < 0);

        const unsigned int hz0[] = { 43, 32, 0 };
        const unsigned int hz1[] = { 32, 43, 0 };
        for (int i = 0; i < WANT_TACH_N; i++) {
            FILE               *tf  = fopen(tach[i], "wb");
            const unsigned int *seq = i ? hz1 : hz0;
            assert(tf && fwrite(seq, sizeof hz0, 1, tf) == 1);
            fclose(tf);
        }
        int r43 = fan_dev_get_rpm(0);
        if (r43 != WANT_RPM_AT_43HZ)
            fprintf(stderr,
                    "  FAIL: tach 43 Hz -> %d rpm, want %d "
                    "(%d pulses/rev)\n",
                    r43, WANT_RPM_AT_43HZ, WANT_TACH_PPR);
        assert(r43 == WANT_RPM_AT_43HZ);
        for (int i = 1; i < WANT_TACH_N; i++) {
            int r = fan_dev_get_rpm(i);
            if (r != WANT_RPM_AT_32HZ)
                fprintf(stderr,
                        "  FAIL: fan %d read %d rpm off its own counter, "
                        "want %d — a fan reading slot 0's counter gets "
                        "%d\n",
                        i, r, WANT_RPM_AT_32HZ, WANT_RPM_AT_43HZ);
            assert(r == WANT_RPM_AT_32HZ);
        }
        int r32 = fan_dev_get_rpm(0);
        if (r32 != WANT_RPM_AT_32HZ)
            fprintf(stderr,
                    "  FAIL: tach 32 Hz -> %d rpm, want %d "
                    "(%d pulses/rev)\n",
                    r32, WANT_RPM_AT_32HZ, WANT_TACH_PPR);
        assert(r32 == WANT_RPM_AT_32HZ);
        for (int i = 1; i < WANT_TACH_N; i++)
            assert(fan_dev_get_rpm(i) == WANT_RPM_AT_43HZ);
        assert(fan_dev_get_rpm(0) == 0);
        assert(fan_dev_get_rpm(0) < 0);
        for (int i = 0; i < WANT_TACH_N; i++)
            unlink(tach[i]);
    }

    printf("test_fan_dev: all pass (%d fan(s))\n", WANT_N);
    return 0;
}
