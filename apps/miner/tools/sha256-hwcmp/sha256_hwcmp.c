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

/*
 * sha256_hwcmp: compare a kernel sha256 driver, reached through AF_ALG, with
 * common/sha, in digests and in cost.
 *
 *   sha256_hwcmp [-d DRIVER] [-q] [-c BYTES] [-b COUNT] [-j PROCS] [-t SECONDS] [-n COUNT]
 *                [verify|bench|contend|stress|soak|gap]...
 *
 *   -d  crypto driver to bind (default sha256-kendryte, the K230 hash engine)
 *   -q  quick: small counts, for host tests
 *   -c  coinbase length of the work cases (default 200)
 *   -b  merkle branches of the work cases (default 12)
 *   -j  stress: processes hashing at once (default 4)
 *   -t  stress: seconds each process runs (default 10)
 *   -n  soak: messages per group (default 100000)
 *
 *   verify   every length 0..1024, split sends, accept() clones, large inputs
 *   bench    latency of the miner's sha256d sizes, of building one ASIC work
 *            (mc_merkle_root) and of checking one share, throughput of large
 *            inputs, CPU time per operation
 *   contend  sha256d 64B latency while another process streams 64 KiB
 *            updates, as an OTA does
 *   stress   PROCS processes verify random inputs at the same time
 *   soak     -n multi-block messages each sent whole, split within one
 *            request, and interleaved between two requests; failure counts
 *            per group
 *   gap      -n multi-block messages from a page-aligned buffer sent whole
 *            (one driver call each), then split in two sends at a random
 *            point with 0, 10 us and 1 ms between them; failure counts per
 *            group
 *
 * Without a mode it runs verify and bench. One send() without MSG_MORE is one
 * crypto_ahash_digest() in the kernel; the miner would hash that way.
 *
 * Exit status: 0 every digest matched, 1 a mismatch or an AF_ALG error,
 * 77 DRIVER cannot be bound.
 */
#define _GNU_SOURCE
#include "sha256.h"

#include <errno.h>
#include <signal.h>
#include <linux/if_alg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_ALG
#define AF_ALG 38
#endif

#define EXIT_UNAVAILABLE 77
#define SPLIT_MAX        70000           /* bytes, past the 16 KiB bounce and 64 KiB send chunk */
#define COINBASE_MAX     (6 * 1024 + 64) /* bytes, MC_COINBASE_MAX */
#define BRANCHES_MAX     30              /* MC_MERKLES_MAX */
#define BULK_CHUNK       (64 << 10)      /* bytes per update of the contend load */
#define BULK_MSG         (16 << 20)      /* bytes per message of the contend load */
#define SOAK_LENS        256             /* distinct soak message lengths */
#define SOAK_MIN         (16 * 1024 + 1) /* bytes; two or more engine blocks */

typedef struct {
    int tfm; /* bound transform socket */
    int op;  /* operation socket accepted from tfm */
} hw_ctx;

enum bench_op { OP_SHA256D, OP_WORK, OP_SHARE, OP_SHA256 };

static const char *g_driver = "sha256-kendryte";
static int         g_quick;
static size_t      g_coinbase = 200; /* bytes */
static int         g_branches = 12;

static uint64_t rng_next(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void rng_fill(uint64_t *s, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)rng_next(s);
}

