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

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libuboot.h>

#include "osif.h"
#include "ubootenv.h"
#include "../../tests/include/tst_check.h"

#define KV_MAX  16 /* capacity of a kvset, entries */
#define KEY_MAX 32 /* key buffer size, bytes */
#define VAL_MAX 64 /* value buffer size, bytes */

#define DEFAULTS_PATH "/etc/u-boot-initial-env" /* defaults file libuboot_load_file accepts */
#define LIST_PATH     "/tmp/some-env-list"      /* variable list file libuboot_load_file accepts */

typedef struct {
    char k[KEY_MAX];
    char v[VAL_MAX];
} kv;

typedef struct {
    kv  e[KV_MAX]; /* variables, first n used */
    int n;
} kvset;

/* libubootenv context: the working copy of the environment */
struct uboot_ctx {
    kvset db; /* variables loaded by libuboot_open and edited in place */
};

/* Simulated libubootenv and environment storage */
static struct {
    kvset stored;           /* environment in flash */
    kvset defaults;         /* contents of DEFAULTS_PATH */
    kvset listfile;         /* contents of LIST_PATH */
    kvset written;          /* last environment stored */
    int   defaults_present; /* 0 makes loading DEFAULTS_PATH fail with -EACCES */
    int   listfile_present; /* 0 makes loading LIST_PATH fail with -EACCES */
    int   init_rc;          /* libuboot_initialize return value */
    int   config_rc;        /* libuboot_read_config return value */
    int   open_rc;          /* libuboot_open return value */
    int   store_rc;         /* libuboot_env_store return value */
    int   n_load;           /* libuboot_load_file calls */
    int   n_open;           /* libuboot_open calls */
    int   n_close;          /* libuboot_close calls */
    int   n_exit;           /* libuboot_exit calls */
    int   n_store;          /* libuboot_env_store calls */
    int   lock_held;        /* 1 between libuboot_open and libuboot_close */
    int   lock_conflicts;   /* libuboot_open calls made while already open */
} env;

static int n_err_logs;

static void kv_clear(kvset *s)
{
    s->n = 0;
}

static const char *kv_get(const kvset *s, const char *k)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->e[i].k, k))
            return s->e[i].v;
    return NULL;
}

static void kv_set(kvset *s, const char *k, const char *v)
{
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->e[i].k, k))
            continue;
        if (!v) {
            s->e[i] = s->e[s->n - 1];
            s->n--;
            return;
        }
        snprintf(s->e[i].v, VAL_MAX, "%s", v);
        return;
    }
    if (!v)
        return;
    assert(s->n < KV_MAX);
    snprintf(s->e[s->n].k, KEY_MAX, "%s", k);
    snprintf(s->e[s->n].v, VAL_MAX, "%s", v);
    s->n++;
}

static void env_reset(void)
{
    memset(&env, 0, sizeof env);
    env.defaults_present = 1;
    env.listfile_present = 1;
    kv_set(&env.stored, "linux_partition", "A");
    kv_set(&env.stored, "rootfs_partition", "A");
    kv_set(&env.stored, "HW", "nano3s");
    kv_set(&env.stored, "SW", "0.3.60");
    kv_set(&env.stored, "pool1url", "stratum+tcp://pool.example:3333");
    kv_set(&env.stored, "ipaddr", "10.0.0.7");
    kv_set(&env.defaults, "linux_partition", "A");
    kv_set(&env.defaults, "rootfs_partition", "A");
    kv_set(&env.defaults, "HW", "factory");
    kv_set(&env.defaults, "SW", "0.0.0");
    kv_set(&env.listfile, "SW", "0.4.0");
}

int libuboot_initialize(struct uboot_ctx **out, struct uboot_env_device *envdevs)
{
    (void)envdevs;
    *out = NULL;
    if (env.init_rc)
        return env.init_rc;
    *out = calloc(1, sizeof **out);
    assert(*out);
    return 0;
}

