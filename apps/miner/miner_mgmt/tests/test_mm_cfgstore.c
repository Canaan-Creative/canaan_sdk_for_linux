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
#include "mm_cfgstore.h"
#include "ini.h"
#include "mc_api_bootby.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../tests/include/tst_check.h"

#define PATH    "test_mm_cfgstore.ini"                  /* scratch INI, relative to the cwd */
#define BADPATH "test_mm_cfgstore_no_dir/systemcfg.ini" /* INI in a missing directory */

static void tst_cleanup(void)
{
    unlink(PATH);
    unlink(PATH ".tmp");
    unlink(PATH ".lock");
}

static void file_put(const char *text)
{
    FILE *f = fopen(PATH, "w");

    if (!f || fputs(text, f) == EOF || fclose(f) != 0) {
        fprintf(stderr, "  FAIL: cannot write the sandbox ini %s\n", PATH);
        tst_cleanup();
        exit(2);
    }
}

static const char *file_str(const char *sect, const char *key)
{
    static char held[64];
    ini_t      *ini = ini_load(PATH);
    const char *v;

    held[0] = '\0';
    if (!ini)
        return held;
    v = ini_get(ini, sect, key);
    if (v)
        snprintf(held, sizeof held, "%s", v);
    ini_free(ini);
    return held;
}

static long file_num(const char *sect, const char *key)
{
    const char *v = file_str(sect, key);

    return *v ? strtol(v, NULL, 0) : -1;
}

static int ram_is(const char *sect, const char *key, const char *want)
{
    char buf[64];

    return mm_cfgstore_get(sect, key, buf, sizeof buf) == 0 && !strcmp(buf, want);
}

static void boot(void)
{
    if (mm_cfgstore_open(PATH) != 0) {
        fprintf(stderr, "  FAIL: cannot open the sandbox ini %s\n", PATH);
        tst_cleanup();
        exit(2);
    }
}

static void crash(int sig)
{
    if (mm_bootby_persist(MC_BOOTBY_SYS, (uint32_t)sig) != 0) {
        fprintf(stderr, "  FAIL: mm_bootby_persist failed\n");
        tst_cleanup();
        exit(2);
    }
}

static void store_tests(void)
{
    char small[2];

    tst_cleanup();
    CHECK(mm_cfgstore_open(PATH) == 0 && !ram_is("led", "bright", "1"),
            "open accepts a missing file and starts empty");
    CHECK(mm_cfgstore_flush() == 0 && access(PATH, F_OK) != 0,
            "an empty store over a missing file writes nothing");

    file_put("[led]\nbright = 42\n[syscfg]\nwebpass = 8c6976e5\n");
    boot();
    CHECK(ram_is("led", "bright", "42") && ram_is("syscfg", "webpass", "8c6976e5"),
            "open reads every key of an existing file");
    CHECK(mm_cfgstore_get("led", "bright", small, sizeof small) == -1 &&
                    mm_cfgstore_get("led", "none", small, sizeof small) == -1,
            "get refuses an absent key and a value longer than the buffer");

    CHECK(mm_cfgstore_set("led", "bright", "9") == 0 && ram_is("led", "bright", "9"),
            "set is visible to readers at once");
    CHECK(file_num("led", "bright") == 42, "set does not touch the file");
    CHECK(mm_cfgstore_flush() == 1 && file_num("led", "bright") == 9, "flush writes a change");
    CHECK(mm_cfgstore_flush() == 0, "a second flush has nothing to write");
    CHECK(!strcmp(file_str("syscfg", "webpass"), "8c6976e5"), "flush keeps every other key");

    CHECK(mm_cfgstore_set("led", "bright", "1") == 0 && mm_cfgstore_reload() == 0 &&
                    ram_is("led", "bright", "9") && mm_cfgstore_flush() == 0,
            "reload drops an unflushed change");

    mm_cfgstore_freeze(1);
    CHECK(mm_cfgstore_frozen(), "freeze is reported");
    CHECK(mm_cfgstore_set("led", "bright", "5") == -1 && ram_is("led", "bright", "9"),
            "while frozen set refuses and RAM keeps its value");
    CHECK(mm_cfgstore_put("led", "rgb", "7") == 0 && mm_cfgstore_flush() == -1 &&
                    file_num("led", "rgb") == -1,
            "while frozen flush refuses and writes nothing");
    {
        static const char *const k[] = { "prod" };
        static const char *const v[] = { "P0" };

        CHECK(mm_cfgstore_commit("hwcfg", k, v, 1) == -1 && !ram_is("hwcfg", "prod", "P0") &&
                        file_num("hwcfg", "prod") == -1,
                "while frozen commit refuses and changes neither RAM nor the file");
    }
    {
        static const char *const sc[] = { "syscfg", "led" };
        static const char *const k[]  = { "ledmode", "bright" };
        static const char *const v[]  = { "1", "4" };

        CHECK(mm_cfgstore_set_n(sc, k, v, 2) == -1 && !ram_is("syscfg", "ledmode", "1") &&
                        ram_is("led", "bright", "9"),
                "while frozen set_n refuses every pair");
    }
    CHECK(mm_cfgstore_sync() == 1 && file_num("led", "rgb") == 7,
            "while frozen put and sync still write");
    mm_cfgstore_freeze(0);
    CHECK(!mm_cfgstore_frozen() && mm_cfgstore_set("led", "bright", "8") == 0,
            "unfreeze lets set through again");
    {
        static const char *const sc[] = { "syscfg", "led" };
        static const char *const k[]  = { "ledmode", "temper" };
        static const char *const v[]  = { "1", "50" };

        CHECK(mm_cfgstore_set_n(sc, k, v, 2) == 0 && ram_is("syscfg", "ledmode", "1") &&
                        ram_is("led", "temper", "50"),
                "set_n sets every pair in RAM after unfreeze");
        CHECK(file_num("syscfg", "ledmode") == -1, "set_n does not touch the file");
    }

    {
        static const char *const k[] = { "prod", "model" };
        static const char *const v[] = { "P1", "M1" };

        CHECK(mm_cfgstore_commit("hwcfg", k, v, 2) == 0 && ram_is("hwcfg", "prod", "P1") &&
                        ram_is("hwcfg", "model", "M1"),
                "commit sets every key in RAM");
        CHECK(!strcmp(file_str("hwcfg", "prod"), "P1") && file_num("led", "bright") == 8,
                "commit writes its keys and the pending change at once");
        CHECK(mm_cfgstore_flush() == 0, "a flush after a commit has nothing to write");

        CHECK(mm_cfgstore_open(BADPATH) == 0, "open over a missing directory");
        CHECK(mm_cfgstore_commit("hwcfg", k, v, 2) == -1 && !ram_is("hwcfg", "prod", "P1"),
                "a commit that cannot write leaves RAM unchanged");
    }
}

