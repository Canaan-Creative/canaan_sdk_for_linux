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
#include "mm_ota.h"

#include "aup.h"
#include "mtd_dev.h"
#include "osif.h"
#include "ota_state.h"
#include "ubootenv.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../tests/include/tst_check.h"

#define TST_SOCK "mm_ota_test.sock" /* socket path, relative to the cwd */
#undef OTA_SUCCEED_FILE
#define OTA_SUCCEED_FILE "mm_ota_test.succeed" /* success latch file, relative to the cwd */
#undef OTA_ENV_LIST_FILE
#define OTA_ENV_LIST_FILE "mm_ota_test.envlist" /* env update list, relative to the cwd */
#undef OTA_UBI_SYSFS
#define OTA_UBI_SYSFS "mm_ota_test.ubi"
/* read in place of /proc/meminfo, relative to the cwd */
#define TST_MEMINFO "mm_ota_test.meminfo"
#define ENV_SLOTS   16  /* fake U-Boot env entries */
#define ENV_KEY_CAP 64  /* env key size, bytes with NUL */
#define ENV_VAL_CAP 64  /* env value size, bytes with NUL */
#define MTD_LOG     16  /* mtd_dev_from_name() names recorded */
#define MTD_TAB     8   /* fake MTD partition table entries */
#define LOG_CAP     256 /* last log line size, bytes */
#define FLASH_CAP   16384

static FILE *tst_fopen(const char *path, const char *mode);
#define fopen(p, m) tst_fopen((p), (m)) /* /proc/meminfo redirectable to TST_MEMINFO */
#include "mm_ota.c"
#undef fopen

/* Slot selection case for one <bin>_partition value */
typedef struct {
    const char *env;                /* <bin>_partition env value */
    char        slot;               /* expected env_slot, 'A' or 'B' */
    const char *part;               /* expected MTD partition name to flash */
    int         overwrites_running; /* 1 if that partition is the running slot */
} slot_case;

/* Fake U-Boot env: key, value, slot in use */
static struct {
    char key[ENV_KEY_CAP];
    char val[ENV_VAL_CAP];
    int  used;
} g_env[ENV_SLOTS];

static char      g_list_body[512];         /* contents of the last applied env list */
static char      g_list_path[256];         /* path of the last applied env list */
static int       g_list_calls;             /* ubootenv_apply_list() call count */
static int       g_list_keys;              /* key=value lines in the last list */
static int       g_list_fail;              /* nonzero fails ubootenv_apply_list() unapplied */
static char      g_mtd_name[MTD_LOG][128]; /* names passed to mtd_dev_from_name() */
static int       g_name_calls;             /* mtd_dev_from_name() call count */
static int       g_mtd_num;                /* MTD number of a name not in g_mtd_tab */
static long long g_mtd_size;               /* size of an MTD not in g_mtd_tab, bytes */
/* Fake MTD partitions: name, MTD number, size in bytes */
static struct {
    char      name[128];
    int       num;
    long long size;
} g_mtd_tab[MTD_TAB];
static int           g_mtd_tab_n;
static int           g_erase_calls; /* mtd_dev_erase() call count */
static long long     g_erase_start; /* last erase offset, bytes; -1 none */
static long long     g_erase_size;  /* last erase length, bytes; -1 none */
static int           g_erase_rc;    /* mtd_dev_erase() result */
static int           g_write_calls; /* mtd_dev_write() call count */
static long long     g_write_off;   /* last write offset, bytes; -1 none */
static size_t        g_write_len;   /* last write length, bytes */
static int           g_write_rc;    /* mtd_dev_write() result */
static unsigned char g_flash[FLASH_CAP];
static int           g_write_erased;
static int           g_end_calls;    /* mtd_dev_write_end() call count */
static int           g_end_mtdnum;   /* last mtd_dev_write_end() MTD number; -99 none */
static int           g_end_commit;   /* last mtd_dev_write_end() commit flag; -1 none */
static int           g_end_rc;       /* mtd_dev_write_end() result */
static int           g_init_rc;      /* mtd_dev_init() result */
static int           g_cleanups;     /* mtd_dev_cleanup() call count */
static int           g_feed_calls;   /* ota_feed() call count */
static int           g_feed_rc;      /* ota_feed() result, UPGRADE_* */
static int8_t        g_feed_percent; /* progress ota_feed() reports, percent */
static int (*g_feed_hook)(void);     /* run inside the first ota_feed(); NULL none */
static int           g_feed_done;    /* a final result was returned; later feeds fail */
static int           g_thread_calls; /* osif_thread_create() call count */
static osif_status_t g_thread_rc;    /* osif_thread_create() result */
static char          g_log[LOG_CAP];
static int           g_log_calls;
static int           g_reboot_calls; /* ota_arm_reboot() call count */
static int           g_meminfo_fake; /* nonzero reads TST_MEMINFO for /proc/meminfo */

static ota_mtd_ctx  g_ctx;       /* sink context of run_slot() */
static ota_mtd_ctx *g_serve_ctx; /* sink context used by the feed hooks */
static char         g_part[128]; /* first partition name run_slot() resolved; "" none */
/* <bin>_partition values and the slot each selects */
static const slot_case g_slots[] = {
    { "", 'B', "rootfs_b", 0 },
    { "A", 'B', "rootfs_b", 0 },
    { "B", 'A', "rootfs_a", 0 },
    { "a", 'B', "rootfs_b", 0 },
    { "b", 'A', "rootfs_a", 0 },
    { "B ", 'A', "rootfs_a", 0 },
    { "A ", 'B', "rootfs_b", 0 },
    { "B\n", 'A', "rootfs_a", 0 },
    { "B\r\n", 'A', "rootfs_a", 0 },
    { "B\t", 'A', "rootfs_a", 0 },
    { "B  ", 'A', "rootfs_a", 0 },
    { " B", 'B', "rootfs_b", 0 },
    { "  B  ", 'B', "rootfs_b", 0 },
    { "AB", 'B', "rootfs_b", 0 },
    { "BB", 'A', "rootfs_a", 0 },
    { "C", 'B', "rootfs_b", 0 },
    { "B B", 'A', "rootfs_a", 0 },
    { "\x01\x7f?", 'B', "rootfs_b", 0 },
    { "B\v", 'A', "rootfs_a", 0 },
};

static void env_reset(void)
{
    memset(g_env, 0, sizeof g_env);
    memset(g_list_body, 0, sizeof g_list_body);
    memset(g_list_path, 0, sizeof g_list_path);
    g_list_calls = 0;
    g_list_keys  = 0;
    g_list_fail  = 0;
    unlink(OTA_ENV_LIST_FILE);
}

static void env_seed(const char *key, const char *val)
{
    for (int i = 0; i < ENV_SLOTS; i++) {
        if (!g_env[i].used) {
            g_env[i].used = 1;
            snprintf(g_env[i].key, sizeof g_env[i].key, "%s", key);
            snprintf(g_env[i].val, sizeof g_env[i].val, "%s", val);
            return;
        }
    }
}

char *ubootenv_get(const char *name)
{
    for (int i = 0; i < ENV_SLOTS; i++)
        if (g_env[i].used && !strcmp(g_env[i].key, name))
            return strdup(g_env[i].val);
    return NULL;
}

static void env_put(const char *name, const char *value)
{
    for (int i = 0; i < ENV_SLOTS; i++) {
        if (g_env[i].used && !strcmp(g_env[i].key, name)) {
            snprintf(g_env[i].val, sizeof g_env[i].val, "%s", value);
            return;
        }
    }
    env_seed(name, value);
}

int ubootenv_apply_list(const char *filename)
{
    char   line[ENV_KEY_CAP + ENV_VAL_CAP + 2];
    size_t n;
    FILE  *f;

    g_list_calls++;
    g_list_keys    = 0;
    g_list_body[0] = 0;
    snprintf(g_list_path, sizeof g_list_path, "%s", filename);

    f = fopen(filename, "r");
    if (!f)
        return -1;
    n              = fread(g_list_body, 1, sizeof g_list_body - 1, f);
    g_list_body[n] = 0;
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        char  *eq;

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        g_list_keys++;
        if (!g_list_fail)
            env_put(line, eq + 1);
    }
    fclose(f);
    return g_list_fail ? -1 : 0;
}

static int env_is(const char *key, const char *want)
{
    char *v  = ubootenv_get(key);
    int   ok = v && !strcmp(v, want);

    free(v);
    return ok;
}

static int env_absent(const char *key)
{
    char *v  = ubootenv_get(key);
    int   ok = v == NULL;

    free(v);
    return ok;
}