void libuboot_exit(struct uboot_ctx *ctx)
{
    env.n_exit++;
    free(ctx);
}

int libuboot_read_config(struct uboot_ctx *ctx, const char *config)
{
    (void)ctx;
    assert(!strcmp(config, "/etc/fw_env.config"));
    return env.config_rc;
}

int libuboot_open(struct uboot_ctx *ctx)
{
    env.n_open++;
    if (env.lock_held)
        env.lock_conflicts++;
    env.lock_held = 1;
    if (env.open_rc)
        return env.open_rc;
    ctx->db = env.stored;
    return 0;
}

void libuboot_close(struct uboot_ctx *ctx)
{
    env.n_close++;
    env.lock_held = 0;
    if (ctx)
        kv_clear(&ctx->db);
}

int libuboot_load_file(struct uboot_ctx *ctx, const char *filename)
{
    const kvset *src;

    env.n_load++;
    if (!strcmp(filename, DEFAULTS_PATH)) {
        if (!env.defaults_present)
            return -EACCES;
        src = &env.defaults;
    } else {
        assert(!strcmp(filename, LIST_PATH));
        if (!env.listfile_present)
            return -EACCES;
        src = &env.listfile;
    }
    for (int i = 0; i < src->n; i++)
        kv_set(&ctx->db, src->e[i].k, src->e[i].v);
    return 0;
}

int libuboot_env_store(struct uboot_ctx *ctx)
{
    env.n_store++;
    if (env.store_rc)
        return env.store_rc;
    env.written = ctx->db;
    env.stored  = ctx->db;
    return 0;
}

char *libuboot_get_env(struct uboot_ctx *ctx, const char *varname)
{
    const char *v = kv_get(&ctx->db, varname);

    return v ? strdup(v) : NULL;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)fmt;
    if (lvl == OSIF_LOG_ERR)
        n_err_logs++;
}

static int stored_is(const char *k, const char *v)
{
    const char *got = kv_get(&env.stored, k);

    return got && !strcmp(got, v);
}

static int get_is(const char *k, const char *v)
{
    char *got = ubootenv_get(k);
    int   ok  = got && !strcmp(got, v);

    free(got);
    return ok;
}