static int hw_open(hw_ctx *h)
{
    struct sockaddr_alg sa = { .salg_family = AF_ALG, .salg_type = "hash" };

    snprintf((char *)sa.salg_name, sizeof sa.salg_name, "%s", g_driver);
    h->tfm = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (h->tfm < 0)
        return -errno;
    if (bind(h->tfm, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int err = -errno;
        close(h->tfm);
        return err;
    }
    h->op = accept4(h->tfm, NULL, NULL, SOCK_CLOEXEC);
    if (h->op < 0) {
        int err = -errno;
        close(h->tfm);
        return err;
    }
    return 0;
}

static void hw_close(hw_ctx *h)
{
    close(h->op);
    close(h->tfm);
}

static int hw_send(int fd, const void *p, size_t n, int flags)
{
    ssize_t r = send(fd, p, n, flags);

    if (r != (ssize_t)n) {
        fprintf(stderr, "send %zu bytes: %s\n", n, r < 0 ? strerror(errno) : "short");
        return -1;
    }
    return 0;
}

static int hw_read(int fd, uint8_t out[SHA256_DIGEST_SIZE])
{
    ssize_t r = read(fd, out, SHA256_DIGEST_SIZE);

    if (r != SHA256_DIGEST_SIZE) {
        fprintf(stderr, "read digest: %s\n", r < 0 ? strerror(errno) : "short");
        return -1;
    }
    return 0;
}

static int hw_sha256(hw_ctx *h, const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (len && hw_send(h->op, msg, len, 0))
        return -1;
    return hw_read(h->op, out);
}

static int hw_sha256d(hw_ctx *h, const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t first[SHA256_DIGEST_SIZE];

    if (hw_sha256(h, msg, len, first))
        return -1;
    return hw_sha256(h, first, sizeof first, out);
}

static int hw_sha256_split(hw_ctx *h, uint64_t *rng, const uint8_t *msg, size_t len,
        uint8_t out[SHA256_DIGEST_SIZE])
{
    size_t off    = 0;
    int    pieces = 1 + (int)(rng_next(rng) % 4);

    for (int i = 0; i < pieces - 1 && off < len; i++) {
        size_t n = (size_t)(rng_next(rng) % (len - off + 1));
        if (hw_send(h->op, msg + off, n, MSG_MORE))
            return -1;
        off += n;
    }
    if (rng_next(rng) & 1) {
        if (hw_send(h->op, msg + off, len - off, MSG_MORE))
            return -1;
    } else if (len - off && hw_send(h->op, msg + off, len - off, 0)) {
        return -1;
    }
    return hw_read(h->op, out);
}

static int hw_sha256_clone(hw_ctx *h, size_t cut, const uint8_t *msg, size_t len,
        uint8_t a[SHA256_DIGEST_SIZE], uint8_t b[SHA256_DIGEST_SIZE])
{
    int clone, ret = -1;

    if (hw_send(h->op, msg, cut, MSG_MORE))
        return -1;
    clone = accept4(h->op, NULL, NULL, SOCK_CLOEXEC);
    if (clone < 0) {
        fprintf(stderr, "accept clone: %s\n", strerror(errno));
        hw_read(h->op, a);
        return -1;
    }
    if (!hw_send(clone, msg + cut, len - cut, MSG_MORE) && !hw_read(clone, b) &&
            !hw_send(h->op, msg + cut, len - cut, MSG_MORE) && !hw_read(h->op, a))
        ret = 0;
    close(clone);
    return ret;
}

static int any_sha256d(hw_ctx *h, const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (h)
        return hw_sha256d(h, msg, len, out);
    sha256d(msg, len, out);
    return 0;
}

static int merkle_root(hw_ctx *h, const uint8_t *buf, uint8_t root[SHA256_DIGEST_SIZE])
{
    uint8_t cat[2 * SHA256_DIGEST_SIZE];

    if (any_sha256d(h, buf, g_coinbase, cat))
        return -1;
    for (int i = 0; i < g_branches; i++) {
        memcpy(cat + SHA256_DIGEST_SIZE, buf + g_coinbase + (size_t)i * SHA256_DIGEST_SIZE,
                SHA256_DIGEST_SIZE);
        if (any_sha256d(h, cat, sizeof cat, cat))
            return -1;
    }
    memcpy(root, cat, SHA256_DIGEST_SIZE);
    return 0;
}

static int hw_sha256_interleave(hw_ctx *a, hw_ctx *b, uint64_t *rng, size_t max_piece,
        const uint8_t *ma, size_t la, const uint8_t *mb, size_t lb, uint8_t da[SHA256_DIGEST_SIZE],
        uint8_t db[SHA256_DIGEST_SIZE])
{
    size_t oa = 0, ob = 0;

    while (oa < la || ob < lb) {
        size_t n = 1 + (size_t)(rng_next(rng) % max_piece);
        if (oa < la) {
            size_t k = n < la - oa ? n : la - oa;
            if (hw_send(a->op, ma + oa, k, MSG_MORE))
                return -1;
            oa += k;
        }
        if (ob < lb) {
            size_t k = n < lb - ob ? n : lb - ob;
            if (hw_send(b->op, mb + ob, k, MSG_MORE))
                return -1;
            ob += k;
        }
    }
    if (la && hw_read(a->op, da))
        return -1;
    if (!la && hw_sha256(a, NULL, 0, da))
        return -1;
    if (lb && hw_read(b->op, db))
        return -1;
    if (!lb && hw_sha256(b, NULL, 0, db))
        return -1;
    return 0;
}

static int check(const char *what, size_t len, const uint8_t *hw, const uint8_t *sw)
{
    if (!memcmp(hw, sw, SHA256_DIGEST_SIZE))
        return 0;
    fprintf(stderr, "MISMATCH %s len=%zu\n", what, len);
    return 1;
}

static int verify(void)
{
    const size_t big[] = { 16383, 16384, 16385, 65535, 65536, 65537, 1 << 20, (1 << 20) + 13,
        16 << 20 };
    uint8_t      hw[SHA256_DIGEST_SIZE], sw[SHA256_DIGEST_SIZE], hw2[SHA256_DIGEST_SIZE];
    size_t       nbig   = g_quick ? 7 : sizeof big / sizeof big[0];
    int          splits = g_quick ? 200 : 2000, clones = g_quick ? 50 : 500;
    uint64_t     rng   = 0x5eed5eed12345678ull;
    uint8_t     *buf   = malloc((16 << 20) + 8);
    int          fails = 0, ops = 0;
    hw_ctx       h;

    if (!buf || hw_open(&h)) {
        fprintf(stderr, "verify: setup failed\n");
        free(buf);
        return 1;
    }
    rng_fill(&rng, buf, (16 << 20) + 8);

    for (size_t len = 0; len <= 1024; len++) {
        const uint8_t *m = buf + len % 8; /* vary the alignment too */
        if (hw_sha256(&h, m, len, hw))
            goto io;
        sha256(m, len, sw);
        fails += check("sha256", len, hw, sw);
        if (hw_sha256d(&h, m, len, hw))
            goto io;
        sha256d(m, len, sw);
        fails += check("sha256d", len, hw, sw);
        ops += 2;
    }
    for (int i = 0; i < splits; i++) {
        size_t len = (size_t)(rng_next(&rng) % (SPLIT_MAX + 1));
        if (hw_sha256_split(&h, &rng, buf, len, hw))
            goto io;
        sha256(buf, len, sw);
        fails += check("split", len, hw, sw);
        ops++;
    }
    for (int i = 0; i < clones; i++) {
        size_t len = (size_t)(rng_next(&rng) % (SPLIT_MAX + 1));
        size_t cut = len ? (size_t)(rng_next(&rng) % len) : 0;
        if (hw_sha256_clone(&h, cut, buf, len, hw, hw2))
            goto io;
        sha256(buf, len, sw);
        fails += check("clone original", len, hw, sw);
        fails += check("clone copy", len, hw2, sw);
        ops++;
    }
    /* two requests sharing the engine, small and large pieces */
    {
        static const size_t pieces[] = { 200, 4096, 20000, 70000 };
        hw_ctx              h2;
        if (hw_open(&h2))
            goto io;
        for (int i = 0; i < splits; i++) {
            size_t         la = (size_t)(rng_next(&rng) % (SPLIT_MAX + 1));
            size_t         lb = (size_t)(rng_next(&rng) % (SPLIT_MAX + 1));
            size_t         mp = pieces[i % 4];
            const uint8_t *mb = buf + (8 << 20);
            int            r  = hw_sha256_interleave(&h, &h2, &rng, mp, buf, la, mb, lb, hw, hw2);
            if (r) {
                hw_close(&h2);
                goto io;
            }
            sha256(buf, la, sw);
            fails += check("interleave a", la, hw, sw);
            sha256(mb, lb, sw);
            fails += check("interleave b", lb, hw2, sw);
            ops++;
        }
        hw_close(&h2);
    }
    for (int br = 0; br <= BRANCHES_MAX; br++) {
        size_t saved_cb = g_coinbase;
        int    saved_br = g_branches;
        g_coinbase      = 1 + (size_t)(rng_next(&rng) % COINBASE_MAX);
        g_branches      = br;
        int r           = merkle_root(&h, buf, hw);
        merkle_root(NULL, buf, sw);
        fails += r ? 0 : check("merkle root", g_coinbase, hw, sw);
        g_coinbase = saved_cb;
        g_branches = saved_br;
        if (r)
            goto io;
        ops++;
    }
    for (size_t i = 0; i < nbig; i++) {
        if (hw_sha256(&h, buf + 3, big[i], hw))
            goto io;
        sha256(buf + 3, big[i], sw);
        fails += check("large", big[i], hw, sw);
        ops++;
    }

    printf("verify: %d operations, %d mismatches\n", ops, fails);
    hw_close(&h);
    free(buf);
    return fails ? 1 : 0;
io:
    hw_close(&h);
    free(buf);
    return 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t cpu_ns(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec) * 1000000000u +
            ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec) * 1000u;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int run_op(hw_ctx *h, enum bench_op op, const uint8_t *buf, size_t len)
{
    uint8_t d[SHA256_DIGEST_SIZE];

    switch (op) {
    case OP_SHA256D:
        return any_sha256d(h, buf, len, d);
    case OP_SHA256:
        if (h)
            return hw_sha256(h, buf, len, d);
        sha256(buf, len, d);
        return 0;
    case OP_WORK: /* asic_miner, per work sent */
        return merkle_root(h, buf, d);
    case OP_SHARE: /* core_miner, per nonce returned */
        return merkle_root(h, buf, d) || any_sha256d(h, buf, 80, d);
    }
    return -1;
}

static int bench_case(hw_ctx *h, const char *name, enum bench_op op, const uint8_t *buf, size_t len,
        int n, double *cpu_op)
{
    uint64_t *lat = malloc((size_t)n * sizeof *lat);
    uint64_t  t0, c0, wall, cpu;

    if (!lat)
        return -1;
    run_op(h, op, buf, len); /* warm caches and the socket */
    c0 = cpu_ns();
    t0 = now_ns();
    for (int i = 0; i < n; i++) {
        uint64_t s = now_ns();
        if (run_op(h, op, buf, len)) {
            free(lat);
            return -1;
        }
        lat[i] = now_ns() - s;
    }
    wall = now_ns() - t0;
    cpu  = cpu_ns() - c0;
    qsort(lat, (size_t)n, sizeof *lat, cmp_u64);

    *cpu_op = (double)cpu / n;
    printf("%-16s %-3s %7d %10.2f %10.2f %10.2f", name, h ? "hw" : "sw", n, lat[n / 2] / 1e3,
            lat[n - 1 - n / 100] / 1e3, *cpu_op / 1e3);
    if (op == OP_SHA256)
        printf(" %9.2f", (double)len * n / (wall / 1e9) / (1 << 20));
    printf("\n");
    free(lat);
    return 0;
}

static int bench(void)
{
    static const struct {
        const char   *name;
        enum bench_op op;
        size_t        len;
    } cases[] = {
        { "sha256d 64B", OP_SHA256D, 64 },
        { "sha256d 80B", OP_SHA256D, 80 },
        { "sha256d 200B", OP_SHA256D, 200 },
        { "work build", OP_WORK, 0 },
        { "share check", OP_SHARE, 0 },
        { "sha256 4KiB", OP_SHA256, 4 << 10 },
        { "sha256 64KiB", OP_SHA256, 64 << 10 },
        { "sha256 1MiB", OP_SHA256, 1 << 20 },
        { "sha256 16MiB", OP_SHA256, 16 << 20 },
    };
    size_t   total = g_quick ? 1 << 20 : 64 << 20; /* bytes per throughput case */
    uint64_t rng   = 0x0123456789abcdefull;
    uint8_t *buf   = malloc(16 << 20);
    hw_ctx   h;
    int      ret = 0;

    if (!buf || hw_open(&h)) {
        fprintf(stderr, "bench: setup failed\n");
        free(buf);
        return 1;
    }
    rng_fill(&rng, buf, 16 << 20);

    printf("work: coinbase %zu B, %d branches\n", g_coinbase, g_branches);
    printf("%-16s %-3s %7s %10s %10s %10s %9s\n", "case", "imp", "ops", "med_us", "p99_us",
            "cpu_us/op", "MiB/s");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int    n = cases[i].op == OP_SHA256 ? (int)(total / cases[i].len) : (g_quick ? 200 : 20000);
        double sw_cpu, hw_cpu;
        if (n < 3)
            n = 3;
        if (bench_case(NULL, cases[i].name, cases[i].op, buf, cases[i].len, n, &sw_cpu) ||
                bench_case(&h, cases[i].name, cases[i].op, buf, cases[i].len, n, &hw_cpu)) {
            fprintf(stderr, "bench %s: failed\n", cases[i].name);
            ret = 1;
            break;
        }
        if (cases[i].op == OP_WORK)
            printf("%-16s works per CPU second: sw %.0f, hw %.0f\n", "", 1e9 / sw_cpu,
                    1e9 / hw_cpu);
    }
    hw_close(&h);
    free(buf);
    return ret;
}

