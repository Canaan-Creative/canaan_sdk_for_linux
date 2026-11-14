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
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <mtd/mtd-user.h>

#include "mtd_dev.h"
#include "osif.h"
#include "../../tests/include/tst_check.h"

#define NAND_MAX_EB  64       /* capacity of the simulated device, erase blocks */
#define NAND_MAX_IMG 262144   /* capacity of the simulated device image, bytes */
#define NAND_LOG_MAX 2048     /* capacity of each operation log */
#define NAND_PART    "boot_a" /* partition name listed in /proc/mtd */

#define FD_BASE  1000 /* first descriptor the fake open returns */
#define FD_SLOTS 16   /* capacity of the descriptor table */

#define MUST(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: setup %s:%d\n", __FILE__, __LINE__); \
            fails++; \
        } \
    } while (0)

typedef struct {
    int eb;   /* erase block index */
    int offs; /* byte offset inside the erase block */
    int len;  /* length, bytes */
} wr_rec;

enum { KIND_PROC = 1, KIND_MTD }; /* kind of a fake descriptor: /proc/mtd or /dev/mtdN */

/* Simulated NAND MTD device */
static struct {
    int           min_io;  /* page size, bytes */
    int           eb_size; /* erase block size, bytes */
    int           eb_cnt;
    long long     size;                     /* device size, bytes */
    unsigned char img[NAND_MAX_IMG];        /* device contents, 0xff when erased */
    int           factory_bad[NAND_MAX_EB]; /* 1 for a factory bad erase block */
    int           marked_bad[NAND_MAX_EB];  /* 1 for an erase block marked bad by MEMSETBADBLOCK */
    int           eio_after[NAND_MAX_EB];   /* writes that succeed after an erase, -1 = all */
    int           attempts[NAND_MAX_EB];    /* write attempts since the last erase */
    int           fail_errno;               /* errno of a failed write */
    int           n_erase;                  /* MEMERASE calls */
    int           n_write;                  /* successful page writes */
    int           n_markbad;                /* MEMSETBADBLOCK calls */
    int           n_isbad;                  /* in-range MEMGETBADBLOCK calls */
    int           n_isbad_oob;              /* out-of-range MEMGETBADBLOCK calls */
    int           n_open;                   /* read-write opens of /dev/mtdN */
    int           erase_eb[NAND_LOG_MAX];   /* erase block of each MEMERASE call */
    long long     erase_off[NAND_LOG_MAX];  /* byte offset of each MEMERASE call */
    int           markbad_eb[NAND_LOG_MAX]; /* erase block of each MEMSETBADBLOCK call */
    wr_rec        wr[NAND_LOG_MAX];         /* each successful page write */
} nand;

static int g_shape_bad; /* writes not exactly one aligned page */
/* opens of /proc/mtd or /dev/mtdN without O_CLOEXEC, across every scenario */
static int g_open_inherit;
static int n_err_toomany; /* ERR logs reporting too many damaged blocks */
static int n_err_nocheck; /* ERR logs reporting a block that cannot be checked */

/* Open fake descriptors, indexed by fd - FD_BASE */
static struct {
    int    used;   /* 1 while the slot is open */
    int    kind;   /* KIND_PROC or KIND_MTD */
    int    mtdnum; /* N of /dev/mtdN */
    size_t pos;    /* read offset into g_proc, bytes */
} fdtab[FD_SLOTS];

static char g_proc[256]; /* /proc/mtd contents served by read */

static int n_warn_logs;

static unsigned char payload[32768];         /* test payload, never 0xff */
static unsigned char base_img[NAND_MAX_IMG]; /* device image of the baseline write */
static wr_rec        base_wr[NAND_LOG_MAX];  /* write log of the baseline write */
static int           base_nw;                /* page writes of the baseline write */

/* chunk sizes fed in turn, bytes */
static const size_t k_irregular[] = { 1, 7, 13, 3, 257, 1021, 2, 509, 31, 4093 };