int main(void)
{
    char *v;

    printf("ubootenv: healthy stored env\n");
    env_reset();
    CHECK(get_is("linux_partition", "A"), "a read returns the stored value");
    CHECK(get_is("pool1url", "stratum+tcp://pool.example:3333"),
            "a runtime-written key reads back");
    v = ubootenv_get("nosuchkey");
    CHECK(v == NULL, "an absent key reads back NULL");
    free(v);
    CHECK(env.n_close == env.n_open && env.lock_held == 0,
            "every opened context is closed again (open %d, close %d)", env.n_open, env.n_close);

    printf("ubootenv: stored env unreadable\n");
    env_reset();
    env.open_rc = -EIO;
    CHECK(get_is("HW", "factory"), "a read falls back to the defaults file");
    CHECK(env.n_store == 0, "a read stores nothing");

    env_reset();
    env.open_rc = -EIO;
    CHECK(ubootenv_apply_list(LIST_PATH) == -EIO,
            "applying a list is REFUSED when the stored env could not be read");
    CHECK(env.n_store == 0, "the refused list never reached the storage (got %d stores)",
            env.n_store);
    CHECK(stored_is("linux_partition", "A") && stored_is("HW", "nano3s") &&
                    stored_is("SW", "0.3.60") && stored_is("ipaddr", "10.0.0.7") &&
                    stored_is("pool1url", "stratum+tcp://pool.example:3333") && env.stored.n == 6,
            "the stored environment is untouched, not replaced by defaults-plus-list");
    CHECK(env.n_close == env.n_open && env.lock_held == 0, "the refused list released its context");

    printf("ubootenv: blank or CRC-invalid stored env\n");
    env_reset();
    env.open_rc = -ENODATA;
    CHECK(ubootenv_apply_list(LIST_PATH) == 0,
            "applying a list against a blank env SUCCEEDS (factory first boot)");
    CHECK(env.n_store == 1, "the first-boot list reached the storage");
    CHECK(stored_is("linux_partition", "A") && stored_is("HW", "factory") &&
                    stored_is("SW", "0.4.0") && env.stored.n == 4,
            "a blank env is seeded with the defaults plus the list");
    CHECK(env.n_close == env.n_open && env.lock_held == 0,
            "the first-boot list released its context");

    env_reset();
    env.open_rc = -ENODATA;
    CHECK(get_is("HW", "factory"), "a read against a blank env returns the default");

    printf("ubootenv: a list the library cannot load is not a successful apply\n");
    env_reset();
    CHECK(ubootenv_apply_list(LIST_PATH) == 0, "applying a healthy list succeeds");
    CHECK(env.n_load == 1 && env.n_store == 1,
            "the list was loaded once and stored once (%d loads, %d stores)", env.n_load,
            env.n_store);
    CHECK(stored_is("SW", "0.4.0"), "the value the list carries reached the storage");
    CHECK(stored_is("HW", "nano3s") && stored_is("ipaddr", "10.0.0.7") && env.stored.n == 6,
            "the apply left every variable the list does not name alone");
    CHECK(env.n_close == env.n_open && env.lock_held == 0, "the apply released its context");

    env_reset();
    env.listfile_present = 0;
    CHECK(ubootenv_apply_list(LIST_PATH) == -EACCES,
            "a missing or malformed list file is propagated verbatim, not reported "
            "to the caller as a successful apply");
    CHECK(env.n_load == 1 && env.n_store == 0,
            "...and the store is skipped: the in-memory env is unchanged, so storing "
            "would erase and rewrite the env partition for nothing (%d loads, %d stores)",
            env.n_load, env.n_store);
    CHECK(stored_is("SW", "0.3.60") && env.stored.n == 6,
            "...and the stored environment keeps every value it had");
    CHECK(env.n_close == env.n_open && env.lock_held == 0, "...and that context was released too");

    printf("ubootenv: failures are reported to the caller\n");
    env_reset();
    env.store_rc = -EIO;
    n_err_logs   = 0;
    CHECK(ubootenv_apply_list(LIST_PATH) != 0,
            "a failed list store returns an error to the caller, not 0");
    CHECK(n_err_logs == 1, "...and logs it once at ERR (got %d)", n_err_logs);

    env_reset();
    env.open_rc          = -EIO;
    env.defaults_present = 0;
    v                    = ubootenv_get("HW");
    CHECK(v == NULL, "with no stored env and no defaults file a read returns NULL");
    free(v);
    CHECK(ubootenv_apply_list(LIST_PATH) != 0,
            "with no stored env and no defaults file applying a list fails");
    CHECK(env.n_store == 0, "and writes nothing");
    CHECK(env.lock_held == 0 && env.lock_conflicts == 0,
            "neither call leaked the environment lock (%d conflicts)", env.lock_conflicts);
    CHECK(env.n_close == env.n_open && env.n_exit == env.n_open,
            "both calls closed and freed their context (open %d, close %d, exit %d)", env.n_open,
            env.n_close, env.n_exit);

    env_reset();
    env.config_rc = -1;
    CHECK(ubootenv_apply_list(LIST_PATH) == -EINVAL,
            "a missing fw_env.config fails applying a list");
    CHECK(ubootenv_get("HW") == NULL, "a missing fw_env.config fails the read");
    CHECK(env.n_store == 0 && env.n_open == 0, "nothing was opened or stored");

    env_reset();
    env.init_rc = -1;
    CHECK(ubootenv_apply_list(LIST_PATH) == -ENODEV,
            "a library init failure fails applying a list");
    CHECK(env.n_store == 0, "nothing was stored");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