static void bulk_load(int use_hw, const uint8_t *buf)
{
    uint8_t d[SHA256_DIGEST_SIZE];
    hw_ctx  h;

    if (use_hw && hw_open(&h))
        _exit(1);
    for (;;) {
        if (use_hw) {
            for (size_t off = 0; off < BULK_MSG; off += BULK_CHUNK)
                if (hw_send(h.op, buf + off, BULK_CHUNK, MSG_MORE))
                    _exit(1);
            if (hw_read(h.op, d))
                _exit(1);
        } else {
            sha256_ctx c;
            sha256_init(&c);
            for (size_t off = 0; off < BULK_MSG; off += BULK_CHUNK)
                sha256_update(&c, buf + off, BULK_CHUNK);
            sha256_final(&c, d);
        }
    }
}

static int contend(void)
{
    int      n = g_quick ? 200 : 20000, ret = 0;
    uint64_t rng = 0x00c0ffee00c0ffeeull;
    uint8_t *buf = malloc(BULK_MSG);
    hw_ctx   h;

    if (!buf || hw_open(&h)) {
        fprintf(stderr, "contend: setup failed\n");
        free(buf);
        return 1;
    }
    rng_fill(&rng, buf, BULK_MSG);
    printf("%-16s %-3s %7s %10s %10s %10s\n", "case", "imp", "ops", "med_us", "p99_us", "max_us");

    for (int use_hw = 0; use_hw <= 1 && !ret; use_hw++) {
        uint64_t *lat = malloc((size_t)n * sizeof *lat);
        pid_t     pid;

        fflush(stdout);
        pid = lat ? fork() : -1;
        if (pid < 0) {
            perror("contend");
            free(lat);
            ret = 1;
            break;
        }
        if (pid == 0)
            bulk_load(use_hw, buf);
        usleep(200000); /* let the load reach steady state */

        for (int i = 0; i < n && !ret; i++) {
            uint64_t s = now_ns();
            ret        = run_op(use_hw ? &h : NULL, OP_SHA256D, buf, 64);
            lat[i]     = now_ns() - s;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        if (!ret) {
            qsort(lat, (size_t)n, sizeof *lat, cmp_u64);
            printf("%-16s %-3s %7d %10.2f %10.2f %10.2f\n", "sha256d 64B+bulk",
                    use_hw ? "hw" : "sw", n, lat[n / 2] / 1e3, lat[n - 1 - n / 100] / 1e3,
                    lat[n - 1] / 1e3);
        }
        free(lat);
    }
    hw_close(&h);
    free(buf);
    return ret ? 1 : 0;
}

static int soak(long n)
{
    static size_t            lens[SOAK_LENS];
    static uint8_t           want[SOAK_LENS][SHA256_DIGEST_SIZE];
    static const char *const group[] = { "whole", "split", "interleave" };
    uint8_t                  hw[SHA256_DIGEST_SIZE], hw2[SHA256_DIGEST_SIZE];
    uint64_t                 rng = 0x50a650a650a650a6ull;
    uint8_t                 *buf = malloc(SPLIT_MAX);
    hw_ctx                   h, h2;
    int                      ret = 0;

    if (!buf || hw_open(&h) || hw_open(&h2)) {
        fprintf(stderr, "soak: setup failed\n");
        free(buf);
        return 1;
    }
    rng_fill(&rng, buf, SPLIT_MAX);
    for (int i = 0; i < SOAK_LENS; i++) {
        lens[i] = SOAK_MIN + (size_t)(rng_next(&rng) % (SPLIT_MAX - SOAK_MIN + 1));
        sha256(buf, lens[i], want[i]);
    }

    for (int g = 0; g < 3; g++) {
        long     fails = 0;
        uint64_t t0    = now_ns();
        for (long i = 0; i < n; i++) {
            int a = (int)(rng_next(&rng) % SOAK_LENS), b = (int)(rng_next(&rng) % SOAK_LENS);
            /* clang-format off */
            int r = g == 0 ? hw_sha256(&h, buf, lens[a], hw)
                    : g == 1 ? hw_sha256_split(&h, &rng, buf, lens[a], hw)
                    : hw_sha256_interleave(&h, &h2, &rng, 20000, buf, lens[a], buf, lens[b],
                            hw, hw2);
            /* clang-format on */
            if (r) {
                ret = 1;
                break;
            }
            if (memcmp(hw, want[a], SHA256_DIGEST_SIZE)) {
                fails++;
                fprintf(stderr, "MISMATCH soak %s len=%zu\n", group[g], lens[a]);
            }
            if (g == 2 && memcmp(hw2, want[b], SHA256_DIGEST_SIZE)) {
                fails++;
                fprintf(stderr, "MISMATCH soak %s len=%zu\n", group[g], lens[b]);
            }
        }
        printf("soak %-10s %ld messages%s, %ld mismatches, %.1f s\n", group[g], g == 2 ? 2 * n : n,
                g == 2 ? " (pairs)" : "", fails, (now_ns() - t0) / 1e9);
        fflush(stdout);
        if (fails)
            ret = 1;
    }
    hw_close(&h2);
    hw_close(&h);
    free(buf);
    return ret;
}

static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) && errno == EINTR)
        ;
}

