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

#include "cali.h"
#include "board_cali.h"
#include "ini.h"
#include "ini_cache.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            fails++; \
        } \
    } while (0)

/* Scratch calibration INI files and systemcfg.ini (TMP_WM) under CALI_TEST_PREFIX */
#define TMP_INI     CALI_TEST_PREFIX "_tmp.ini"
#define TMP_WM      CALI_TEST_PREFIX "_systemcfg.ini"
#define TMP_NEW     CALI_TEST_PREFIX "_new.ini"
#define TMP_CACHE   CALI_TEST_PREFIX "_cache.ini"
#define TMP_ABSENT  CALI_TEST_PREFIX "_absent.ini"
#define TMP_RESOLVE CALI_TEST_PREFIX "_resolve.ini"

static int                 fails;
static int                 ini_loads;
static ini_cache_t         wm_cache;  /* RAM copy of TMP_WM behind wm_store */
static int                 wm_refuse; /* 1: wm_store refuses every set, as a frozen store does */
static int                 wm_get(const char *section, const char *key, char *buf, size_t cap);
static int                 wm_set(const char *section, const char *key, const char *value);
static const cali_wm_store wm_store = { wm_get, wm_set }; /* work mode store over wm_cache */

ini_t *__real_ini_load(const char *path);
ini_t *__wrap_ini_load(const char *path)
{
    ini_loads++;
    return __real_ini_load(path);
}

static int same_param(const cali_param *a, const cali_param *b)
{
    return a->max_pout == b->max_pout && a->temp == b->temp && a->volt == b->volt &&
            a->pll_start == b->pll_start && a->pll_interval == b->pll_interval;
}

static void test_default_table(void)
{
    for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
        const cali_param *p = cali_default_param(MAX_ASIC_ON_SINGLE_HASH, m);
        CHECK(p != NULL);
        CHECK(p->max_pout && p->temp && p->volt && p->pll_start && p->pll_interval);
        CHECK(p->volt >= VOL_MIN && p->volt <= VOL_MAX);
    }

#if MAX_ASIC_ON_SINGLE_HASH == 12
    {
        const cali_param *lo = cali_default_param(12, 0);
        CHECK(lo->max_pout == 62 && lo->temp == 80 && lo->volt == 3200 && lo->pll_start == 174 &&
                lo->pll_interval == 20);
        const cali_param *mid = cali_default_param(12, 1);
        CHECK(mid->max_pout == 95 && mid->temp == 85 && mid->volt == 3300 &&
                mid->pll_start == 302 && mid->pll_interval == 20);
        const cali_param *hi = cali_default_param(12, 2);
        CHECK(hi->max_pout == 133 && hi->temp == 90 && hi->volt == 3450 && hi->pll_start == 396 &&
                hi->pll_interval == 20);
    }
#elif MAX_ASIC_ON_SINGLE_HASH == 44
    {
        const cali_param *lo = cali_default_param(44, 0);
        CHECK(lo->max_pout == 180 && lo->temp == 75 && lo->volt == 2500 && lo->pll_start == 250 &&
                lo->pll_interval == 25);
        const cali_param *mid = cali_default_param(44, 1);
        CHECK(mid->max_pout == 240 && mid->temp == 80 && mid->volt == 2700 &&
                mid->pll_start == 300 && mid->pll_interval == 25);
        const cali_param *hi = cali_default_param(44, 2);
        CHECK(hi->max_pout == 300 && hi->temp == 85 && hi->volt == 2900 && hi->pll_start == 350 &&
                hi->pll_interval == 25);
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++)
            CHECK(cali_default_param(44, m)->pll_interval == FREQ_INTERVAL);
    }
#else
#error "no default cali table goldens for this ASIC count"
#endif
}

static int curing_has(uint16_t asic_cnt)
{
    static const struct {
        uint16_t   hash_asics;
        cali_param param[CALI_MODE_MAX];
    } rows[] = { BOARD_CALI_ROWS };

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (rows[i].hash_asics == asic_cnt)
            return 1;
    }
    return 0;
}

static void test_absent_population_fallback(void)
{
    const cali_param *own_low  = cali_default_param(MAX_ASIC_ON_SINGLE_HASH, 0);
    const uint16_t    absent[] = { 0, (uint16_t)(MAX_ASIC_ON_SINGLE_HASH + 1) };

    CHECK(curing_has(MAX_ASIC_ON_SINGLE_HASH) == 1);
    for (unsigned i = 0; i < sizeof absent / sizeof absent[0]; i++) {
        CHECK(curing_has(absent[i]) == 0);
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
            const cali_param *fb = cali_default_param(absent[i], m);
            CHECK(fb != NULL && same_param(fb, own_low));
        }
    }
    const cali_param *fbm = cali_default_param(MAX_ASIC_ON_SINGLE_HASH, CALI_MODE_MAX);
    CHECK(fbm != NULL && same_param(fbm, own_low));

    static const cali_param nano3s_low = { 62, 80, 3200, 174, 20 };
    static const cali_param x3_low     = { 180, 75, 2500, 250, 25 };
    if (MAX_ASIC_ON_SINGLE_HASH != 12)
        CHECK(!same_param(own_low, &nano3s_low));
    if (MAX_ASIC_ON_SINGLE_HASH != 44)
        CHECK(!same_param(own_low, &x3_low));
}