static void mtd_reset(void)
{
    memset(g_mtd_name, 0, sizeof g_mtd_name);
    memset(g_mtd_tab, 0, sizeof g_mtd_tab);
    g_name_calls  = 0;
    g_mtd_tab_n   = 0;
    g_mtd_num     = 7;
    g_mtd_size    = 4096;
    g_erase_calls = 0;
    g_erase_start = -1;
    g_erase_size  = -1;
    g_erase_rc    = 0;
    g_write_calls = 0;
    g_write_off   = -1;
    g_write_len   = 0;
    g_write_rc    = 0;
    memset(g_flash, 0, sizeof g_flash);
    g_write_erased = -1;
    g_end_calls    = 0;
    g_end_mtdnum   = -99;
    g_end_commit   = -1;
    g_end_rc       = 0;
    g_init_rc      = 0;
    g_cleanups     = 0;
}

int mtd_dev_init(void)
{
    return g_init_rc;
}
void mtd_dev_cleanup(void)
{
    g_cleanups++;
}

static void mtd_slot(const char *name, int num, long long size)
{
    if (g_mtd_tab_n >= MTD_TAB)
        return;
    snprintf(g_mtd_tab[g_mtd_tab_n].name, sizeof g_mtd_tab[0].name, "%s", name);
    g_mtd_tab[g_mtd_tab_n].num  = num;
    g_mtd_tab[g_mtd_tab_n].size = size;
    g_mtd_tab_n++;
}

int mtd_dev_from_name(const char *name)
{
    int n = g_name_calls++;

    if (n < MTD_LOG)
        snprintf(g_mtd_name[n], sizeof g_mtd_name[n], "%s", name);
    for (int i = 0; i < g_mtd_tab_n; i++)
        if (!strcmp(g_mtd_tab[i].name, name))
            return g_mtd_tab[i].num;
    return g_mtd_num;
}

long long mtd_dev_size(int mtdnum)
{
    for (int i = 0; i < g_mtd_tab_n; i++)
        if (g_mtd_tab[i].num == mtdnum)
            return g_mtd_tab[i].size;
    return g_mtd_size;
}

int mtd_dev_erase(int mtdnum, long long start, long long size)
{
    (void)mtdnum;
    g_erase_calls++;
    g_erase_start = start;
    g_erase_size  = size;
    return g_erase_rc;
}

int mtd_dev_write(int mtdnum, long long offset, const void *buf, size_t len)
{
    (void)mtdnum;
    if (g_write_calls++ == 0)
        g_write_erased = g_erase_calls;
    g_write_off = offset;
    g_write_len = len;
    if (offset >= 0 && (size_t)offset <= sizeof g_flash && len <= sizeof g_flash - (size_t)offset)
        memcpy(g_flash + offset, buf, len);
    return g_write_rc;
}

int mtd_dev_write_end(int mtdnum, int commit)
{
    g_end_calls++;
    g_end_mtdnum = mtdnum;
    g_end_commit = commit;
    return g_end_rc;
}

size_t ota_state_size(void)
{
    return 64;
}

void ota_reset(ota_state_t *st, const ota_sink *sink)
{
    (void)st;
    (void)sink;
}

int ota_feed(ota_state_t *st, const void *buf, int len, int8_t *percent)
{
    (void)st;
    (void)buf;
    (void)len;
    g_feed_calls++;
    if (g_feed_done)
        return UPGRADE_ALREADY_ERR;
    if (percent)
        *percent = g_feed_percent;
    if (g_feed_hook && g_feed_calls == 1)
        g_feed_hook();
    if (g_feed_rc != UPGRADE_PROCESS_OK)
        g_feed_done = 1;
    return g_feed_rc;
}

static void log_reset(void)
{
    g_log[0]    = 0;
    g_log_calls = 0;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_log, sizeof g_log, fmt, ap);
    va_end(ap);
    g_log_calls++;
}

osif_status_t osif_thread_create(osif_thread_t **t, const char *name, osif_thread_fn fn, void *arg,
        size_t stack_bytes)
{
    (void)name;
    (void)fn;
    (void)arg;
    (void)stack_bytes;
    g_thread_calls++;
    if (t)
        *t = NULL;
    return g_thread_rc;
}

void ota_arm_reboot(void)
{
    g_reboot_calls++;
}

static FILE *tst_fopen(const char *path, const char *mode)
{
    if (g_meminfo_fake && !strcmp(path, "/proc/meminfo"))
        path = TST_MEMINFO;
    return fopen(path, mode);
}

static void meminfo_set(const char *body)
{
    FILE *f = fopen(TST_MEMINFO, "w");

    if (f) {
        fputs(body, f);
        fclose(f);
    }
    g_meminfo_fake = 1;
}

static void latch_reset(void)
{
    g_ota_success = 0;
    unlink(OTA_SUCCEED_FILE);
}

static void ubi_fake(const char *dev, int mtdnum)
{
    char  path[128];
    FILE *f;

    mkdir(OTA_UBI_SYSFS, 0700);
    snprintf(path, sizeof path, "%s/%s", OTA_UBI_SYSFS, dev);
    mkdir(path, 0700);
    if (mtdnum < 0)
        return;
    snprintf(path, sizeof path, "%s/%s/mtd_num", OTA_UBI_SYSFS, dev);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", mtdnum);
        fclose(f);
    }
}

static void ubi_clear(void)
{
    static const char *const devs[] = { "ubi0", "ubi0_0", "ubi1", "ubi1_0", "ubi2" };
    char                     path[128];

    for (size_t i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s/mtd_num", OTA_UBI_SYSFS, devs[i]);
        unlink(path);
        snprintf(path, sizeof path, "%s/%s", OTA_UBI_SYSFS, devs[i]);
        rmdir(path);
    }
    rmdir(OTA_UBI_SYSFS);
}

static void bin_name(struct aup_bin *b, const char *name)
{
    size_t n = strlen(name);

    memset(b, 0, sizeof *b);
    memcpy(b->name, name, n < sizeof b->name ? n : sizeof b->name);
}

static char run_slot(const char *bin, const char *env)
{
    struct aup_bin b;
    char           key[128];

    env_reset();
    mtd_reset();
    if (env) {
        snprintf(key, sizeof key, "%.*s" OTA_ENV_SUFFIX, OTA_BIN_NAME_MAX, bin);
        env_seed(key, env);
    }
    memset(&g_ctx, 0, sizeof g_ctx);
    bin_name(&b, bin);
    mtd_sink_begin(&g_ctx, &b);
    snprintf(g_part, sizeof g_part, "%s", g_name_calls ? g_mtd_name[0] : "");
    return g_ctx.env_slot;
}

static char running_slot(const char *env)
{
    return (env && (*env == 'B' || *env == 'b')) ? 'B' : 'A';
}