static int gap(long n)
{
    static size_t     lens[SOAK_LENS];
    static uint8_t    want[SOAK_LENS][SHA256_DIGEST_SIZE];
    static const long gaps_us[] = { -1, 0, 10, 1000 }; /* -1: one send */
    uint8_t           hw[SHA256_DIGEST_SIZE];
    uint64_t          rng = 0x6a906a906a906a90ull;
    uint8_t          *buf = NULL;
    hw_ctx            h;
    int               ret = 0;

    if (posix_memalign((void **)&buf, 4096, 64 << 10) || hw_open(&h)) {
        fprintf(stderr, "gap: setup failed\n");
        free(buf);
        return 1;
    }
    rng_fill(&rng, buf, 64 << 10);
    for (int i = 0; i < SOAK_LENS; i++) {
        lens[i] = SOAK_MIN + (size_t)(rng_next(&rng) % ((64 << 10) - SOAK_MIN + 1));
        sha256(buf, lens[i], want[i]);
    }

    for (size_t g = 0; g < sizeof gaps_us / sizeof gaps_us[0]; g++) {
        long     fails = 0;
        uint64_t t0    = now_ns();
        for (long i = 0; i < n; i++) {
            int    a   = (int)(rng_next(&rng) % SOAK_LENS);
            size_t len = lens[a], cut = 1 + (size_t)(rng_next(&rng) % (len - 1));
            int    r;
            if (gaps_us[g] < 0) {
                r = hw_sha256(&h, buf, len, hw);
            } else {
                r = hw_send(h.op, buf, cut, MSG_MORE);
                if (!r && gaps_us[g])
                    sleep_us(gaps_us[g]);
                if (!r)
                    r = hw_sha256(&h, buf + cut, len - cut, hw);
            }
            if (r) {
                hw_close(&h);
                free(buf);
                return 1;
            }
            if (memcmp(hw, want[a], SHA256_DIGEST_SIZE)) {
                fails++;
                fprintf(stderr, "MISMATCH gap %ld us len=%zu cut=%zu\n", gaps_us[g], len,
                        gaps_us[g] < 0 ? len : cut);
            }
        }
        if (gaps_us[g] < 0)
            printf("gap one send     %ld messages, %ld mismatches, %.1f s\n", n, fails,
                    (now_ns() - t0) / 1e9);
        else
            printf("gap two sends %4ld us %ld messages, %ld mismatches, %.1f s\n", gaps_us[g], n,
                    fails, (now_ns() - t0) / 1e9);
        fflush(stdout);
        if (fails)
            ret = 1;
    }
    hw_close(&h);
    free(buf);
    return ret;
}