static void test_format_parse(void)
{
    cali_param p = { 133, 90, 3450, 420, 20 };
    char       buf[CALI_STR_MAX];
    CHECK(cali_format(&p, buf, sizeof buf) > 0);
    CHECK(strcmp(buf, "133-90-3450-420-20") == 0);

    cali_param q;
    CHECK(cali_parse(buf, &q) == 0);
    CHECK(q.max_pout == 133 && q.temp == 90 && q.volt == 3450 && q.pll_start == 420 &&
            q.pll_interval == 20);

    CHECK(cali_parse("133-90-3450-420", &q) < 0);
    CHECK(cali_parse("0-90-3450-420-20", &q) < 0);
    CHECK(cali_parse("133-90-3450-420-0", &q) < 0);
    CHECK(cali_parse("", &q) < 0);
}

static void test_persistence(void)
{
    unlink(TMP_INI);

    char raw[CALI_STR_MAX];
    CHECK(cali_get(TMP_INI, 2, 0, raw, sizeof raw) == 0);
    CHECK(raw[0] == '\0');

    CHECK(cali_set(TMP_INI, 2, 0, "133-90-3450-420-20") == 0);
    CHECK(cali_get(TMP_INI, 2, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "133-90-3450-420-20") == 0);

    CHECK(cali_set(TMP_INI, 0, 0, "62-80-3200-174-20") == 0);
    CHECK(cali_get(TMP_INI, 2, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "133-90-3450-420-20") == 0);
    CHECK(cali_get(TMP_INI, 0, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "62-80-3200-174-20") == 0);

    unlink(TMP_INI);
}

