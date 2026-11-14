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

#include "mtd_dev.h"
#include "osif.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>

#define MTD_PROC_PATH  "/proc/mtd"
#define MTD_PROC_BYTES 4096
#define MTD_PARTS_MAX  32
#define MTD_NAME_MAX   64 /* partition name buffer, bytes with NUL */

/* MTD partition listed in /proc/mtd, geometry from MEMGETINFO */
typedef struct {
    int       num; /* N of /dev/mtdN */
    char      name[MTD_NAME_MAX];
    long long size;   /* bytes */
    unsigned  ebsize; /* erase block, bytes */
    unsigned  unit;   /* write unit: page on NAND, erase block otherwise */
    int       nand;   /* 1 on NAND: bad blocks are probed and retired */
} mtd_part;

typedef struct {
    const mtd_part *part;  /* target, NULL when no session */
    int             fd;    /* read-write descriptor, -1 when no session */
    long long       next;  /* logical offset the next call must start at */
    int             blk;   /* physical erase block receiving data */
    int             good;  /* 1 once blk is known good */
    unsigned        first; /* in-block offset of the first byte blk received */
    unsigned        done;  /* in-block offset up to which units are on flash */
    unsigned        fill;  /* bytes buffered after done */
    unsigned char  *buf;   /* contents of blk, one erase block */
} mtd_stream;

static mtd_part   g_parts[MTD_PARTS_MAX];
static int        g_nparts = -1; /* -1 before mtd_dev_init */
static mtd_stream g_st     = { .fd = -1 };

static const mtd_part *part_get(int mtdnum)
{
    for (int i = 0; i < g_nparts; i++)
        if (g_parts[i].num == mtdnum)
            return &g_parts[i];
    return NULL;
}

static int part_geometry(mtd_part *p)
{
    struct mtd_info_user mi;
    char                 path[32];
    int                  fd, rc;

    snprintf(path, sizeof path, "/dev/mtd%d", p->num);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    rc = ioctl(fd, MEMGETINFO, &mi);
    close(fd);
    if (rc < 0 || mi.erasesize == 0 || mi.size == 0)
        return -1;
    p->size   = mi.size;
    p->ebsize = mi.erasesize;
    p->nand   = mi.type == MTD_NANDFLASH || mi.type == MTD_MLCNANDFLASH;
    p->unit   = p->nand ? mi.writesize : mi.erasesize;
    if (p->unit == 0 || p->ebsize % p->unit != 0)
        return -1;
    return 0;
}