static int stress_child(int id, int seconds)
{
    uint64_t rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)getpid() << 20) ^ (uint64_t)id;
    uint64_t end = now_ns() + (uint64_t)seconds * 1000000000u;
    uint8_t  hw[SHA256_DIGEST_SIZE], sw[SHA256_DIGEST_SIZE];
    uint8_t *buf = malloc(SPLIT_MAX);
    long     ops = 0, fails = 0;
    hw_ctx   h;

    if (!buf || hw_open(&h))
        return 1;
    rng_fill(&rng, buf, SPLIT_MAX);
    while (now_ns() < end) {
        size_t len = (size_t)(rng_next(&rng) % (SPLIT_MAX + 1));
        int    r   = (rng_next(&rng) & 1) ? hw_sha256_split(&h, &rng, buf, len, hw) :
                                            hw_sha256(&h, buf, len, hw);
        if (r) {
            fails++;
            break;
        }
        sha256(buf, len, sw);
        fails += check("stress", len, hw, sw);
        ops++;
    }
    printf("stress[%d]: %ld operations, %ld mismatches\n", id, ops, fails);
    fflush(stdout);
    hw_close(&h);
    free(buf);
    return fails ? 1 : 0;
}

static int stress(int procs, int seconds)
{
    int fails = 0;

    fflush(stdout);
    for (int i = 0; i < procs; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            fails++;
            break;
        }
        if (pid == 0)
            _exit(stress_child(i, seconds));
    }
    for (;;) {
        int st;
        if (wait(&st) < 0)
            break;
        if (!WIFEXITED(st) || WEXITSTATUS(st))
            fails++;
    }
    printf("stress: %d processes x %d s, %d failed\n", procs, seconds, fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    int    procs = 4, seconds = 10, opt, ret = 0, ran = 0;
    long   soak_n = 0;
    hw_ctx probe;

    while ((opt = getopt(argc, argv, "d:qc:b:j:t:n:")) != -1) {
        switch (opt) {
        case 'd':
            g_driver = optarg;
            break;
        case 'q':
            g_quick = 1;
            break;
        case 'c':
            g_coinbase = (size_t)atoi(optarg);
            break;
        case 'b':
            g_branches = atoi(optarg);
            break;
        case 'j':
            procs = atoi(optarg);
            break;
        case 't':
            seconds = atoi(optarg);
            break;
        case 'n':
            soak_n = atol(optarg);
            break;
        default:
            fprintf(stderr,
                    "usage: %s [-d DRIVER] [-q] [-c BYTES] [-b COUNT] [-j PROCS] "
                    "[-t SECONDS] [-n COUNT] [verify|bench|contend|stress|soak|gap]...\n",
                    argv[0]);
            return 1;
        }
    }
    if (procs < 1 || seconds < 1) {
        fprintf(stderr, "-j and -t take a positive number\n");
        return 1;
    }
    if (g_coinbase < 1 || g_coinbase > COINBASE_MAX || g_branches < 0 ||
            g_branches > BRANCHES_MAX) {
        fprintf(stderr, "-c takes 1..%d bytes, -b 0..%d branches\n", COINBASE_MAX, BRANCHES_MAX);
        return 1;
    }

    opt = hw_open(&probe);
    if (opt) {
        fprintf(stderr, "%s: cannot bind AF_ALG hash: %s\n", g_driver, strerror(-opt));
        return EXIT_UNAVAILABLE;
    }
    hw_close(&probe);
    printf("driver %s\n", g_driver);

    for (int i = optind; i < argc; i++) {
        if (!strcmp(argv[i], "verify"))
            ret |= verify();
        else if (!strcmp(argv[i], "bench"))
            ret |= bench();
        else if (!strcmp(argv[i], "contend"))
            ret |= contend();
        else if (!strcmp(argv[i], "soak"))
            ret |= soak(soak_n > 0 ? soak_n : g_quick ? 200 : 100000);
        else if (!strcmp(argv[i], "gap"))
            ret |= gap(soak_n > 0 ? soak_n : g_quick ? 200 : 100000);
        else if (!strcmp(argv[i], "stress"))
            ret |= stress(procs, seconds);
        else {
            fprintf(stderr, "unknown mode %s\n", argv[i]);
            return 1;
        }
        ran = 1;
    }
    if (!ran)
        ret = verify() | bench();
    return ret;
}