static const char *disp(const char *s)
{
    static char out[64];
    size_t      o = 0;

    if (!s)
        return "(unset)";
    for (size_t i = 0; s[i] && o + 4 < sizeof out; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n' || c == '\r' || c == '\t' || c < 0x20 || c > 0x7e) {
            out[o++] = '<';
            out[o++] = "0123456789abcdef"[(c >> 4) & 0xf];
            out[o++] = "0123456789abcdef"[c & 0xf];
            out[o++] = '>';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return out;
}

static int log_is_printable(void)
{
    for (const unsigned char *p = (const unsigned char *)g_log; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            return 0;
    return 1;
}

static int feed_one_bin(void)
{
    struct aup_bin b;

    bin_name(&b, "rootfs");
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    return mtd_sink_end(g_serve_ctx, &b);
}

static int feed_begin_only(void)
{
    struct aup_bin b;

    bin_name(&b, "rootfs");
    return mtd_sink_begin(g_serve_ctx, &b);
}

static int feed_write_fail(void)
{
    static const unsigned char data[4];
    struct aup_bin             b;

    bin_name(&b, "rootfs");
    b.size = aup_be32((int32_t)sizeof data);
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    if (mtd_sink_write(g_serve_ctx, &b, 0, data, sizeof data) != 0)
        return -1;
    return mtd_sink_end(g_serve_ctx, &b);
}

static int feed_staged_only(void)
{
    static const unsigned char data[4];
    struct aup_bin             b;

    bin_name(&b, "rootfs");
    b.size = aup_be32((int32_t)sizeof data);
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    return mtd_sink_write(g_serve_ctx, &b, 0, data, sizeof data);
}

static int feed_stage_fail(void)
{
    static const unsigned char data[4];
    struct rlimit              old, lim;
    struct aup_bin             b;
    int                        rc;

    bin_name(&b, "rootfs");
    b.size = aup_be32((int32_t)sizeof data);
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    getrlimit(RLIMIT_FSIZE, &old);
    lim          = old;
    lim.rlim_cur = 2;
    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &lim);
    rc = mtd_sink_write(g_serve_ctx, &b, 0, data, sizeof data);
    setrlimit(RLIMIT_FSIZE, &old);
    return rc;
}

static int feed_two_bins(void)
{
    struct aup_bin b;

    bin_name(&b, "uboot");
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    if (mtd_sink_end(g_serve_ctx, &b) != 0)
        return -1;
    bin_name(&b, "rootfs");
    if (mtd_sink_begin(g_serve_ctx, &b) != 0)
        return -1;
    return mtd_sink_end(g_serve_ctx, &b);
}

static int serve_run(const char *payload, size_t len, mm_ota_msg *out)
{
    static uint64_t    stbuf[16];
    static ota_mtd_ctx mctx;
    int                sv[2];
    ssize_t            n;

    memset(out, 0, sizeof *out);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    memset(&mctx, 0, sizeof mctx);
    g_serve_ctx  = &mctx;
    g_feed_calls = 0;
    g_feed_done  = 0;
    if (len && write(sv[0], payload, len) != (ssize_t)len) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    shutdown(sv[0], SHUT_WR);
    serve(sv[1], (ota_state_t *)stbuf, &mctx);
    n = read(sv[0], out, sizeof *out);
    close(sv[0]);
    return n == (ssize_t)sizeof *out ? 0 : -1;
}

int main(void)
{
    struct aup_bin b;
    mm_ota_msg     msg;
    int            violations        = 0;
    int            sets_during_flash = 0;
    int            sets_before       = 0;
    struct stat    lst;

    g_thread_rc = OSIF_OK;
    CHECK(mm_ota_start() == 0 && g_thread_calls == 1, "mm_ota_start spawns the listener thread");
    g_thread_rc = OSIF_ERR;
    CHECK(mm_ota_start() == -1 && g_thread_calls == 2,
            "a failed thread spawn is reported, not ignored");

    for (size_t i = 0; i < sizeof g_slots / sizeof g_slots[0]; i++) {
        const slot_case *c   = &g_slots[i];
        char             run = running_slot(c->env);
        char             got = run_slot("rootfs", c->env);
        int              overwrote;

        CHECK(got == c->slot, "%s_partition='%s' selects slot %c", "rootfs", disp(c->env), c->slot);
        CHECK(!strcmp(g_part, c->part), "%s_partition='%s' flashes '%s'", "rootfs", disp(c->env),
                c->part);
        overwrote = got == run;
        violations += overwrote;
        sets_during_flash += g_list_calls;
        CHECK(overwrote == c->overwrites_running, "'%s': running slot %c, chosen slot %c%s",
                disp(c->env), run, got,
                c->overwrites_running ? " (RECORDED DEFECT: the running slot is overwritten)" :
                                        " -> the spare");
    }
    CHECK(violations == 0, "none of the %zu env values overwrite the slot U-Boot boots from",
            sizeof g_slots / sizeof g_slots[0]);
    CHECK(sets_during_flash == 0,
            "picking a slot never writes the env: the switch is queued, not committed");

    log_reset();
    CHECK(run_slot("rootfs", NULL) == 0 && g_part[0] == 0,
            "an unset or unreadable %s_partition selects NO slot", "rootfs");
    CHECK(g_name_calls == 0 && g_erase_calls == 0 && g_write_calls == 0 && g_list_calls == 0,
            "...the bin is refused before any mtd lookup, erase, write or env write: "
            "slot B may be the one running");
    CHECK(strstr(g_log, "rootfs" OTA_ENV_SUFFIX "' is unset") != NULL,
            "...and the ERR line names the missing key");

    CHECK(!strcmp(g_ctx.env_key, "rootfs" OTA_ENV_SUFFIX), "the env key is the bin name plus '%s'",
            OTA_ENV_SUFFIX);
    run_slot("rootfs", "B");
    CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_ctx.stage_fd >= 0,
            "beginning a bin opens its memory stage and touches no flash");
    CHECK(g_ctx.mtdnum == 7 && g_ctx.partsize == 4096,
            "the mtd number and size of the chosen partition are latched");

    CHECK(OTA_BIN_NAME_MAX + (int)sizeof(OTA_ENV_SUFFIX) <= OTA_PART_NAME_MAX,
            "the env-key overflow guard cannot trip: a bin name is capped at %d and "
            "%d + %d <= %d",
            OTA_BIN_NAME_MAX, OTA_BIN_NAME_MAX, (int)sizeof(OTA_ENV_SUFFIX), OTA_PART_NAME_MAX);
    CHECK(OTA_BIN_NAME_MAX + 3 <= OTA_PART_NAME_MAX,
            "the '<name>_x' partition name cannot overflow its %d-byte buffer", OTA_PART_NAME_MAX);

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&b, 0, sizeof b);
    memcpy(b.name, "abcdefghijklmno", 15);
    b.ofst = -1;
    b.size = -1;
    env_seed("abcdefghijklmno" OTA_ENV_SUFFIX, "A");
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0,
            "a 15-byte bin name, the longest the packager can emit, is accepted");
    CHECK(!strcmp(g_mtd_name[0], "abcdefghijklmno_b") && strlen(g_mtd_name[0]) == 17,
            "the name stops at its terminator and does not run into the next header "
            "field");
    CHECK(!strcmp(g_ctx.env_key, "abcdefghijklmno" OTA_ENV_SUFFIX) && strlen(g_ctx.env_key) == 25,
            "the env key stops there too");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&b, 0, sizeof b);
    memcpy(b.name, "abcdefghijklmnop", 16);
    b.ofst = -1;
    b.size = -1;
    CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.env_slot == 0,
            "a 16-byte bin name with no terminator is REFUSED: the packager caps a "
            "name at 15 bytes so a real one always carries one");
    CHECK(g_name_calls == 0 && g_erase_calls == 0 && g_list_calls == 0,
            "...and reaches no mtd lookup, no erase and no env write");

    {
        static const char *const good[] = { "uboot", "uboot_env", "linux", "rootfs", "app" };

        for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
            struct aup_hdr h;
            char           want[64];

            memset(&h, 0, sizeof h);
            h.totalbins = aup_be32(1);
            bin_name(&h.f[0], good[i]);
            h.f[0].partsize = aup_be32(4096);
            env_reset();
            mtd_reset();
            memset(&g_ctx, 0, sizeof g_ctx);
            if (!strcmp(good[i], OTA_ENV_SKIP)) {
                CHECK(mtd_sink_check_part(&g_ctx, &h) == 0 && g_name_calls == 0,
                        "'%s' is accepted WITHOUT a partition probe: mtd_sink_begin "
                        "never flashes it, so its size cannot refuse the package",
                        good[i]);
                continue;
            }
            snprintf(want, sizeof want, "%s" OTA_ENV_SUFFIX, good[i]);
            env_seed(want, "A");
            snprintf(want, sizeof want, "%s_a", good[i]);
            CHECK(mtd_sink_check_part(&g_ctx, &h) == 0 && !strcmp(g_mtd_name[0], want),
                    "the packager's own bin name '%s' is accepted and sized against "
                    "'%s'",
                    good[i], want);
        }

        for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
            char slot_a[64], slot_b[64];

            if (!strcmp(good[i], OTA_ENV_SKIP))
                continue;
            snprintf(slot_a, sizeof slot_a, "%s_a", good[i]);
            snprintf(slot_b, sizeof slot_b, "%s_b", good[i]);
            CHECK(run_slot(good[i], "B") == 'A' && !strcmp(g_part, slot_a),
                    "'%s' with the running slot B flashes '%s'", good[i], slot_a);
            CHECK(run_slot(good[i], "A") == 'B' && !strcmp(g_part, slot_b),
                    "'%s' with the running slot A flashes '%s'", good[i], slot_b);
        }
    }

    {
        static const char *const bad[] = {
            "../uboot",
            "..",
            ".",
            "a/b",
            "a\\b",
            "uboot.bin",
            "",
            "rootfs ",
            "uboot=x",
            "roo\tfs",
            "roo\nfs",
            "\x01uboot",
            "rootfs\x7f",
            "/rootfs",
            "..\\..\\uboot",
        };

        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            struct aup_hdr h;

            env_reset();
            mtd_reset();
            log_reset();
            memset(&g_ctx, 0, sizeof g_ctx);
            bin_name(&b, bad[i]);
            CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.env_slot == 0,
                    "an image-supplied bin name '%s' is REFUSED", disp(bad[i]));
            CHECK(g_name_calls == 0 && g_erase_calls == 0 && g_write_calls == 0 &&
                            g_list_calls == 0,
                    "...'%s' reaches no mtd lookup, no erase, no write and no env "
                    "write",
                    disp(bad[i]));
            CHECK(g_log_calls == 1 && strstr(g_log, "not a partition name") != NULL,
                    "...'%s' is reported once, at ERR", disp(bad[i]));
            CHECK(strchr(g_log, '/') == NULL && strchr(g_log, '\\') == NULL &&
                            strchr(g_log, '.') == NULL && strchr(g_log, '=') == NULL &&
                            strchr(g_log, '\t') == NULL && strchr(g_log, '\n') == NULL &&
                            strchr(g_log, '\x01') == NULL && strchr(g_log, '\x7f') == NULL,
                    "...and the ERR line quoting '%s' carries none of its own bytes: "
                    "every one outside [A-Za-z0-9_] became '?'",
                    disp(bad[i]));

            memset(&h, 0, sizeof h);
            h.totalbins     = aup_be32(1);
            h.f[0]          = b;
            h.f[0].partsize = aup_be32(4096);
            env_reset();
            mtd_reset();
            CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_HEAER_ERR,
                    "...and '%s' refuses the whole image at check_part, before any "
                    "bin begins",
                    disp(bad[i]));
            CHECK(g_name_calls == 0, "...without check_part probing a partition for '%s' either",
                    disp(bad[i]));
        }
    }

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    bin_name(&b, OTA_ENV_SKIP);
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0 && g_ctx.mtdnum == -1 && g_ctx.env_slot == 0,
            "'%s' claims no partition", OTA_ENV_SKIP);
    CHECK(g_name_calls == 0 && g_erase_calls == 0, "'%s' looks up no mtd and erases nothing",
            OTA_ENV_SKIP);
    CHECK(mtd_sink_write(&g_ctx, &b, 0, "x", 1) == 0 && g_write_calls == 0,
            "'%s' payload is swallowed, never flashed", OTA_ENV_SKIP);
    CHECK(mtd_sink_end(&g_ctx, &b) == 0 && g_ctx.npending == 0, "'%s' queues no slot switch",
            OTA_ENV_SKIP);

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    bin_name(&b, "rootfs");
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_mtd_num = -1;
    CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.env_slot == 0,
            "a missing partition fails the bin and queues no slot switch");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_erase_rc = -1;
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0 && mtd_sink_end(&g_ctx, &b) == -1 && g_ctx.mtdnum == -1 &&
                    g_ctx.npending == 0 && g_write_calls == 0 && g_ctx.stage_fd == -1,
            "a failed erase fails the bin, writes nothing, releases the stage and queues no "
            "slot switch");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    {
        struct rlimit old, lim;

        getrlimit(RLIMIT_NOFILE, &old);
        lim          = old;
        lim.rlim_cur = 0;
        setrlimit(RLIMIT_NOFILE, &lim);
        CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.mtdnum == -1 && g_ctx.env_slot == 0 &&
                        g_ctx.stage_fd < 0,
                "a memory stage that cannot be created fails the bin");
        setrlimit(RLIMIT_NOFILE, &old);
    }
    CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0,
            "...before any erase or write");

    env_reset();
    mtd_reset();
    log_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    meminfo_set("MemTotal:         65536 kB\nMemFree:            1 kB\n"
                "MemAvailable:         2 kB\n");
    b.size = aup_be32(2049);
    CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.mtdnum == -1 && g_ctx.env_slot == 0 &&
                    g_ctx.stage_fd < 0,
            "a bin one byte larger than MemAvailable (2 kB) is refused before it is staged");
    CHECK(strstr(g_log, "'rootfs' is 2049 bytes, only 2048 bytes of RAM") != NULL,
            "...and the ERR line names the bin, its size and the RAM available");
    CHECK(mtd_sink_end(&g_ctx, &b) == -1 && g_erase_calls == 0 && g_write_calls == 0 &&
                    g_end_calls == 0 && g_ctx.npending == 0 && g_list_calls == 0,
            "...so nothing is erased, written or queued as a slot switch");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    b.size = aup_be32(2048);
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0 && g_ctx.stage_fd >= 0,
            "a bin exactly as large as MemAvailable is staged");
    stage_release(&g_ctx);

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    meminfo_set("MemTotal:         65536 kB\nMemFree:            1 kB\n");
    b.size = aup_be32(2049);
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0 && g_ctx.stage_fd >= 0,
            "a meminfo without MemAvailable skips the RAM check");
    stage_release(&g_ctx);
    b.size         = 0;
    g_meminfo_fake = 0;
    unlink(TST_MEMINFO);

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_mtd_size = 0;
    CHECK(mtd_sink_begin(&g_ctx, &b) == -1 && g_ctx.mtdnum == -1 && g_ctx.env_slot == 0,
            "a zero-size partition fails the bin");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    b.size = aup_be32(1);
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0, "a normal bin begins");
    CHECK(mtd_sink_write(&g_ctx, &b, 0, "x", 1) == 0 && g_write_calls == 0 && g_erase_calls == 0,
            "a payload byte is staged, not yet written to the mtd");
    CHECK(mtd_sink_write(&g_ctx, &b, 4096, "x", 1) == -1,
            "a write past the end of the partition is refused");
    CHECK(mtd_sink_write(&g_ctx, &b, -1, "x", 1) == -1, "a negative offset is refused");
    CHECK(mtd_sink_write(&g_ctx, &b, 4096, "", 0) == 0,
            "a zero-length write at the end is accepted and writes nothing");
    CHECK(g_write_calls == 0 && g_erase_calls == 0, "no flash is touched before the end");
    CHECK(mtd_sink_end(&g_ctx, &b) == 0 && g_end_calls == 1 && g_end_commit == 1 &&
                    g_end_mtdnum == 7,
            "a completed bin commits its last page");
    CHECK(g_erase_calls == 1 && g_erase_start == 0 && g_erase_size == 4096 && g_write_erased == 1,
            "...after erasing the chosen partition whole, before its first write");
    CHECK(g_write_calls == 1 && g_write_off == 0 && g_write_len == 1 && g_flash[0] == 'x',
            "...and the staged byte reaches the mtd");
    CHECK(g_ctx.stage_fd == -1, "...and the memory stage is released");
    b.size = 0;
    CHECK(g_ctx.npending == 1 && !strcmp(g_ctx.pending[0].key, "rootfs" OTA_ENV_SUFFIX) &&
                    g_ctx.pending[0].slot == 'A',
            "the slot switch is queued as %s=A", "rootfs" OTA_ENV_SUFFIX);
    CHECK(g_ctx.mtdnum == -1 && g_ctx.env_slot == 0, "the context is cleared for the next bin");
    CHECK(g_list_calls == 0, "mtd_sink_end still writes no env");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0, "a bin begins for the write_end failure case");
    g_end_rc = -1;
    CHECK(mtd_sink_end(&g_ctx, &b) == -1 && g_ctx.npending == 0,
            "a last page that will not flush queues no slot switch");

    env_reset();
    mtd_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    CHECK(mtd_sink_begin(&g_ctx, &b) == 0, "a bin begins for the queue overflow case");
    g_ctx.npending = OTA_ENV_PENDING;
    CHECK(mtd_sink_end(&g_ctx, &b) == -1, "more than %d queued slot switches fails the image",
            OTA_ENV_PENDING);

    {
        static unsigned char img[3 * OTA_CHUNK + 1234];
        static const size_t  cuts[] = { 1, 4095, 4097, 7, sizeof img - 1 - 4095 - 4097 - 7 };
        size_t               off    = 0;
        int                  ok     = 1;

        for (size_t i = 0; i < sizeof img; i++)
            img[i] = (unsigned char)(i * 7 + 3);
        env_reset();
        mtd_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        mtd_slot("rootfs_a", 10, FLASH_CAP);
        bin_name(&b, "rootfs");
        b.size = aup_be32((int32_t)sizeof img);
        CHECK(mtd_sink_begin(&g_ctx, &b) == 0, "a %zu-byte bin begins", sizeof img);
        for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
            ok &= mtd_sink_write(&g_ctx, &b, (long long)off, img + off, cuts[i]) == 0;
            off += cuts[i];
        }
        CHECK(ok && off == sizeof img && g_write_calls == 0,
                "...its bytes are staged in uneven pieces without touching the mtd");
        CHECK(mtd_sink_end(&g_ctx, &b) == 0 && g_erase_calls == 1 && g_write_erased == 1 &&
                        g_end_commit == 1 && g_end_mtdnum == 10,
                "...and its end erases rootfs_a, then writes and commits it");
        CHECK(!memcmp(g_flash, img, sizeof img) &&
                        g_write_off + (long long)g_write_len == (long long)sizeof img,
                "...with exactly the staged bytes, in order, and nothing past them");
        b.size = 0;
    }

    env_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    for (int i = 0; i < 4; i++) {
        snprintf(g_ctx.pending[i].key, sizeof g_ctx.pending[i].key, "bin%d%s", i + 1,
                OTA_ENV_SUFFIX);
        g_ctx.pending[i].slot = (i % 2) ? 'B' : 'A';
    }
    g_ctx.npending = 4;
    log_reset();
    CHECK(env_flush(&g_ctx) == 0 && g_list_calls == 1,
            "a clean flush commits all four queued keys in ONE environment store, "
            "not one store per key");
    CHECK(g_list_keys == 4 && !strcmp(g_list_path, OTA_ENV_LIST_FILE) &&
                    !strcmp(g_list_body,
                            "bin1" OTA_ENV_SUFFIX "=A\n"
                            "bin2" OTA_ENV_SUFFIX "=B\n"
                            "bin3" OTA_ENV_SUFFIX "=A\n"
                            "bin4" OTA_ENV_SUFFIX "=B\n"),
            "...from one list file carrying every key in the 'name=value' form "
            "libuboot_load_file parses");
    CHECK(env_is("bin1" OTA_ENV_SUFFIX, "A") && env_is("bin2" OTA_ENV_SUFFIX, "B") &&
                    env_is("bin3" OTA_ENV_SUFFIX, "A") && env_is("bin4" OTA_ENV_SUFFIX, "B"),
            "each key is written as the single uppercase letter A or B");
    CHECK(g_ctx.npending == 0, "the queue is emptied by a clean flush");
    CHECK(access(OTA_ENV_LIST_FILE, F_OK) != 0, "...and the list file does not outlive the flush");
    CHECK(g_log_calls == 1,
            "a clean flush is ONE log line naming every key, not one per key (got %d)",
            g_log_calls);

    env_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    for (int i = 0; i < 4; i++) {
        snprintf(g_ctx.pending[i].key, sizeof g_ctx.pending[i].key, "bin%d%s", i + 1,
                OTA_ENV_SUFFIX);
        g_ctx.pending[i].slot = (i % 2) ? 'B' : 'A';
    }
    g_ctx.npending = 4;
    g_list_fail    = 1;
    log_reset();
    CHECK(env_flush(&g_ctx) == ENV_FLUSH_FAIL && g_list_calls == 1,
            "a failed store fails the whole flush");
    CHECK(env_absent("bin1" OTA_ENV_SUFFIX) && env_absent("bin2" OTA_ENV_SUFFIX) &&
                    env_absent("bin3" OTA_ENV_SUFFIX) && env_absent("bin4" OTA_ENV_SUFFIX),
            "...and NOT ONE key is switched: one store cannot leave a half-switched "
            "environment behind");
    CHECK(g_list_keys == 4,
            "...the failed store was still asked for all four keys at once, so the "
            "failure is all-or-nothing rather than per-key");
    CHECK(g_ctx.npending == 0, "the queue is discarded even on failure, so no retry is possible");
    CHECK(g_log_calls == 1, "the failed flush logs ONE line, not one per key (got %d)",
            g_log_calls);
    CHECK(strstr(g_log, "bin1" OTA_ENV_SUFFIX "=A") != NULL &&
                    strstr(g_log, "bin2" OTA_ENV_SUFFIX "=B") != NULL &&
                    strstr(g_log, "bin3" OTA_ENV_SUFFIX "=A") != NULL &&
                    strstr(g_log, "bin4" OTA_ENV_SUFFIX "=B") != NULL,
            "...naming every key that did not switch");
    CHECK(strstr(g_log, "NOT committed") != NULL, "...and saying the switch was not committed");
    CHECK(strstr(g_log, "re-run the upgrade") != NULL,
            "...so it tells the operator to re-run, which is the safe recovery");
    CHECK(strstr(g_log, "MIXED") == NULL && strstr(g_log, "do not re-run") == NULL,
            "...and neither reports a mixed state nor forbids a retry");
    CHECK(access(OTA_ENV_LIST_FILE, F_OK) != 0, "the list file is removed on the failure path too");

    env_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    snprintf(g_ctx.pending[0].key, sizeof g_ctx.pending[0].key, "bin1" OTA_ENV_SUFFIX);
    g_ctx.pending[0].slot = 'A';
    g_ctx.npending        = 1;
    g_list_fail           = 1;
    log_reset();
    CHECK(env_flush(&g_ctx) == ENV_FLUSH_FAIL && g_list_calls == 1 &&
                    env_absent("bin1" OTA_ENV_SUFFIX),
            "a single queued key fails the same way and writes nothing at all");
    CHECK(g_log_calls == 1, "...in one log line");

    env_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    snprintf(g_ctx.pending[0].key, sizeof g_ctx.pending[0].key, "bin1" OTA_ENV_SUFFIX);
    g_ctx.pending[0].slot = 'B';
    g_ctx.npending        = 1;
    unlink("mm_ota_test.envlist_target");
    CHECK(symlink("mm_ota_test.envlist_target", OTA_ENV_LIST_FILE) == 0,
            "a symlink is planted at the env list path");
    CHECK(env_flush(&g_ctx) == 0 && env_is("bin1" OTA_ENV_SUFFIX, "B") &&
                    !strcmp(g_list_body, "bin1" OTA_ENV_SUFFIX "=B\n") &&
                    access("mm_ota_test.envlist_target", F_OK) != 0,
            "the env list replaces a planted symlink instead of following it");
    unlink("mm_ota_test.envlist_target");

    env_reset();
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.npending = 0;
    CHECK(env_flush(&g_ctx) == 0 && g_list_calls == 0,
            "an empty queue flushes clean and stores NOTHING: the env partition is "
            "not erased and rewritten for zero keys");

    CHECK(rtrim_len("", 0) == 0, "rtrim_len of the empty string is 0");
    CHECK(rtrim_len("B", 0) == 0, "rtrim_len honours n over the terminator");
    CHECK(rtrim_len("   ", 3) == 0, "an all-space string trims to nothing");
    CHECK(rtrim_len(" \t\r\n", 4) == 0, "space, tab, CR and LF are all trimmed");
    CHECK(rtrim_len("B", 1) == 1, "a bare letter is untouched");
    CHECK(rtrim_len("B ", 2) == 1, "one trailing space is trimmed");
    CHECK(rtrim_len("B  \t\r\n", 6) == 1, "a run of trailing whitespace is trimmed");
    CHECK(rtrim_len(" B", 2) == 2, "LEADING whitespace is NOT trimmed");
    CHECK(rtrim_len("  B  ", 5) == 3, "only the trailing run goes");
    CHECK(rtrim_len("a b ", 4) == 3, "embedded whitespace is preserved");
    CHECK(rtrim_len("ABC", 3) == 3, "a string with no trailing whitespace is unchanged");
    CHECK(rtrim_len("B\v", 2) == 2, "vertical tab is not whitespace here");
    CHECK(rtrim_len("B\f", 2) == 2, "form feed is not whitespace here");

    {
        struct aup_hdr h;

        memset(&h, 0, sizeof h);
        memset(h.sw, ' ', sizeof h.sw);
        memcpy(h.sw, "0.3.7", 5);
        memset(h.hw, ' ', sizeof h.hw);
        memcpy(h.hw, "nano3s", 6);
        h.totalbins = aup_be32(1);
        memcpy(h.f[0].name, "rootfs", 6);
        h.f[0].partsize = aup_be32(4096);

        env_reset();
        mtd_reset();
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_SW_ERR,
                "an unset SW env refuses the image");
        CHECK(g_name_calls == 0,
                "...and check_hdr never probes a partition: that gate is "
                "check_part's, and it runs only after the signature");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_HW_ERR,
                "a matching SW with an unset HW refuses the image");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        log_reset();
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR,
                "a board with no %s_partition refuses the image at check_part", "rootfs");
        CHECK(g_name_calls == 0 && g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0 &&
                        g_list_calls == 0,
                "...before any bin begins: no mtd is probed, erased or written and "
                "no env is written");
        CHECK(strstr(g_log, "rootfs" OTA_ENV_SUFFIX "' is unset") != NULL,
                "...and the ERR line names the missing key");

        {
            struct aup_hdr h2 = h;

            h2.totalbins = aup_be32(2);
            bin_name(&h2.f[0], "uboot");
            h2.f[0].partsize = aup_be32(4096);
            bin_name(&h2.f[1], "rootfs");
            h2.f[1].partsize = aup_be32(4096);
            env_reset();
            mtd_reset();
            env_seed("uboot" OTA_ENV_SUFFIX, "B");
            CHECK(mtd_sink_check_part(&g_ctx, &h2) == UPGRADE_PARTITION_ERR,
                    "a two-bin image whose second bin has no %s_partition is "
                    "refused whole",
                    "rootfs");
            CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0 && g_list_calls == 0,
                    "...so the first bin, whose key is set, is not flashed either");
        }

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == 0,
                "space-padded header fields match the unpadded env values");
        CHECK(mtd_sink_check_part(&g_ctx, &h) == 0,
                "...and the declared partition size matches the flash's");
        CHECK(!strcmp(g_mtd_name[0], "rootfs_a") && !strcmp(g_mtd_name[1], "rootfs_b"),
                "the size check probes slot A's partition, then slot B's");

        env_reset();
        mtd_reset();
        log_reset();
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        g_mtd_num  = -1;
        g_mtd_size = 4096;
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR,
                "a board with no rootfs_a partition refuses the image at check_part, "
                "even when the size lookup would have matched");
        CHECK(strstr(g_log, "'rootfs_a' not found") != NULL && strstr(g_log, "mtd-1") == NULL,
                "...and the line names the missing partition, not mtd-1");
        CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_list_calls == 0,
                "...and nothing is erased, written or switched");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7  ");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == 0,
                "a trailing-space env value matches too: both sides are right-trimmed");

        env_reset();
        mtd_reset();
        env_seed("SW", " 0.3.7");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_SW_ERR,
                "a LEADING-space env value does not match");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.8");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_SW_ERR, "a different SW refuses the image");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "x3");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_HW_ERR, "a different HW refuses the image");

        env_reset();
        mtd_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        g_mtd_size = 2048;
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR,
                "a declared partition size that is not the flash's refuses the image");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == 0,
                "...while check_hdr passes it: a wrong size is not an SW or "
                "HW refusal");
    }

    {
        struct aup_hdr h;

        memset(&h, 0, sizeof h);
        memset(h.sw, ' ', sizeof h.sw);
        memcpy(h.sw, "0.3.7", 5);
        memset(h.hw, ' ', sizeof h.hw);
        memcpy(h.hw, "nano3s", 6);
        h.totalbins = aup_be32(1);
        bin_name(&h.f[0], "rootfs");
        h.f[0].partsize = aup_be32(4096);
        bin_name(&b, "rootfs");

        env_reset();
        mtd_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == 0 && g_name_calls == 2 &&
                        !strcmp(g_mtd_name[0], "rootfs_a") && !strcmp(g_mtd_name[1], "rootfs_b"),
                "rootfs_a 4096, rootfs_b 4096: a package declaring 4096 passes the size "
                "gate, which probes both slots");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 2048);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        !strcmp(g_mtd_name[1], "rootfs_b"),
                "rootfs_a 4096, rootfs_b 2048, running slot A: a package declaring "
                "4096 is REFUSED, because rootfs_b, the slot it would write, is short");
        CHECK(strstr(g_log, "'rootfs_b' is 2048 bytes") != NULL,
                "...and the ERR line names rootfs_b");
        CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_list_calls == 0,
                "...and nothing is erased, written or switched");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 2048);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        strstr(g_log, "'rootfs_b' is 2048 bytes") != NULL,
                "the same pair with the running slot B is refused the same way: both "
                "slots must match the declared size");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        mtd_slot("rootfs_a", 10, 4096);
        g_mtd_num = -1;
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        strstr(g_log, "'rootfs_b' not found") != NULL,
                "a board with rootfs_a but no rootfs_b refuses the image at check_part");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        mtd_slot("rootfs_a", 10, 2048);
        mtd_slot("rootfs_b", 11, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR,
                "rootfs_a 2048, rootfs_b 4096, running slot B: a package declaring "
                "4096 is REFUSED, and rootfs_a is the slot it would have written");
        CHECK(strstr(g_log, "'rootfs_a' is 2048 bytes") != NULL,
                "...and the ERR line names the partition that was measured, so an "
                "operator can tell which slot the refusal is about");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        mtd_slot("rootfs_a", 10, 2048);
        mtd_slot("rootfs_b", 11, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        !strcmp(g_mtd_name[0], "rootfs_a"),
                "the same pair with the running slot A is REFUSED TOO, although the "
                "4096 it declares is exactly what rootfs_b — the slot it would have "
                "written — holds");
        CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_list_calls == 0,
                "...and a refusal at the size gate erases nothing, writes nothing and "
                "switches no slot");
    }

    {
        struct aup_hdr h;

        memset(&h, 0, sizeof h);
        h.totalbins = aup_be32(3);
        bin_name(&h.f[0], "linux");
        h.f[0].partsize = aup_be32(4096);
        bin_name(&h.f[1], "rootfs");
        h.f[1].partsize = aup_be32(4096);
        bin_name(&h.f[2], "app");
        h.f[2].partsize = aup_be32(4096);

        ubi_clear();
        ubi_fake("ubi0", 10);
        ubi_fake("ubi0_0", -1);
        ubi_fake("ubi1", 13);
        ubi_fake("ubi1_0", -1);

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("linux" OTA_ENV_SUFFIX, "A");
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        env_seed("app" OTA_ENV_SUFFIX, "B");
        mtd_slot("linux_a", 8, 4096);
        mtd_slot("linux_b", 9, 4096);
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 4096);
        mtd_slot("app_a", 12, 4096);
        mtd_slot("app_b", 13, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == 0,
                "rootfs on mtd10 (ubi0) and app on mtd13 (ubi1) as the env says: the "
                "targets rootfs_b and app_a are free and the package passes");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("linux" OTA_ENV_SUFFIX, "B");
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        env_seed("app" OTA_ENV_SUFFIX, "B");
        mtd_slot("linux_a", 8, 4096);
        mtd_slot("linux_b", 9, 4096);
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 4096);
        mtd_slot("app_a", 12, 4096);
        mtd_slot("app_b", 13, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR,
                "an env already flipped to B while ubi0 still runs rootfs_a (mtd10): the "
                "package is REFUSED, because its target rootfs_a is the running slot");
        CHECK(strstr(g_log, "'rootfs_a' (mtd10) is attached to UBI") != NULL,
                "...and the ERR line names the partition");
        CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0 && g_list_calls == 0,
                "...before any bin begins: nothing erased, written or switched");

        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("linux" OTA_ENV_SUFFIX, "A");
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        env_seed("app" OTA_ENV_SUFFIX, "A");
        mtd_slot("linux_a", 8, 4096);
        mtd_slot("linux_b", 9, 4096);
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 4096);
        mtd_slot("app_a", 12, 4096);
        mtd_slot("app_b", 13, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        strstr(g_log, "'app_b' (mtd13) is attached to UBI") != NULL,
                "app_partition=A while ubi1 runs app_b (mtd13): REFUSED the same way");
        CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_list_calls == 0,
                "...and nothing is erased, written or switched");

        ubi_fake("ubi2", -1);
        env_reset();
        mtd_reset();
        log_reset();
        memset(&g_ctx, 0, sizeof g_ctx);
        env_seed("linux" OTA_ENV_SUFFIX, "A");
        env_seed("rootfs" OTA_ENV_SUFFIX, "A");
        env_seed("app" OTA_ENV_SUFFIX, "B");
        mtd_slot("linux_a", 8, 4096);
        mtd_slot("linux_b", 9, 4096);
        mtd_slot("rootfs_a", 10, 4096);
        mtd_slot("rootfs_b", 11, 4096);
        mtd_slot("app_a", 12, 4096);
        mtd_slot("app_b", 13, 4096);
        CHECK(mtd_sink_check_part(&g_ctx, &h) == UPGRADE_PARTITION_ERR &&
                        strstr(g_log, "UBI state unreadable") != NULL,
                "a UBI device whose mtd_num cannot be read refuses the package rather "
                "than risk the running slot");
        ubi_clear();
    }

    {
        static const char raw[] = "0.\n3\r.\t7\x01\x7f\xff";
        struct aup_hdr    h;

        memset(&h, 0, sizeof h);
        h.totalbins = aup_be32(1);
        memcpy(h.f[0].name, "rootfs", 6);
        h.f[0].partsize = aup_be32(4096);
        memcpy(h.sw, raw, sizeof raw - 1);
        memset(h.hw, ' ', sizeof h.hw);
        memcpy(h.hw, "nano3s", 6);

        env_reset();
        mtd_reset();
        log_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_SW_ERR,
                "an SW field carrying LF, CR, TAB, 0x01, DEL and 0xff refuses the "
                "image");
        CHECK(g_log_calls == 1 && strstr(g_log, "does not match this board") != NULL,
                "...and is reported once, at ERR — before the signature has been "
                "checked, so these are UNAUTHENTICATED bytes");
        CHECK(log_is_printable(),
                "...and the whole ERR line is printable ASCII: none of those bytes "
                "reached the journal, so the field cannot forge a log line");
        CHECK(strstr(g_log, "'0.?3?.?7?\?\?'") != NULL,
                "...every byte outside 0x20..0x7e became '?' and the rest is "
                "verbatim");

        memset(h.sw, ' ', sizeof h.sw);
        memcpy(h.sw, "0.3.7", 5);
        memcpy(h.hw, raw, sizeof raw - 1);
        env_reset();
        mtd_reset();
        log_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_HW_ERR,
                "the same bytes in the HW field refuse the image");
        CHECK(g_log_calls == 1 && log_is_printable() && strstr(g_log, "'0.?3?.?7?\?\?'") != NULL,
                "...and are scrubbed on that path too: one printable ERR line");

        memset(h.hw, 0, sizeof h.hw);
        memcpy(h.hw, "na-no3s.1", 9);
        env_reset();
        mtd_reset();
        log_reset();
        env_seed("SW", "0.3.7");
        env_seed("HW", "nano3s");
        CHECK(mtd_sink_check_hdr(&g_ctx, &h) == UPGRADE_HW_ERR &&
                        strstr(g_log, "'na-no3s.1'") != NULL,
                "a mismatching but printable field is quoted unchanged: '.' and '-' "
                "are legitimate in an SW or HW value, unlike in a bin name");
    }

    CHECK(sizeof(mm_ota_msg) == 12, "the status message the web side reads is 12 bytes");
    CHECK(offsetof(mm_ota_msg, magic) == 0 && offsetof(mm_ota_msg, type) == 4 &&
                    offsetof(mm_ota_msg, upgrade) == 8,
            "magic, type, upgrade at 0, 4, 8");
    CHECK(sizeof(mm_ota_data) == 4, "the upgrade word is 4 bytes");
    CHECK(MM_OTA_MSG_TYPE == 1, "the message type is 1");
    CHECK((uint32_t)(int)MM_OTA_MAGIC == 0xdeadbeafu,
            "the magic is 0xdeadbeaf once the int cast is undone");
    CHECK(!strcmp(MM_OTA_SOCK_PATH, "/tmp/sockupctrl"), "the listener path is /tmp/sockupctrl");
    CHECK(MM_OTA_REBOOT_DELAY_MS == 3000u, "the post-upgrade reboot delay is 3000 ms");
    {
        mm_ota_data   d;
        unsigned char raw[4];

        d.data    = 0;
        d.stat    = 1;
        d.percent = 2;
        d.error   = -3;
        d.set     = 4;
        memcpy(raw, &d, sizeof raw);
        CHECK(raw[0] == 1 && raw[1] == 2 && raw[2] == (unsigned char)-3 && raw[3] == 4,
                "stat, percent, error, set are bytes 0..3 of the word");
    }

    {
        struct stat st;
        int         fd, fd2;

        unlink(TST_SOCK);
        fd = listener_create(TST_SOCK);
        CHECK(fd >= 0, "listener_create binds and listens");
        CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC),
                "the listener is close-on-exec from creation");
        CHECK(stat(TST_SOCK, &st) == 0 && S_ISSOCK(st.st_mode), "the path is a unix socket");
        CHECK((st.st_mode & 07777) == (S_IRUSR | S_IWUSR), "the socket is mode 0600: owner only");
        fd2 = listener_create(TST_SOCK);
        CHECK(fd2 >= 0, "a stale socket path is unlinked and rebound");
        if (fd >= 0)
            close(fd);
        if (fd2 >= 0)
            close(fd2);
        unlink(TST_SOCK);
    }

    g_feed_hook = feed_one_bin;

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_SUCCEED;
    g_feed_percent = 100;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0, "a completed upload is answered");
    CHECK((uint32_t)msg.magic == 0xdeadbeafu && msg.type == MM_OTA_MSG_TYPE,
            "the reply carries the 0xdeadbeaf/1 framing the web side checks");
    CHECK(msg.upgrade.stat == 0 && msg.upgrade.error == UPGRADE_SUCCEED &&
                    msg.upgrade.percent == 100 && msg.upgrade.set == 0,
            "the reply reports stat 0, UPGRADE_SUCCEED, the last percent and the "
            "spare byte left at 0");
    CHECK(!strcmp(g_mtd_name[0], "rootfs_a"), "the spare slot was the one flashed");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "A"), "a successful image commits the slot switch");
    CHECK(g_reboot_calls == 1, "the reboot is armed");

    {
        static char tail[OTA_CHUNK + 8];

        memset(tail, 'x', sizeof tail);

        latch_reset();
        env_reset();
        mtd_reset();
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        g_feed_rc      = UPGRADE_SUCCEED;
        g_feed_percent = 100;
        g_reboot_calls = 0;
        CHECK(serve_run(tail, sizeof tail, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED &&
                        msg.upgrade.percent == 100,
                "an upload whose last bin completes with %zu bytes still on the "
                "socket is still a SUCCESS on the wire: trailing bytes cannot turn "
                "it into UPGRADE_ALREADY_ERR",
                sizeof tail - OTA_CHUNK);
        CHECK(g_feed_calls == 1,
                "...because once the image is complete the rest of the body is "
                "drained and never fed back into the state machine");
        CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "A"), "...the slot switch is committed");
        CHECK(g_reboot_calls == 1, "...the reboot is armed");
        CHECK(g_ota_success == 1 && access(OTA_SUCCEED_FILE, F_OK) == 0,
                "...and the success is latched in RAM and on disk");

        latch_reset();
        env_reset();
        mtd_reset();
        env_seed("rootfs" OTA_ENV_SUFFIX, "B");
        g_feed_rc      = UPGRADE_CHECK_SHA_ERR;
        g_reboot_calls = 0;
        CHECK(serve_run(tail, sizeof tail, &msg) == 0 && msg.upgrade.error == UPGRADE_CHECK_SHA_ERR,
                "a FAILED upload followed by trailing bytes reports its OWN error, "
                "not the UPGRADE_ALREADY_ERR a second feed would answer");
        CHECK(g_feed_calls == 1,
                "...because the loop stops at the failure and the trailing bytes "
                "are never fed either");
        CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_list_calls == 0 && g_reboot_calls == 0,
                "...and it switches no slot and arms no reboot");
        CHECK(g_ota_success == 0 && access(OTA_SUCCEED_FILE, F_OK) != 0,
                "...and latches nothing, so the operator can retry");
        g_feed_rc = UPGRADE_SUCCEED;
    }

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_SW_ERR;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SW_ERR,
            "a rejected image reports its own error code");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_list_calls == 0,
            "a rejected image leaves the running slot selected");
    CHECK(g_reboot_calls == 0, "a rejected image arms no reboot");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_SUCCEED;
    g_list_fail    = 1;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_PARTITION_ERR,
            "a flashed image whose env store fails is reported as a FAILURE");
    CHECK(g_reboot_calls == 0, "...and arms no reboot into an image that will not boot");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_VERIFY_ERR;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_VERIFY_ERR &&
                    msg.upgrade.set == 0,
            "a signature failure is reported as its own error, with no separate "
            "verification flag on the wire");
    CHECK(g_reboot_calls == 0, "...and arms no reboot");
    g_feed_rc = UPGRADE_SUCCEED;

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_begin_only;
    g_feed_rc      = UPGRADE_PROCESS_OK;
    g_feed_percent = 42;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_PROCESS_OK &&
                    msg.upgrade.percent == 42,
            "a truncated upload is answered as incomplete");
    CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0,
            "the half-received bin never reached the flash: nothing erased or written");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_reboot_calls == 0,
            "a truncated upload switches no slot and arms no reboot");
    g_feed_hook = feed_one_bin;

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_write_fail;
    g_feed_rc      = UPGRADE_IMAGE_ERR;
    g_write_rc     = -1;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_IMAGE_ERR,
            "a failed flash write is answered as an image error");
    CHECK(g_write_calls == 1 && g_end_calls == 1 && g_end_commit == 0,
            "...and the partition is abandoned with commit=0, its tail page dropped");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_reboot_calls == 0 && g_ota_success == 0,
            "...switching no slot, arming no reboot and latching nothing");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_staged_only;
    g_feed_rc      = UPGRADE_CHECK_SHA_ERR;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_CHECK_SHA_ERR,
            "a bin whose SHA-256 does not match is answered as a checksum error");
    CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0,
            "...and its staged bytes never reach the flash: nothing erased or written");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_list_calls == 0 && g_reboot_calls == 0 &&
                    g_ota_success == 0,
            "...switching no slot, arming no reboot and latching nothing");
    CHECK(g_serve_ctx->stage_fd == -1, "...and the memory stage is released");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_stage_fail;
    g_feed_rc      = UPGRADE_IMAGE_ERR;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_IMAGE_ERR,
            "a bin that cannot be staged in memory is answered as an image error");
    CHECK(g_erase_calls == 0 && g_write_calls == 0 && g_end_calls == 0, "...and touches no flash");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "B") && g_list_calls == 0 && g_reboot_calls == 0 &&
                    g_ota_success == 0,
            "...switching no slot, arming no reboot and latching nothing");
    CHECK(g_serve_ctx->stage_fd == -1, "...and the memory stage is released");
    g_feed_hook = feed_one_bin;
    g_feed_rc   = UPGRADE_PROCESS_OK;

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_SUCCEED;
    g_feed_percent = 100;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED,
            "upload 1 of the double-click: it succeeds");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "A") && g_reboot_calls == 1,
            "...flips %s to A and arms the reboot", "rootfs" OTA_ENV_SUFFIX);
    CHECK(g_ota_success == 1, "the success is latched in RAM");
    CHECK(access(OTA_SUCCEED_FILE, F_OK) == 0, "...and on disk at %s", OTA_SUCCEED_FILE);
    CHECK(stat(OTA_SUCCEED_FILE, &lst) == 0 && S_ISREG(lst.st_mode) && lst.st_size == 0,
            "the on-disk latch is an empty regular file: its existence is the whole "
            "content");
    CHECK((lst.st_mode & 07777) == 0, "...created with mode 0, as creat(path, O_RDONLY) leaves it");

    mtd_reset();
    sets_before = g_list_calls;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_ALREADY_ERR,
            "upload 2 inside the %u ms reboot window is REFUSED with "
            "UPGRADE_ALREADY_ERR",
            MM_OTA_REBOOT_DELAY_MS);
    CHECK((uint32_t)msg.magic == 0xdeadbeafu && msg.type == MM_OTA_MSG_TYPE &&
                    msg.upgrade.stat == 0 && msg.upgrade.percent == 0 && msg.upgrade.set == 0,
            "the refusal reaches the operator in the normal framing, finished at 0 "
            "percent");
    CHECK(g_feed_calls == 0, "the refused upload is not fed a single byte");
    CHECK(g_name_calls == 0, "...opens no mtd partition");
    CHECK(g_erase_calls == 0, "...erases nothing: the rollback copy in slot A survives");
    CHECK(g_write_calls == 0 && g_end_calls == 0, "...writes no flash");
    CHECK(g_list_calls == sets_before, "...writes no env");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "A"), "...and leaves the slot upload 1 selected");
    CHECK(g_reboot_calls == 1, "...and arms no second reboot");

    g_ota_success = 0;
    mtd_reset();
    CHECK(access(OTA_SUCCEED_FILE, F_OK) == 0 && serve_run("aup", 3, &msg) == 0 &&
                    msg.upgrade.error == UPGRADE_ALREADY_ERR,
            "an mm_miner that restarted WITHOUT a reboot still refuses: the on-disk "
            "half of the latch alone is enough");
    CHECK(g_feed_calls == 0 && g_name_calls == 0 && g_erase_calls == 0,
            "...and that refusal touches no flash either");

    unlink(OTA_SUCCEED_FILE);
    g_ota_success = 1;
    mtd_reset();
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_ALREADY_ERR,
            "the in-RAM half alone also refuses, with the file removed under us");
    CHECK(g_feed_calls == 0 && g_erase_calls == 0, "...and touches no flash either");

    latch_reset();
    unlink("mm_ota_test.symlink_target");
    CHECK(symlink("mm_ota_test.symlink_target", OTA_SUCCEED_FILE) == 0,
            "a symlink is planted at the latch path");
    ota_latch_set();
    CHECK(g_ota_success == 1 && access("mm_ota_test.symlink_target", F_OK) != 0,
            "the latch does not follow a symlink, and is still latched in RAM");
    unlink("mm_ota_test.symlink_target");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc      = UPGRADE_CHECK_SHA_ERR;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_CHECK_SHA_ERR,
            "a FAILED upgrade reports its own error");
    CHECK(g_ota_success == 0 && access(OTA_SUCCEED_FILE, F_OK) != 0,
            "a FAILED upgrade latches nothing, in RAM or on disk");
    mtd_reset();
    g_feed_rc = UPGRADE_SUCCEED;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED,
            "a retry after a FAILED upgrade is ALLOWED and can succeed");
    CHECK(g_name_calls > 0 && g_erase_calls == 1,
            "...and really re-opens and re-erases the spare partition");
    CHECK(env_is("rootfs" OTA_ENV_SUFFIX, "A") && g_ota_success == 1,
            "...commits the slot switch and only then latches");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_rc   = UPGRADE_SUCCEED;
    g_list_fail = 1;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_PARTITION_ERR,
            "a flashed image whose env store fails is a FAILURE");
    CHECK(g_ota_success == 0 && access(OTA_SUCCEED_FILE, F_OK) != 0,
            "...so it latches nothing and the operator can still retry");
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED &&
                    env_is("rootfs" OTA_ENV_SUFFIX, "A"),
            "...proven: the retry runs and commits");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_begin_only;
    g_feed_rc      = UPGRADE_PROCESS_OK;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_PROCESS_OK,
            "a truncated upload is incomplete, not a success");
    CHECK(g_ota_success == 0 && access(OTA_SUCCEED_FILE, F_OK) != 0,
            "...so it latches nothing either");
    g_feed_hook = feed_one_bin;
    g_feed_rc   = UPGRADE_SUCCEED;
    latch_reset();

    env_reset();
    mtd_reset();
    env_seed("uboot" OTA_ENV_SUFFIX, "B");
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_feed_hook    = feed_two_bins;
    g_feed_rc      = UPGRADE_SUCCEED;
    g_feed_percent = 100;
    g_list_fail    = 1;
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_PARTITION_ERR,
            "a two-bin image whose single env store fails is reported as a FAILURE");
    CHECK(g_list_calls == 1,
            "...after exactly ONE store attempt carrying both keys, so a power cut "
            "has no gap between two stores to land in");
    CHECK(env_is("uboot" OTA_ENV_SUFFIX, "B") && env_is("rootfs" OTA_ENV_SUFFIX, "B"),
            "...and NEITHER key is switched: the environment is whole, the running "
            "image still boots, and no unit is left needing a reflash");
    CHECK(g_reboot_calls == 0, "...arms no reboot");
    CHECK(g_ota_success == 0 && access(OTA_SUCCEED_FILE, F_OK) != 0,
            "...and latches NOTHING: with no half-switched environment to protect, "
            "re-running the upgrade is the correct recovery and is allowed");

    env_reset();
    mtd_reset();
    env_seed("uboot" OTA_ENV_SUFFIX, "B");
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED &&
                    env_is("uboot" OTA_ENV_SUFFIX, "A") && env_is("rootfs" OTA_ENV_SUFFIX, "A"),
            "...proven: the retry runs and switches BOTH keys");

    latch_reset();
    env_reset();
    mtd_reset();
    env_seed("uboot" OTA_ENV_SUFFIX, "B");
    env_seed("rootfs" OTA_ENV_SUFFIX, "B");
    g_reboot_calls = 0;
    CHECK(serve_run("aup", 3, &msg) == 0 && msg.upgrade.error == UPGRADE_SUCCEED,
            "the good path is untouched: a flush that switches ALL queued keys "
            "still succeeds");
    CHECK(env_is("uboot" OTA_ENV_SUFFIX, "A") && env_is("rootfs" OTA_ENV_SUFFIX, "A"),
            "...both queued keys are switched");
    CHECK(g_reboot_calls == 1, "...the reboot is armed, same as a single-key success");
    CHECK(g_ota_success == 1 && access(OTA_SUCCEED_FILE, F_OK) == 0,
            "...and the success latch is set exactly as before");

    g_feed_hook = feed_one_bin;
    g_feed_rc   = UPGRADE_SUCCEED;
    latch_reset();

    CHECK(ota_path_is_tmpfs("/dev/shm/mm_ota_tmpfs_probe") == 1,
            "the latch directory check confirms a real tmpfs mount (/dev/shm)");
    CHECK(ota_path_is_tmpfs("/usr/mm_ota_tmpfs_probe") == 0,
            "...and flags a real non-tmpfs mount (/usr) instead of assuming the best");

    return tst_done();
}