static void test_set_creates_file(void)
{
    unlink(TMP_NEW);
    struct stat st;
    CHECK(stat(TMP_NEW, &st) != 0);
    CHECK(cali_set(TMP_NEW, 1, 0, "95-85-3300-302-20") == 0);
    CHECK(stat(TMP_NEW, &st) == 0 && st.st_size > 0);
    CHECK(access(TMP_NEW ".tmp", F_OK) != 0);
    char raw[CALI_STR_MAX];
    CHECK(cali_get(TMP_NEW, 1, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "95-85-3300-302-20") == 0);
    unlink(TMP_NEW);
    unlink(TMP_NEW ".lock");
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

static void test_load_once(void)
{
    unlink(TMP_CACHE);
    char raw[CALI_STR_MAX];

    write_file(TMP_CACHE, "[mode2]\ncali_param0=133-90-3450-420-20\n");
    int before = ini_loads;
    for (int i = 0; i < 5; i++) {
        CHECK(cali_get(TMP_CACHE, 2, 0, raw, sizeof raw) == 0);
        CHECK(strcmp(raw, "133-90-3450-420-20") == 0);
    }
    CHECK(ini_loads == before + 1);

    write_file(TMP_CACHE,
            "[mode2]\ncali_param0=140-91-3460-430-25\n"
            "[mode0]\ncali_param0=62-80-3200-174-20\n");
    CHECK(cali_get(TMP_CACHE, 2, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "133-90-3450-420-20") == 0);
    CHECK(cali_get(TMP_CACHE, 0, 0, raw, sizeof raw) == 0);
    CHECK(raw[0] == '\0');
    CHECK(ini_loads == before + 1);

    CHECK(cali_set(TMP_CACHE, 1, 0, "95-85-3300-302-20") == 0);
    before = ini_loads;
    CHECK(cali_get(TMP_CACHE, 1, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "95-85-3300-302-20") == 0);
    CHECK(cali_get(TMP_CACHE, 2, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "133-90-3450-420-20") == 0);
    CHECK(ini_loads == before);

    unlink(TMP_CACHE);
    unlink(TMP_CACHE ".lock");
}

static void test_absent_cached(void)
{
    unlink(TMP_ABSENT);
    char raw[CALI_STR_MAX];

    int before = ini_loads;
    for (int i = 0; i < 5; i++) {
        CHECK(cali_get(TMP_ABSENT, 2, 0, raw, sizeof raw) == 0);
        CHECK(raw[0] == '\0');
    }
    CHECK(ini_loads == before + 1);

    cali_param out;
    uint8_t    aged = 1;
    CHECK(cali_resolve(TMP_ABSENT, MAX_ASIC_ON_SINGLE_HASH, 2, 0, &out, &aged) == 0);
    CHECK(aged == 0 && same_param(&out, cali_default_param(MAX_ASIC_ON_SINGLE_HASH, 2)));
    CHECK(ini_loads == before + 1);

    CHECK(cali_set(TMP_ABSENT, 2, 0, "150-92-3470-440-30") == 0);
    before = ini_loads;
    CHECK(cali_get(TMP_ABSENT, 2, 0, raw, sizeof raw) == 0);
    CHECK(strcmp(raw, "150-92-3470-440-30") == 0);
    CHECK(cali_get(TMP_ABSENT, 0, 0, raw, sizeof raw) == 0);
    CHECK(raw[0] == '\0');
    CHECK(ini_loads == before);

    unlink(TMP_ABSENT);
    unlink(TMP_ABSENT ".lock");
}

static void test_resolve(void)
{
    unlink(TMP_RESOLVE);
    cali_param out;
    uint8_t    aged;

    const uint16_t   asics = MAX_ASIC_ON_SINGLE_HASH;
    const cali_param dflt  = *cali_default_param(asics, 2);
    cali_param       rec   = dflt;
    rec.pll_start          = (uint16_t)(dflt.pll_start + 1);
    char raw[CALI_STR_MAX];
    CHECK(cali_format(&rec, raw, sizeof raw) > 0);

    CHECK(cali_resolve(TMP_RESOLVE, asics, 2, 0, &out, &aged) == 0);
    CHECK(aged == 0 && same_param(&out, &dflt));

    CHECK(cali_set(TMP_RESOLVE, 2, 0, raw) == 0);
    CHECK(cali_resolve(TMP_RESOLVE, asics, 2, 0, &out, &aged) == 0);
    CHECK(aged == 1 && same_param(&out, &rec));

    CHECK(cali_set(TMP_RESOLVE, 2, 0, "0-0-0-0-0") == 0);
    CHECK(cali_resolve(TMP_RESOLVE, asics, 2, 0, &out, &aged) == 0);
    CHECK(aged == 0 && same_param(&out, &dflt));

    unlink(TMP_RESOLVE);
    unlink(TMP_RESOLVE ".lock");
}

static int wm_get(const char *section, const char *key, char *buf, size_t cap)
{
    return ini_cache_get_copy(&wm_cache, section, key, buf, cap);
}

static int wm_set(const char *section, const char *key, const char *value)
{
    return wm_refuse ? -1 : ini_cache_set(&wm_cache, section, key, value);
}

static void wm_put(const char *text)
{
    FILE *f = fopen(TMP_WM, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

static int wm_is(const char *want)
{
    char  buf[256] = { 0 };
    FILE *f        = fopen(TMP_WM, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return n == strlen(want) && !memcmp(buf, want, n);
}

static void wm_open(void)
{
    CHECK(ini_cache_open(&wm_cache, TMP_WM) == 0);
}

static void wm_close(void)
{
    ini_cache_close(&wm_cache);
    unlink(TMP_WM);
    unlink(TMP_WM ".lock");
}

static void test_workmode_no_file(void)
{
    unlink(TMP_WM);
    wm_open();
    CHECK(cali_workmode_get(&wm_store) == CALI_MODE_DEFAULT);
    CHECK(cali_workmode_set(&wm_store, 0) == 0);
    CHECK(cali_workmode_get(&wm_store) == 0);
    CHECK(access(TMP_WM, F_OK) != 0);
    CHECK(ini_cache_flush(&wm_cache) == 1);
    CHECK(wm_is("[syscfg]\nwork_mode = 0\n\n"));
    ini_cache_close(&wm_cache);
    wm_open();
    CHECK(cali_workmode_get(&wm_store) == 0);
    wm_close();
}

static void test_workmode_missing_key(void)
{
    wm_put("[syscfg]\nwebuser = admin\n\n[led]\neffect = 1\n");
    wm_open();
    CHECK(cali_workmode_get(&wm_store) == CALI_MODE_DEFAULT);
    CHECK(cali_workmode_set(&wm_store, 1) == 0);
    CHECK(ini_cache_flush(&wm_cache) == 1);
    CHECK(wm_is("[syscfg]\nwebuser = admin\nwork_mode = 1\n\n[led]\neffect = 1\n\n"));
    wm_close();
}

static void test_workmode_existing_key(void)
{
    wm_put("[syscfg]\nwork_mode = 1\nwebuser = admin\n\n[led]\neffect = 1\n\n");
    wm_open();
    CHECK(cali_workmode_get(&wm_store) == 1);
    CHECK(cali_workmode_set(&wm_store, 2) == 0);
    CHECK(cali_workmode_get(&wm_store) == 2);
    CHECK(wm_is("[syscfg]\nwork_mode = 1\nwebuser = admin\n\n[led]\neffect = 1\n\n"));
    CHECK(ini_cache_flush(&wm_cache) == 1);
    CHECK(wm_is("[syscfg]\nwork_mode = 2\nwebuser = admin\n\n[led]\neffect = 1\n\n"));
    CHECK(cali_workmode_set(&wm_store, 2) == 0);
    CHECK(ini_cache_flush(&wm_cache) == 0);
    CHECK(cali_workmode_set(&wm_store, CALI_MODE_MAX) < 0);
    CHECK(cali_workmode_get(&wm_store) == 2);
    CHECK(ini_cache_flush(&wm_cache) == 0);
    wm_close();
}

static void test_workmode_read_once(void)
{
    wm_put("[syscfg]\nwork_mode = 2\n");
    wm_open();
    wm_put("[syscfg]\nwork_mode = 0\n");
    CHECK(cali_workmode_get(&wm_store) == 2);
    unlink(TMP_WM);
    CHECK(cali_workmode_get(&wm_store) == 2);
    wm_close();
}

static void test_workmode_refused(void)
{
    uint8_t def   = CALI_MODE_DEFAULT;
    uint8_t other = (uint8_t)((def + 1) % CALI_MODE_MAX);
    unlink(TMP_WM);
    wm_open();
    wm_refuse = 1;
    CHECK(cali_workmode_set(&wm_store, other) < 0);
    CHECK(cali_workmode_get(&wm_store) == def);
    wm_refuse = 0;
    CHECK(cali_workmode_set(&wm_store, other) == 0);
    CHECK(cali_workmode_get(&wm_store) == other);
    wm_close();
}

static void test_workmode_reset(void)
{
    uint8_t def   = CALI_MODE_DEFAULT;
    uint8_t other = (uint8_t)((def + 1) % CALI_MODE_MAX);
    char    text[64];
    snprintf(text, sizeof text, "[syscfg]\nwork_mode = %u\n", (unsigned)def);
    const char *factory[] = { text, "[syscfg]\nwebuser = admin\n", NULL };

    for (unsigned i = 0; i < sizeof factory / sizeof factory[0]; i++) {
        unlink(TMP_WM);
        wm_open();
        CHECK(cali_workmode_set(&wm_store, other) == 0);
        CHECK(ini_cache_flush(&wm_cache) == 1);
        CHECK(cali_workmode_set(&wm_store, (uint8_t)((def + 2) % CALI_MODE_MAX)) == 0);
        if (factory[i])
            wm_put(factory[i]);
        else
            unlink(TMP_WM);
        CHECK(ini_cache_reload(&wm_cache) == 0);
        CHECK(cali_workmode_get(&wm_store) == def);
        CHECK(ini_cache_flush(&wm_cache) == 0);
        ini_cache_close(&wm_cache);
        wm_open();
        CHECK(cali_workmode_get(&wm_store) == def);
        wm_close();
    }
}

static void test_workmode_default(void)
{
    uint8_t want = WORK_MODE_DEFAULT_ID;
    CHECK(want < CALI_MODE_MAX);

    char oob[64];
    snprintf(oob, sizeof oob, "[syscfg]\nwork_mode = %d\n", (int)CALI_MODE_MAX);
    const char *junk[] = { "", "[led]\nwork_mode = 1\n", "[syscfg]\nwork_mode = not-a-number\n",
        "[syscfg]\nwork_mode =\n", "[syscfg]\nwork_mode = -1\n", "[syscfg]\nwork_mode = 1x\n",
        "[syscfg]\nwork_mode = 99999999999\n", oob };
    for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        wm_put(junk[i]);
        wm_open();
        CHECK(cali_workmode_get(&wm_store) == want);
        wm_close();
    }
}

int main(void)
{
    test_default_table();
    test_absent_population_fallback();
    test_format_parse();
    test_persistence();
    test_set_creates_file();
    test_load_once();
    test_absent_cached();
    test_resolve();
    test_workmode_no_file();
    test_workmode_missing_key();
    test_workmode_existing_key();
    test_workmode_read_once();
    test_workmode_refused();
    test_workmode_reset();
    test_workmode_default();
    if (fails) {
        printf("test_cali: %d FAILED (%s)\n", fails, BOARDCONF_SECTION);
        return 1;
    }
    printf("test_cali: OK (%s)\n", BOARDCONF_SECTION);
    return 0;
}