static void nand_reset(int min_io, int eb_size, int eb_cnt)
{
    assert(eb_cnt <= NAND_MAX_EB);
    assert((long long)eb_size * eb_cnt <= NAND_MAX_IMG);
    memset(&nand, 0, sizeof nand);
    nand.min_io  = min_io;
    nand.eb_size = eb_size;
    nand.eb_cnt  = eb_cnt;
    nand.size    = (long long)eb_size * eb_cnt;
    memset(nand.img, 0xff, (size_t)nand.size);
    for (int i = 0; i < NAND_MAX_EB; i++)
        nand.eio_after[i] = -1;
    nand.fail_errno = EIO;
}

static void nand_bad(int eb)
{
    nand.factory_bad[eb] = 1;
}

static void nand_fail(int eb, int after)
{
    nand.eio_after[eb] = after;
}

static int nand_blank(long long from, long long to)
{
    for (long long i = from; i < to; i++)
        if (nand.img[i] != 0xff)
            return 0;
    return 1;
}

static int slot_alloc(void)
{
    for (int i = 0; i < FD_SLOTS; i++)
        if (!fdtab[i].used) {
            memset(&fdtab[i], 0, sizeof fdtab[i]);
            fdtab[i].used = 1;
            return i;
        }
    return -1;
}

static int slot_of(int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + FD_SLOTS)
        return -1;
    return fdtab[fd - FD_BASE].used ? fd - FD_BASE : -1;
}

int open(const char *path, int flags, ...)
{
    int s;

    if (!(flags & O_CLOEXEC))
        g_open_inherit++;
    if (strcmp(path, "/proc/mtd") == 0) {
        if ((s = slot_alloc()) < 0) {
            errno = EMFILE;
            return -1;
        }
        fdtab[s].kind = KIND_PROC;
        snprintf(g_proc, sizeof g_proc,
                "dev:    size   erasesize  name\n"
                "mtd0: %08llx %08x \"%s\"\n",
                (unsigned long long)nand.size, (unsigned)nand.eb_size, NAND_PART);
        return FD_BASE + s;
    }

    if (strncmp(path, "/dev/mtd", 8) == 0) {
        if ((s = slot_alloc()) < 0) {
            errno = EMFILE;
            return -1;
        }
        fdtab[s].kind   = KIND_MTD;
        fdtab[s].mtdnum = atoi(path + 8);
        if ((flags & O_ACCMODE) == O_RDWR)
            nand.n_open++;
        return FD_BASE + s;
    }

    errno = ENOENT;
    return -1;
}

int close(int fd)
{
    int s = slot_of(fd);

    if (s >= 0)
        fdtab[s].used = 0;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    int    s = slot_of(fd);
    size_t left, n;

    if (s < 0 || fdtab[s].kind != KIND_PROC) {
        errno = EBADF;
        return -1;
    }
    left = strlen(g_proc) - fdtab[s].pos;
    n    = count < left ? count : left;
    memcpy(buf, g_proc + fdtab[s].pos, n);
    fdtab[s].pos += n;
    return (ssize_t)n;
}