static void bootby_tests(void)
{
    uint8_t  r   = 0;
    uint32_t sub = 0;

    tst_cleanup();
    boot();
    crash(SIGSEGV);
    boot();
    mm_bootby_recover(0, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == (uint32_t)SIGSEGV,
            "a crash persisted as SYS/%u reads back on the next boot as MC_BOOTBY_SYS / "
            "sub %u (saw %u/%u)",
            (unsigned)SIGSEGV, (unsigned)SIGSEGV, (unsigned)r, (unsigned)sub);
    CHECK(file_num(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_REASON) == MC_BOOTBY_POWRON &&
                    file_num(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_SUB) == 0,
            "...and the consumed reason is reset to POWRON/0 in the file at once");
    boot();
    mm_bootby_recover(0, &r, &sub);
    CHECK(r == MC_BOOTBY_POWRON && sub == 0,
            "...so the NEXT boot is a plain power-on again (saw %u/%u)", (unsigned)r,
            (unsigned)sub);

    tst_cleanup();
    boot();
    CHECK(mm_bootby_persist(MC_BOOTBY_UPGRADE, 0) == 0, "persist reports success");
    crash(SIGSEGV);
    boot();
    mm_bootby_recover(0, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == (uint32_t)SIGSEGV,
            "a crash AFTER a pending MC_BOOTBY_UPGRADE resolves to SYS/%u: the last "
            "writer wins (saw %u/%u)",
            (unsigned)SIGSEGV, (unsigned)r, (unsigned)sub);

    tst_cleanup();
    boot();
    crash(SIGSEGV);
    (void)mm_bootby_persist(MC_BOOTBY_UPGRADE, 0);
    boot();
    mm_bootby_recover(0, &r, &sub);
    CHECK(r == MC_BOOTBY_UPGRADE && sub == 0,
            "...and the other way round: an OTA reboot after a crash record reports "
            "UPGRADE (saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    tst_cleanup();
    boot();
    (void)mm_bootby_persist(MC_BOOTBY_UPGRADE, 0);
    boot();
    mm_bootby_recover(1, &r, &sub);
    CHECK(r == MC_BOOTBY_UPGRADE && sub == 0,
            "a persisted reason outranks the watchdog bootstatus (saw %u/%u)", (unsigned)r,
            (unsigned)sub);

    tst_cleanup();
    boot();
    crash(SIGABRT);
    boot();
    mm_bootby_recover(1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == (uint32_t)SIGABRT,
            "with both a crash record and a watchdog bootstatus the signal wins "
            "(saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    tst_cleanup();
    boot();
    mm_bootby_recover(1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == MC_BOOTBY_SUB_WDT,
            "no file, watchdog bootstatus set: SYS/0xF1 (saw %u/%u)", (unsigned)r, (unsigned)sub);

    tst_cleanup();
    file_put("[syscfg]\nreason = 0\nsubreason = 0\n");
    boot();
    mm_bootby_recover(1, &r, &sub);
    CHECK(r == MC_BOOTBY_SYS && sub == MC_BOOTBY_SUB_WDT,
            "a stored reason of 0 is 'no record': the watchdog arm still applies "
            "(saw %u/%u)",
            (unsigned)r, (unsigned)sub);

    tst_cleanup();
    file_put("[syscfg]\nledmode = 3\nwebpass = 8c6976e5\n");
    boot();
    crash(SIGSEGV);
    CHECK(file_num("syscfg", "ledmode") == 3 && !strcmp(file_str("syscfg", "webpass"), "8c6976e5"),
            "persisting a boot reason keeps every other key in the file");
    boot();
    mm_bootby_recover(0, &r, &sub);
    CHECK(!strcmp(file_str("syscfg", "webpass"), "8c6976e5"),
            "...and so does consuming it on the next boot");

    tst_cleanup();
    boot();
    CHECK(mm_cfgstore_set("syscfg", "ledmode", "1") == 0, "a change is pending");
    crash(SIGSEGV);
    CHECK(mm_cfgstore_set("syscfg", "ledmode", "0") == 0 && mm_cfgstore_flush() == 1,
            "a later change is flushed");
    CHECK(file_num(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_REASON) == MC_BOOTBY_SYS &&
                    file_num(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_SUB) == SIGSEGV &&
                    file_num("syscfg", "ledmode") == 0,
            "the periodic flush after a boot reason write does not put an older "
            "reason back");
}

int main(void)
{
    store_tests();
    bootby_tests();
    tst_cleanup();
    return tst_done();
}