static void parts_parse(char *text)
{
    char *line = text;

    while (line && *line && g_nparts < MTD_PARTS_MAX) {
        char     *nl = strchr(line, '\n');
        mtd_part *p  = &g_parts[g_nparts];

        if (nl)
            *nl = '\0';
        memset(p, 0, sizeof *p);
        if (sscanf(line, "mtd%d: %*x %*x \"%63[^\"]\"", &p->num, p->name) == 2) {
            if (part_geometry(p) == 0)
                g_nparts++;
            else
                osif_log(OSIF_LOG_WARN, "mtd%d: no geometry, '%s' ignored", p->num, p->name);
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void stream_close(void)
{
    if (g_st.fd >= 0)
        close(g_st.fd);
    free(g_st.buf);
    memset(&g_st, 0, sizeof g_st);
    g_st.fd = -1;
}

void mtd_dev_cleanup(void)
{
    stream_close();
    g_nparts = -1;
}

int mtd_dev_init(void)
{
    static char text[MTD_PROC_BYTES];
    size_t      len = 0;
    int         fd;

    mtd_dev_cleanup();
    fd = open(MTD_PROC_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;

        osif_log(OSIF_LOG_ERR, "mtd: cannot open %s: %s", MTD_PROC_PATH, strerror(err));
        return -err;
    }
    for (;;) {
        char    chunk[256];
        ssize_t n = read(fd, chunk, sizeof chunk);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if ((size_t)n > sizeof text - 1 - len)
            n = (ssize_t)(sizeof text - 1 - len);
        memcpy(text + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(fd);
    text[len] = '\0';
    g_nparts  = 0;
    parts_parse(text);
    return 0;
}

int mtd_dev_from_name(const char *name)
{
    if (name == NULL)
        return -1;
    for (int i = 0; i < g_nparts; i++)
        if (strcmp(g_parts[i].name, name) == 0)
            return g_parts[i].num;
    return -1;
}

long long mtd_dev_size(int mtdnum)
{
    const mtd_part *p = part_get(mtdnum);

    return p ? p->size : -ENODEV;
}

static int part_open_rw(const mtd_part *p)
{
    char path[32];
    int  fd;

    snprintf(path, sizeof path, "/dev/mtd%d", p->num);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;

        osif_log(OSIF_LOG_ERR, "mtd%d: cannot open for writing: %s", p->num, strerror(err));
        return -err;
    }
    return fd;
}

static int blk_count(const mtd_part *p)
{
    return (int)(p->size / p->ebsize);
}

static int blk_is_bad(int fd, const mtd_part *p, int blk)
{
    long long ofs = (long long)blk * p->ebsize;
    int       rc;

    if (!p->nand)
        return 0;
    rc = ioctl(fd, MEMGETBADBLOCK, &ofs);
    if (rc < 0) {
        osif_log(OSIF_LOG_ERR, "mtd%d: cannot check a block for damage at 0x%llx: %s", p->num, ofs,
                strerror(errno));
        return -EIO;
    }
    return rc > 0;
}

static int blk_erase(int fd, const mtd_part *p, int blk)
{
    struct erase_info_user ei;

    ei.start  = (__u32)((long long)blk * p->ebsize);
    ei.length = p->ebsize;
    if (ioctl(fd, MEMISLOCKED, &ei) > 0 && ioctl(fd, MEMUNLOCK, &ei) < 0)
        osif_log(OSIF_LOG_WARN, "mtd%d: cannot unlock block at 0x%x", p->num, ei.start);
    if (ioctl(fd, MEMERASE, &ei) < 0)
        return -errno;
    return 0;
}

static void blk_mark_bad(int fd, const mtd_part *p, int blk)
{
    long long ofs = (long long)blk * p->ebsize;

    if (ioctl(fd, MEMSETBADBLOCK, &ofs) < 0)
        osif_log(OSIF_LOG_WARN, "mtd%d: cannot mark the block at 0x%llx bad: %s", p->num, ofs,
                strerror(errno));
    else
        osif_log(OSIF_LOG_WARN, "mtd%d: block at 0x%llx failed and is marked bad", p->num, ofs);
}

int mtd_dev_erase(int mtdnum, long long start, long long size)
{
    const mtd_part *p = part_get(mtdnum);
    long long       need;
    int             fd, blk, rc = 0;

    if (p == NULL)
        return -ENODEV;
    if (start < 0 || size < 0 || start > p->size || start % p->ebsize != 0)
        return -EINVAL;
    if (size == 0)
        size = p->size - start;
    if (size > p->size - start)
        return -EINVAL;

    fd = part_open_rw(p);
    if (fd < 0)
        return fd;
    blk  = (int)(start / p->ebsize);
    need = (size + p->ebsize - 1) / p->ebsize;
    while (need > 0 && blk < blk_count(p)) {
        int bad = blk_is_bad(fd, p, blk);

        if (bad < 0) {
            rc = bad;
            break;
        }
        if (bad) {
            blk++;
            continue;
        }
        rc = blk_erase(fd, p, blk);
        if (rc == -EIO && p->nand) {
            blk_mark_bad(fd, p, blk);
            rc = 0;
            blk++;
            continue;
        }
        if (rc < 0) {
            osif_log(OSIF_LOG_ERR, "mtd%d: erase at 0x%llx failed: %s", p->num,
                    (long long)blk * p->ebsize, strerror(-rc));
            break;
        }
        need--;
        blk++;
    }
    close(fd);
    return rc;
}

static int stream_find_good(void)
{
    const mtd_part *p = g_st.part;

    for (;;) {
        int bad;

        if (g_st.blk >= blk_count(p)) {
            osif_log(OSIF_LOG_ERR,
                    "mtd%d: too many damaged blocks, the image "
                    "does not fit",
                    p->num);
            return -ENOSPC;
        }
        bad = blk_is_bad(g_st.fd, p, g_st.blk);
        if (bad < 0)
            return bad;
        if (!bad) {
            g_st.good = 1;
            return 0;
        }
        g_st.blk++;
    }
}

static int unit_is_blank(const unsigned char *u, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (u[i] != 0xff)
            return 0;
    return 1;
}

static int unit_program(unsigned ofs)
{
    const mtd_part      *p   = g_st.part;
    const unsigned char *u   = g_st.buf + ofs;
    off_t                pos = (off_t)g_st.blk * p->ebsize + ofs;

    if (unit_is_blank(u, p->unit))
        return 0;
    for (;;) {
        ssize_t n = pwrite(g_st.fd, u, p->unit, pos);

        if (n == (ssize_t)p->unit)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 ? -errno : -EIO;
    }
}

static int stream_program(unsigned from, unsigned to)
{
    const mtd_part *p   = g_st.part;
    unsigned        ofs = from;

    while (ofs < to) {
        int rc;

        if (!g_st.good) {
            rc = stream_find_good();
            if (rc < 0)
                return rc;
        }
        rc = unit_program(ofs);
        if (rc == 0) {
            ofs += p->unit;
            continue;
        }
        if (rc != -EIO || !p->nand) {
            osif_log(OSIF_LOG_ERR, "mtd%d: write at 0x%llx failed: %s", p->num,
                    (long long)g_st.blk * p->ebsize + ofs, strerror(-rc));
            return rc;
        }
        (void)blk_erase(g_st.fd, p, g_st.blk);
        blk_mark_bad(g_st.fd, p, g_st.blk);
        g_st.blk++;
        g_st.good = 0;
        ofs       = g_st.first;
    }
    return 0;
}

static int stream_unit_done(void)
{
    const mtd_part *p  = g_st.part;
    int             rc = stream_program(g_st.done, g_st.done + p->unit);

    if (rc < 0)
        return rc;
    g_st.done += p->unit;
    g_st.fill = 0;
    if (g_st.done == p->ebsize) {
        g_st.blk++;
        g_st.good  = 0;
        g_st.first = 0;
        g_st.done  = 0;
    }
    return 0;
}

static void stream_drop(void)
{
    if (g_st.fd >= 0 && g_st.fill > 0)
        osif_log(OSIF_LOG_WARN, "mtd%d: write abandoned, %u buffered bytes dropped", g_st.part->num,
                g_st.fill);
    stream_close();
}

static int stream_open(const mtd_part *p, long long offset)
{
    int fd;

    if (offset % p->unit != 0)
        return -EIO;
    g_st.buf = malloc(p->ebsize);
    if (g_st.buf == NULL)
        return -ENOMEM;
    fd = part_open_rw(p);
    if (fd < 0) {
        stream_close();
        return fd;
    }
    g_st.part  = p;
    g_st.fd    = fd;
    g_st.next  = offset;
    g_st.blk   = (int)(offset / p->ebsize);
    g_st.good  = 0;
    g_st.first = (unsigned)(offset % p->ebsize);
    g_st.done  = g_st.first;
    g_st.fill  = 0;
    return 0;
}

int mtd_dev_write(int mtdnum, long long offset, const void *buf, size_t len)
{
    const mtd_part      *p   = part_get(mtdnum);
    const unsigned char *src = buf;
    int                  rc;

    if (p == NULL)
        return -ENODEV;
    if (buf == NULL && len > 0)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (offset < 0 || offset > p->size || (long long)len > p->size - offset) {
        osif_log(OSIF_LOG_ERR, "mtd%d: %zu bytes at 0x%llx do not fit %lld bytes", p->num, len,
                offset, p->size);
        return -EIO;
    }
    if (g_st.fd < 0 || g_st.part != p || g_st.next != offset) {
        stream_drop();
        rc = stream_open(p, offset);
        if (rc < 0)
            return rc;
    }

    while (len > 0) {
        size_t n = p->unit - g_st.fill;

        if (n > len)
            n = len;
        memcpy(g_st.buf + g_st.done + g_st.fill, src, n);
        g_st.fill += (unsigned)n;
        g_st.next += (long long)n;
        src += n;
        len -= n;
        if (g_st.fill == p->unit) {
            rc = stream_unit_done();
            if (rc < 0) {
                stream_close();
                return rc;
            }
        }
    }
    return 0;
}

int mtd_dev_write_end(int mtdnum, int commit)
{
    int rc = 0;

    if (g_st.fd < 0 || g_st.part->num != mtdnum)
        return 0;
    if (!commit) {
        stream_drop();
        return 0;
    }
    if (g_st.fill > 0) {
        memset(g_st.buf + g_st.done + g_st.fill, 0xff, g_st.part->unit - g_st.fill);
        rc = stream_unit_done();
    }
    stream_close();
    return rc;
}