int ioctl(int fd, unsigned long req, ...)
{
    int     s = slot_of(fd);
    va_list ap;
    void   *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (s < 0 || fdtab[s].kind != KIND_MTD) {
        errno = ENOTTY;
        return -1;
    }
    if (fdtab[s].mtdnum != 0) {
        errno = ENODEV;
        return -1;
    }

    switch (req) {
    case MEMGETINFO: {
        struct mtd_info_user *mi = arg;

        memset(mi, 0, sizeof *mi);
        mi->type      = MTD_NANDFLASH;
        mi->size      = (uint32_t)nand.size;
        mi->erasesize = (uint32_t)nand.eb_size;
        mi->writesize = (uint32_t)nand.min_io;
        return 0;
    }
    case MEMGETBADBLOCK: {
        int eb = (int)(*(long long *)arg / nand.eb_size);

        if (eb < 0 || eb >= nand.eb_cnt) {
            nand.n_isbad_oob++;
            errno = EINVAL;
            return -1;
        }
        nand.n_isbad++;
        return (nand.factory_bad[eb] || nand.marked_bad[eb]) ? 1 : 0;
    }
    case MEMSETBADBLOCK: {
        int eb = (int)(*(long long *)arg / nand.eb_size);

        if (eb < 0 || eb >= nand.eb_cnt) {
            errno = EINVAL;
            return -1;
        }
        nand.marked_bad[eb] = 1;
        if (nand.n_markbad < NAND_LOG_MAX)
            nand.markbad_eb[nand.n_markbad] = eb;
        nand.n_markbad++;
        return 0;
    }
    case MEMERASE: {
        struct erase_info_user *e  = arg;
        int                     eb = (int)(e->start / nand.eb_size);

        if (eb < 0 || eb >= nand.eb_cnt) {
            errno = EINVAL;
            return -1;
        }
        if (nand.n_erase < NAND_LOG_MAX) {
            nand.erase_eb[nand.n_erase]  = eb;
            nand.erase_off[nand.n_erase] = (long long)eb * nand.eb_size;
        }
        nand.n_erase++;
        memset(nand.img + (long long)eb * nand.eb_size, 0xff, (size_t)nand.eb_size);
        nand.attempts[eb] = 0;
        return 0;
    }
    case MEMISLOCKED:
        return 0;
    case MEMUNLOCK:
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}

ssize_t pread(int fd, void *buf, size_t count, off_t off)
{
    int s = slot_of(fd);

    if (s < 0 || fdtab[s].kind != KIND_MTD) {
        errno = EBADF;
        return -1;
    }
    if (off < 0 || (long long)off + (long long)count > nand.size) {
        errno = EINVAL;
        return -1;
    }
    memcpy(buf, nand.img + off, count);
    return (ssize_t)count;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t off)
{
    int s    = slot_of(fd);
    int eb   = (int)(off / nand.eb_size);
    int offs = (int)(off % nand.eb_size);

    if (s < 0 || fdtab[s].kind != KIND_MTD) {
        errno = EBADF;
        return -1;
    }
    if (eb < 0 || eb >= nand.eb_cnt || offs < 0 || (size_t)offs + count > (size_t)nand.eb_size) {
        errno = EINVAL;
        return -1;
    }
    if (count != (size_t)nand.min_io || offs % nand.min_io != 0)
        g_shape_bad++;
    if (nand.eio_after[eb] >= 0 && nand.attempts[eb] >= nand.eio_after[eb]) {
        nand.attempts[eb]++;
        errno = nand.fail_errno;
        return -1;
    }
    nand.attempts[eb]++;
    if (nand.n_write < NAND_LOG_MAX) {
        nand.wr[nand.n_write].eb   = eb;
        nand.wr[nand.n_write].offs = offs;
        nand.wr[nand.n_write].len  = (int)count;
    }
    nand.n_write++;
    memcpy(nand.img + (long long)eb * nand.eb_size + offs, buf, count);
    return (ssize_t)count;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    if (lvl == OSIF_LOG_WARN)
        n_warn_logs++;
    if (lvl == OSIF_LOG_ERR && strstr(fmt, "too many damaged blocks"))
        n_err_toomany++;
    if (lvl == OSIF_LOG_ERR && strstr(fmt, "cannot check a block for damage"))
        n_err_nocheck++;
}

static void fill_payload(unsigned char *p, size_t n, uint32_t seed)
{
    uint32_t s = seed;

    for (size_t i = 0; i < n; i++) {
        s    = s * 1103515245u + 12345u;
        p[i] = (unsigned char)((s >> 16) & 0xffu);
        if (p[i] == 0xff)
            p[i] = 0x5a;
    }
}

static int feed_fixed(const unsigned char *p, size_t len, size_t chunk)
{
    size_t off = 0;

    while (off < len) {
        size_t n = len - off;

        if (n > chunk)
            n = chunk;
        if (mtd_dev_write(0, (long long)off, p + off, n) != 0)
            return -1;
        off += n;
    }
    return 0;
}

static int feed_irregular(const unsigned char *p, size_t len)
{
    size_t off = 0;
    size_t i   = 0;

    while (off < len) {
        size_t n = k_irregular[i++ % (sizeof k_irregular / sizeof k_irregular[0])];

        if (n > len - off)
            n = len - off;
        if (mtd_dev_write(0, (long long)off, p + off, n) != 0)
            return -1;
        off += n;
    }
    return 0;
}

static int run_variant(const unsigned char *p, size_t len, size_t chunk, int irregular,
        unsigned char *out, wr_rec *wlog, int *nw, int *ne)
{
    int rc;

    nand_reset(512, 4096, 16);
    if (mtd_dev_init() != 0)
        return -1;
    rc = irregular ? feed_irregular(p, len) : feed_fixed(p, len, chunk);
    if (rc == 0)
        rc = mtd_dev_write_end(0, 1);
    if (out != NULL)
        memcpy(out, nand.img, (size_t)nand.size);
    if (wlog != NULL)
        memcpy(wlog, nand.wr, sizeof nand.wr);
    *nw = nand.n_write;
    *ne = nand.n_erase;
    mtd_dev_cleanup();
    return rc;
}

int main(void)
{
    static unsigned char img[NAND_MAX_IMG];
    static wr_rec        wlog[NAND_LOG_MAX];
    const size_t         LEN = 20000;
    int                  nw, ne;

    fill_payload(payload, sizeof payload, 0x2026u);

    CHECK(mtd_dev_write(0, 0, payload, 512) == -ENODEV, "write before init -> -ENODEV");

    nand_reset(512, 4096, 16);
    CHECK(mtd_dev_init() == 0, "init scans the fake MTD");
    CHECK(mtd_dev_size(0) == 65536, "mtd0 is 65536 bytes");
    CHECK(mtd_dev_size(1) == -ENODEV, "an absent mtd reports -ENODEV");
    CHECK(mtd_dev_from_name(NAND_PART) == 0, "name lookup resolves " NAND_PART);
    CHECK(mtd_dev_from_name("nope") == -1, "an unknown partition name -> -1");
    CHECK(mtd_dev_write(0, 0, NULL, 16) == -EINVAL, "NULL buf with len -> -EINVAL");
    CHECK(mtd_dev_write(0, 0, payload, 0) == 0 && nand.n_write == 0,
            "a zero-length write is a no-op");
    CHECK(mtd_dev_write(0, 300, payload, 512) == -EIO,
            "a start offset that is not page-aligned -> -EIO");
    mtd_dev_cleanup();

    CHECK(run_variant(payload, LEN, LEN, 0, base_img, base_wr, &base_nw, &ne) == 0,
            "one-chunk stream of 20000 bytes commits");
    CHECK(base_nw == 40, "20000 bytes over 512B pages = 40 page writes (got %d)", base_nw);
    CHECK(ne == 0, "a plain stream erases nothing (got %d)", ne);

    CHECK(run_variant(payload, LEN, 4096, 0, img, wlog, &nw, &ne) == 0,
            "4 KiB-chunk stream commits");
    CHECK(memcmp(img, base_img, 65536) == 0, "4 KiB chunks: byte-identical image");
    CHECK(nw == base_nw && memcmp(wlog, base_wr, sizeof base_wr) == 0,
            "4 KiB chunks: identical write log (%d vs %d)", nw, base_nw);
    CHECK(ne == 0, "4 KiB chunks erase nothing");

    CHECK(run_variant(payload, LEN, 1, 0, img, wlog, &nw, &ne) == 0, "1-byte-chunk stream commits");
    CHECK(memcmp(img, base_img, 65536) == 0, "1-byte chunks: byte-identical image");
    CHECK(nw == base_nw && memcmp(wlog, base_wr, sizeof base_wr) == 0,
            "1-byte chunks: identical write log (%d vs %d)", nw, base_nw);
    CHECK(ne == 0, "1-byte chunks erase nothing");
    CHECK(nand.n_open == 1, "20000 write calls open /dev/mtd0 once, not once per call (got %d)",
            nand.n_open);
    CHECK(nand.n_isbad == 5, "the bad-block probe runs once per eraseblock entered (got %d)",
            nand.n_isbad);

    CHECK(run_variant(payload, LEN, 0, 1, img, wlog, &nw, &ne) == 0,
            "prime/irregular-chunk stream commits");
    CHECK(memcmp(img, base_img, 65536) == 0, "irregular chunks: byte-identical image");
    CHECK(nw == base_nw && memcmp(wlog, base_wr, sizeof base_wr) == 0,
            "irregular chunks: identical write log (%d vs %d)", nw, base_nw);
    CHECK(ne == 0, "irregular chunks erase nothing");

    CHECK(memcmp(base_img, payload, LEN) == 0, "the committed image carries the payload verbatim");

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    MUST(feed_fixed(payload, LEN, 1) == 0);
    MUST(mtd_dev_write_end(0, 1) == 0);
    CHECK(nand_blank(20000, 20480), "the final partial page is 0xff-padded, not dropped");
    CHECK(nand_blank(20480, 65536), "nothing is written past the padded tail");
    mtd_dev_cleanup();

    nand_reset(256, 2048, 16);
    CHECK(mtd_dev_init() == 0, "init at a second geometry (256B page, 2 KiB block)");
    CHECK(feed_fixed(payload, 5000, 5000) == 0, "5000B in one chunk at 256/2048");
    CHECK(mtd_dev_write_end(0, 1) == 0, "commit at 256/2048");
    memcpy(img, nand.img, 32768);
    nw = nand.n_write;
    mtd_dev_cleanup();
    nand_reset(256, 2048, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(feed_irregular(payload, 5000) == 0, "5000B irregular at 256/2048");
    CHECK(mtd_dev_write_end(0, 1) == 0, "commit at 256/2048 (irregular)");
    CHECK(memcmp(img, nand.img, 32768) == 0 && nw == nand.n_write,
            "split invariance holds at the second geometry too");
    CHECK(nw == 20, "5000 bytes over 256B pages = 20 page writes (got %d)", nw);
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 700) == 0, "700B accepted");
    CHECK(nand.n_write == 1, "700B flushes 1 page and buffers the 188B tail (got %d)",
            nand.n_write);
    CHECK(mtd_dev_write(0, 700, payload + 700, 100) == 0, "+100B accepted");
    CHECK(nand.n_write == 1, "288 buffered bytes are still not on flash (got %d)", nand.n_write);
    CHECK(mtd_dev_write_end(0, 1) == 0, "write_end(commit) succeeds");
    CHECK(nand.n_write == 2, "write_end(commit) flushes the tail page (got %d)", nand.n_write);
    CHECK(memcmp(nand.img, payload, 800) == 0, "800 payload bytes landed at 0");
    CHECK(nand_blank(800, 1024), "the tail page is padded with 0xff to a full page");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 700) == 0, "700B accepted (non-commit arm)");
    n_warn_logs = 0;
    CHECK(mtd_dev_write_end(0, 0) == 0, "write_end(commit=0) returns 0");
    CHECK(n_warn_logs == 1,
            "write_end(commit=0) logs the dropped bytes once at WARN "
            "(got %d)",
            n_warn_logs);
    CHECK(nand.n_write == 1, "write_end(commit=0) drops the buffered tail (got %d)", nand.n_write);
    CHECK(nand_blank(512, 4096), "the dropped tail never reaches flash");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 700) == 0, "700B accepted (foreign mtdnum arm)");
    CHECK(mtd_dev_write_end(1, 1) == 0, "write_end on another mtdnum returns 0");
    CHECK(mtd_dev_write_end(-1, 1) == 0, "write_end on a negative mtdnum returns 0");
    CHECK(nand.n_write == 1, "neither touched the live session (got %d)", nand.n_write);
    CHECK(mtd_dev_write_end(0, 1) == 0 && nand.n_write == 2,
            "the session is still committable afterwards");
    CHECK(mtd_dev_write_end(0, 1) == 0, "write_end with no session open also returns 0");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    memset(payload + 512, 0xff, 512);
    CHECK(mtd_dev_write(0, 0, payload, 1024) == 0, "1024B whose second page is all 0xff");
    CHECK(mtd_dev_write_end(0, 1) == 0, "commit the all-0xff page case");
    CHECK(nand.n_write == 1, "an all-0xff page is not written at all (got %d)", nand.n_write);
    CHECK(nand_blank(512, 1024), "the skipped page stays erased");
    mtd_dev_cleanup();
    fill_payload(payload, sizeof payload, 0x2026u);

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_erase(0, 0, 0) == 0, "erase(start 0, size 0) covers the device");
    CHECK(nand.n_erase == 16, "16 eraseblocks erased exactly once each (got %d)", nand.n_erase);
    {
        int ok = 1;

        for (int i = 0; i < 16; i++)
            if (nand.erase_eb[i] != i || nand.erase_off[i] != (long long)i * 4096)
                ok = 0;
        CHECK(ok, "erase offsets are 0,4096,...,61440 in order");
    }
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_erase(0, 8192, 8192) == 0, "erase of a 2-block range");
    CHECK(nand.n_erase == 2 && nand.erase_eb[0] == 2 && nand.erase_eb[1] == 3,
            "only eb 2 and 3 erased (got %d)", nand.n_erase);
    CHECK(mtd_dev_erase(0, 0, 70000) == -EINVAL, "an out-of-range erase -> -EINVAL");
    CHECK(mtd_dev_erase(0, -1, 4096) == -EINVAL, "a negative erase start -> -EINVAL");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    nand_bad(0);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_erase(0, 0, 8192) == 0, "erase over a factory-bad block");
    CHECK(nand.n_erase == 2 && nand.erase_eb[0] == 1 && nand.erase_eb[1] == 2,
            "the bad block is skipped and the range extends by one (got %d)", nand.n_erase);
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    nand_bad(2);
    MUST(mtd_dev_init() == 0);
    CHECK(feed_fixed(payload, 12288, 300) == 0, "300B chunks stream across a factory-bad block");
    CHECK(mtd_dev_write_end(0, 1) == 0, "bad-block stream commits");
    CHECK(nand.n_write == 24, "24 pages written (got %d)", nand.n_write);
    {
        int hit = 0;

        for (int i = 0; i < nand.n_write; i++)
            if (nand.wr[i].eb == 2)
                hit++;
        CHECK(hit == 0, "no page write addressed the bad eraseblock (got %d)", hit);
    }
    CHECK(nand.n_markbad == 0, "a factory-bad block is not re-marked");
    CHECK(nand.n_erase == 0, "skipping a bad block erases nothing");
    CHECK(memcmp(nand.img, payload, 8192) == 0, "the first two blocks hold payload[0..8191]");
    CHECK(nand_blank(8192, 12288), "the skipped block is left erased");
    CHECK(memcmp(nand.img + 12288, payload + 8192, 4096) == 0,
            "the physical cursor resumed at 12288 while the logical count kept counting");
    CHECK(nand_blank(16384, 65536), "nothing spilled past the payload");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    nand_bad(2);
    MUST(mtd_dev_init() == 0);
    MUST(feed_fixed(payload, 12288, 1) == 0);
    MUST(mtd_dev_write_end(0, 1) == 0);
    CHECK(memcmp(nand.img, payload, 8192) == 0 && nand_blank(8192, 12288) &&
                    memcmp(nand.img + 12288, payload + 8192, 4096) == 0,
            "the same bad-block layout at 1-byte chunk granularity");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    nand_fail(1, 2);
    MUST(mtd_dev_init() == 0);
    CHECK(feed_fixed(payload, 12288, 300) == 0,
            "300B chunks stream across a block that EIOs mid-write");
    CHECK(mtd_dev_write_end(0, 1) == 0, "EIO stream commits");
    CHECK(nand.n_markbad == 1 && nand.markbad_eb[0] == 1,
            "mark-bad called once, on the failing block (got %d)", nand.n_markbad);
    CHECK(nand.n_erase == 1 && nand.erase_eb[0] == 1 && nand.erase_off[0] == 4096,
            "the failing block is erased once, at its own offset, before the mark "
            "(got %d)",
            nand.n_erase);
    CHECK(memcmp(nand.img, payload, 4096) == 0, "block 0 is untouched by the rewind");
    CHECK(nand_blank(4096, 8192), "the failing block is left erased");
    CHECK(memcmp(nand.img + 8192, payload + 4096, 4096) == 0,
            "the buffered block is replayed whole into the next good block");
    CHECK(memcmp(nand.img + 12288, payload + 8192, 4096) == 0,
            "the stream resumes after the replayed block with no payload loss");
    CHECK(nand.n_write == 26, "24 payload pages plus the 2 that landed before the EIO (got %d)",
            nand.n_write);
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    nand_fail(0, 0);
    nand.fail_errno = ENOSPC;
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 4096) != 0, "a non-EIO write failure fails the call");
    CHECK(nand.n_markbad == 0 && nand.n_erase == 0,
            "a non-EIO failure neither erases nor marks bad");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 2);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 12288) == -EIO,
            "an image larger than the device is refused up front");
    CHECK(nand.n_write == 0, "nothing was written by the refused call");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 2);
    MUST(mtd_dev_init() == 0);
    CHECK(feed_fixed(payload, 12288, 1) != 0, "byte-at-a-time past the end of the device fails");
    CHECK(nand.n_write == 16, "the device filled exactly, no silent truncation (got %d)",
            nand.n_write);
    CHECK(memcmp(nand.img, payload, 8192) == 0, "what did fit is intact");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 2);
    nand_bad(0);
    nand_bad(1);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 512) != 0, "running out of good blocks fails the write");
    CHECK(nand.n_write == 0, "no page was written with every block bad");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 2);
    nand_bad(1);
    MUST(mtd_dev_init() == 0);
    n_err_toomany = 0;
    n_err_nocheck = 0;
    CHECK(mtd_dev_write(0, 0, payload, 4608) != 0,
            "a stream that needs a bad last block fails the write");
    CHECK(nand.n_write == 8, "the good first block is filled (got %d)", nand.n_write);
    CHECK(n_err_toomany == 1 && n_err_nocheck == 0,
            "a bad last block reports too many damaged blocks, not a failed "
            "check (got %d/%d)",
            n_err_toomany, n_err_nocheck);
    CHECK(nand.n_isbad_oob == 0, "no bad-block probe is issued at the partition end (got %d)",
            nand.n_isbad_oob);
    mtd_dev_cleanup();

    nand_reset(512, 4096, 2);
    nand_fail(1, 7);
    MUST(mtd_dev_init() == 0);
    n_err_toomany = 0;
    CHECK(feed_fixed(payload, 8192, 512) != 0,
            "an EIO on the last page of the last block fails the write");
    CHECK(nand.n_markbad == 1 && nand.markbad_eb[0] == 1,
            "the failing last block is marked bad (got %d)", nand.n_markbad);
    CHECK(n_err_toomany == 1, "the unwritten block reports too many damaged blocks (got %d)",
            n_err_toomany);
    CHECK(nand_blank(4096, 8192), "the failing last block is left erased");
    mtd_dev_cleanup();

    nand_reset(512, 4096, 16);
    MUST(mtd_dev_init() == 0);
    CHECK(mtd_dev_write(0, 0, payload, 700) == 0, "700B opens a session at 0");
    CHECK(mtd_dev_write(0, 5120, payload, 512) == 0,
            "a write at a different logical offset opens a new session");
    CHECK(nand.n_write == 2, "the dropped session's 188 buffered bytes are lost (got %d)",
            nand.n_write);
    CHECK(nand_blank(512, 5120), "nothing was written between the two sessions");
    CHECK(memcmp(nand.img + 5120, payload, 512) == 0,
            "the new session lands at the physical offset it named");
    CHECK(mtd_dev_write_end(0, 1) == 0, "the restarted session commits");
    mtd_dev_cleanup();

    CHECK(g_shape_bad == 0,
            "every page write in every scenario was one page-aligned min_io page "
            "with no OOB (got %d)",
            g_shape_bad);
    CHECK(g_open_inherit == 0,
            "every open of /proc/mtd and /dev/mtdN is close-on-exec, so a child "
            "mm_miner starts does not inherit it (got %d without)",
            g_open_inherit);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
