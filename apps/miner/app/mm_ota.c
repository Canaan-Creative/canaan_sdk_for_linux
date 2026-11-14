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
#include "mm_system.h"
#include "mtd_dev.h"
#include "osif.h"
#include "ota_proto.h"
#include "ota_state.h"
#include "ubootenv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/vfs.h>

#define OTA_ENV_SUFFIX    "_partition" /* env key of a bin's running slot: <bin>_partition */
#define OTA_ENV_SKIP      "uboot_env"  /* bin that is never flashed and never flipped */
#define OTA_BIN_NAME_MAX  15           /* bin name, bytes without NUL */
#define OTA_PART_NAME_MAX 32           /* partition name or env key buffer, bytes with NUL */
#define OTA_ENV_PENDING   AUP_MAX_BINS /* slot switches queued per package */
#define OTA_CHUNK         4096         /* socket read and flash write piece, bytes */
#define OTA_STACK_BYTES   (64 * 1024)  /* service thread stack, bytes */
#define OTA_UBI_MAX       16           /* UBI devices checked */
#define ENV_FLUSH_FAIL    (-1)         /* env_flush(): the slot switch was not stored */

_Static_assert(OTA_BIN_NAME_MAX + sizeof(OTA_ENV_SUFFIX) <= OTA_PART_NAME_MAX,
        "<bin>_partition fits its buffer");

/* Slot switch committed to the env once the whole package succeeded */
typedef struct {
    char key[OTA_PART_NAME_MAX]; /* <bin>_partition */
    char slot;                   /* new value, 'A' or 'B' */
} ota_env_switch;

typedef struct {
    int            mtdnum;                     /* MTD of the current bin, -1 none */
    long long      partsize;                   /* size of that MTD, bytes */
    int            stage_fd;                   /* memfd holding the current bin, -1 none */
    char           env_slot;                   /* slot the current bin goes to, 0 none */
    char           env_key[OTA_PART_NAME_MAX]; /* <bin>_partition of the current bin */
    char           part[OTA_PART_NAME_MAX];    /* MTD label of the current bin */
    ota_env_switch pending[OTA_ENV_PENDING];
    int            npending;
} ota_mtd_ctx;

/* 1 once an upgrade succeeded in this process; OTA_SUCCEED_FILE covers restarts */
static int g_ota_success;

static int name_char_ok(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static void field_scrub(char *dst, size_t cap, const char *src, size_t n, int name)
{
    size_t o = 0;

    for (size_t i = 0; i < n && src[i] != '\0' && o + 1 < cap; i++) {
        unsigned char c  = (unsigned char)src[i];
        int           ok = name ? name_char_ok(c) : (c >= 0x20 && c <= 0x7e);

        dst[o++] = ok ? (char)c : '?';
    }
    dst[o] = '\0';
}

static size_t rtrim_len(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        n--;
    return n;
}

static int bin_name_get(const struct aup_bin *b, char out[OTA_BIN_NAME_MAX + 1])
{
    const char *end = memchr(b->name, '\0', sizeof b->name);
    size_t      len = end ? (size_t)(end - b->name) : sizeof b->name;
    int         ok  = end != NULL && len > 0 && len <= OTA_BIN_NAME_MAX;

    for (size_t i = 0; ok && i < len; i++)
        ok = name_char_ok((unsigned char)b->name[i]);
    if (!ok) {
        char shown[sizeof b->name + 1];

        field_scrub(shown, sizeof shown, b->name, len, 1);
        osif_log(OSIF_LOG_ERR, "OTA: bin '%s' is not a partition name", shown);
        return -1;
    }
    memcpy(out, b->name, len);
    out[len] = '\0';
    return 0;
}

static int bin_is_env(const struct aup_bin *b)
{
    return strncmp(b->name, OTA_ENV_SKIP, sizeof b->name) == 0;
}

static char slot_target(const char *running)
{
    return (*running == 'b' || *running == 'B') ? 'A' : 'B';
}

static void slot_part_name(char *dst, const char *bin, char slot)
{
    snprintf(dst, OTA_PART_NAME_MAX, "%s_%c", bin, slot == 'B' ? 'b' : 'a');
}

static long long mem_available(void)
{
    char      line[128];
    long long kb = -1;
    FILE     *f  = fopen("/proc/meminfo", "re");

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof line, f) != NULL)
        if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1)
            break;
    fclose(f);
    return kb < 0 ? -1 : kb * 1024;
}

static void stage_release(ota_mtd_ctx *ctx)
{
    if (ctx->stage_fd >= 0)
        close(ctx->stage_fd);
    ctx->stage_fd = -1;
}

static void bin_clear(ota_mtd_ctx *ctx)
{
    ctx->mtdnum   = -1;
    ctx->partsize = 0;
    ctx->env_slot = 0;
    ctx->part[0]  = '\0';
}

static int hdr_field_check(const char *key, const char *field, size_t n, int err)
{
    char   shown_pkg[64], shown_env[64];
    char  *env = ubootenv_get(key);
    size_t flen, elen;

    if (env == NULL) {
        osif_log(OSIF_LOG_ERR, "OTA: %s is not set in the U-Boot env; package refused", key);
        return err;
    }
    flen = rtrim_len(field, strnlen(field, n));
    elen = rtrim_len(env, strlen(env));
    if (flen == elen && memcmp(field, env, flen) == 0) {
        free(env);
        return 0;
    }
    field_scrub(shown_pkg, sizeof shown_pkg, field, flen, 0);
    field_scrub(shown_env, sizeof shown_env, env, elen, 0);
    osif_log(OSIF_LOG_ERR, "OTA: package %s '%s' does not match this board's '%s'", key, shown_pkg,
            shown_env);
    free(env);
    return err;
}

static int mtd_sink_check_hdr(void *arg, const struct aup_hdr *h)
{
    int rc;

    (void)arg;
    rc = hdr_field_check("SW", h->sw, sizeof h->sw, UPGRADE_SW_ERR);
    if (rc == 0)
        rc = hdr_field_check("HW", h->hw, sizeof h->hw, UPGRADE_HW_ERR);
    return rc;
}

static int ubi_dev_name(const char *s)
{
    if (strncmp(s, "ubi", 3) != 0 || s[3] == '\0')
        return 0;
    for (s += 3; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

static int ubi_mtds(int *nums, int cap)
{
    struct dirent *de;
    int            n = 0;
    DIR           *d = opendir(OTA_UBI_SYSFS);

    if (d == NULL)
        return errno == ENOENT ? 0 : -1;
    while ((de = readdir(d)) != NULL) {
        char  path[512];
        FILE *f;
        int   ok;

        if (!ubi_dev_name(de->d_name))
            continue;
        if (n >= cap) {
            n = -1;
            break;
        }
        snprintf(path, sizeof path, "%s/%s/mtd_num", OTA_UBI_SYSFS, de->d_name);
        f  = fopen(path, "re");
        ok = f != NULL && fscanf(f, "%d", &nums[n]) == 1;
        if (f != NULL)
            fclose(f);
        if (!ok) {
            n = -1;
            break;
        }
        n++;
    }
    closedir(d);
    return n;
}

static int slot_probe(const char *part, int32_t declared)
{
    int       num = mtd_dev_from_name(part);
    long long size;

    if (num < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: partition '%s' not found; package refused", part);
        return -1;
    }
    size = mtd_dev_size(num);
    if (size != (long long)declared) {
        osif_log(OSIF_LOG_ERR,
                "OTA: partition '%s' is %lld bytes, the package "
                "declares %d; package refused",
                part, size, (int)declared);
        return -1;
    }
    return num;
}

static int mtd_sink_check_part(void *arg, const struct aup_hdr *h)
{
    char name[OTA_BIN_NAME_MAX + 1];
    int  ubi[OTA_UBI_MAX];
    int  nbins = aup_be32(h->totalbins);
    int  nubi;

    (void)arg;
    if (nbins <= 0 || nbins > AUP_MAX_BINS)
        return UPGRADE_HEAER_ERR;
    for (int i = 0; i < nbins; i++)
        if (bin_name_get(&h->f[i], name) != 0)
            return UPGRADE_HEAER_ERR;

    if (mtd_dev_init() != 0) {
        osif_log(OSIF_LOG_ERR, "OTA: MTD partitions unreadable; package refused");
        return UPGRADE_PARTITION_ERR;
    }
    nubi = ubi_mtds(ubi, OTA_UBI_MAX);
    if (nubi < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: UBI state unreadable under %s; package refused",
                OTA_UBI_SYSFS);
        return UPGRADE_PARTITION_ERR;
    }

    for (int i = 0; i < nbins; i++) {
        const struct aup_bin *b        = &h->f[i];
        int32_t               declared = aup_be32(b->partsize);
        char  key[OTA_PART_NAME_MAX], part_a[OTA_PART_NAME_MAX], part_b[OTA_PART_NAME_MAX];
        char *running;
        int   num_a, num_b, target;
        char  slot;

        if (bin_is_env(b))
            continue;
        (void)bin_name_get(b, name);
        snprintf(key, sizeof key, "%s" OTA_ENV_SUFFIX, name);
        running = ubootenv_get(key);
        if (running == NULL) {
            osif_log(OSIF_LOG_ERR, "OTA: '%s' is unset; package refused", key);
            return UPGRADE_PARTITION_ERR;
        }
        slot = slot_target(running);
        free(running);

        slot_part_name(part_a, name, 'A');
        slot_part_name(part_b, name, 'B');
        num_a = slot_probe(part_a, declared);
        if (num_a < 0)
            return UPGRADE_PARTITION_ERR;
        num_b = slot_probe(part_b, declared);
        if (num_b < 0)
            return UPGRADE_PARTITION_ERR;

        target = slot == 'A' ? num_a : num_b;
        for (int u = 0; u < nubi; u++) {
            if (ubi[u] != target)
                continue;
            osif_log(OSIF_LOG_ERR,
                    "OTA: partition '%s' (mtd%d) is attached to UBI; "
                    "package refused",
                    slot == 'A' ? part_a : part_b, target);
            return UPGRADE_PARTITION_ERR;
        }
    }
    return 0;
}

static int mtd_sink_begin(void *arg, const struct aup_bin *b)
{
    ota_mtd_ctx *ctx = arg;
    char         name[OTA_BIN_NAME_MAX + 1];
    int32_t      size = aup_be32(b->size);
    long long    avail;
    char        *running;
    char         slot;
    int          num;
    long long    partsize;

    bin_clear(ctx);
    ctx->stage_fd = -1;
    if (bin_name_get(b, name) != 0)
        return -1;
    if (bin_is_env(b))
        return 0;

    snprintf(ctx->env_key, sizeof ctx->env_key, "%s" OTA_ENV_SUFFIX, name);
    running = ubootenv_get(ctx->env_key);
    if (running == NULL) {
        osif_log(OSIF_LOG_ERR, "OTA: '%s' is unset; bin '%s' refused", ctx->env_key, name);
        return -1;
    }
    slot = slot_target(running);
    free(running);

    slot_part_name(ctx->part, name, slot);
    num = mtd_dev_from_name(ctx->part);
    if (num < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: partition '%s' not found; bin refused", ctx->part);
        bin_clear(ctx);
        return -1;
    }
    partsize = mtd_dev_size(num);
    if (partsize <= 0) {
        osif_log(OSIF_LOG_ERR, "OTA: partition '%s' has no usable size; bin refused", ctx->part);
        bin_clear(ctx);
        return -1;
    }
    avail = mem_available();
    if (size > 0 && avail >= 0 && (long long)size > avail) {
        osif_log(OSIF_LOG_ERR,
                "OTA: bin '%s' is %d bytes, only %lld bytes of RAM "
                "available to stage it; bin refused",
                name, (int)size, avail);
        bin_clear(ctx);
        return -1;
    }
    ctx->stage_fd = memfd_create("mm_ota_stage", MFD_CLOEXEC);
    if (ctx->stage_fd < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: cannot create the memory stage for '%s': %s", name,
                strerror(errno));
        ctx->stage_fd = -1;
        bin_clear(ctx);
        return -1;
    }
    ctx->mtdnum   = num;
    ctx->partsize = partsize;
    ctx->env_slot = slot;
    return 0;
}

static int mtd_sink_write(void *arg, const struct aup_bin *b, long long off, const void *data,
        size_t len)
{
    ota_mtd_ctx         *ctx  = arg;
    const unsigned char *p    = data;
    size_t               done = 0;

    if (bin_is_env(b))
        return 0;
    if (ctx->stage_fd < 0 || off < 0 || off > ctx->partsize || (long long)len > ctx->partsize - off)
        return -1;
    while (done < len) {
        ssize_t n = pwrite(ctx->stage_fd, p + done, len - done, (off_t)(off + (long long)done));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            osif_log(OSIF_LOG_ERR, "OTA: cannot stage %zu bytes of '%s' in RAM", len, ctx->part);
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int stage_read(int fd, unsigned char *buf, size_t len, off_t off)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, off + (off_t)done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int stage_flash(ota_mtd_ctx *ctx)
{
    unsigned char buf[OTA_CHUNK];
    struct stat   st;
    long long     len, off;

    if (fstat(ctx->stage_fd, &st) != 0 || st.st_size > ctx->partsize)
        return -1;
    len = st.st_size;
    if (mtd_dev_erase(ctx->mtdnum, 0, ctx->partsize) < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: erasing '%s' failed", ctx->part);
        return -1;
    }
    for (off = 0; off < len; off += OTA_CHUNK) {
        size_t n = len - off < OTA_CHUNK ? (size_t)(len - off) : OTA_CHUNK;

        if (stage_read(ctx->stage_fd, buf, n, (off_t)off) != 0 ||
                mtd_dev_write(ctx->mtdnum, off, buf, n) < 0) {
            osif_log(OSIF_LOG_ERR, "OTA: writing '%s' failed at offset %lld", ctx->part, off);
            mtd_dev_write_end(ctx->mtdnum, 0);
            return -1;
        }
    }
    if (mtd_dev_write_end(ctx->mtdnum, 1) < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: writing the last page of '%s' failed", ctx->part);
        return -1;
    }
    osif_log(OSIF_LOG_INFO, "OTA: '%s' written, %lld bytes", ctx->part, len);
    return 0;
}

static int mtd_sink_end(void *arg, const struct aup_bin *b)
{
    ota_mtd_ctx *ctx = arg;
    int          rc  = -1;

    if (bin_is_env(b))
        return 0;
    if (ctx->mtdnum < 0 || ctx->stage_fd < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: bin ended with no partition open");
    } else if (ctx->npending >= OTA_ENV_PENDING) {
        osif_log(OSIF_LOG_ERR, "OTA: more than %d slot switches in one package", OTA_ENV_PENDING);
    } else if (stage_flash(ctx) == 0) {
        ota_env_switch *sw = &ctx->pending[ctx->npending++];

        snprintf(sw->key, sizeof sw->key, "%s", ctx->env_key);
        sw->slot = ctx->env_slot;
        rc       = 0;
    }
    stage_release(ctx);
    bin_clear(ctx);
    return rc;
}

static int list_write(const char *path, const char *body, size_t len)
{
    size_t done = 0;
    int    fd;

    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    while (done < len) {
        ssize_t n = write(fd, body + done, len - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    return close(fd) == 0 ? 0 : -1;
}

static int env_flush(ota_mtd_ctx *ctx)
{
    char   body[OTA_ENV_PENDING * (OTA_PART_NAME_MAX + 3) + 1];
    char   keys[OTA_ENV_PENDING * (OTA_PART_NAME_MAX + 3) + 1];
    size_t blen = 0, klen = 0;
    int    rc;

    if (ctx->npending == 0)
        return 0;
    body[0] = '\0';
    keys[0] = '\0';
    for (int i = 0; i < ctx->npending && i < OTA_ENV_PENDING; i++) {
        const ota_env_switch *sw = &ctx->pending[i];

        blen += (size_t)snprintf(body + blen, sizeof body - blen, "%s=%c\n", sw->key, sw->slot);
        klen += (size_t)snprintf(keys + klen, sizeof keys - klen, "%s%s=%c", i ? " " : "", sw->key,
                sw->slot);
    }
    ctx->npending = 0;

    rc = list_write(OTA_ENV_LIST_FILE, body, blen);
    if (rc == 0)
        rc = ubootenv_apply_list(OTA_ENV_LIST_FILE);
    unlink(OTA_ENV_LIST_FILE);
    if (rc != 0) {
        osif_log(OSIF_LOG_ERR,
                "OTA: env store failed (%d), slot switch NOT committed "
                "for %s; the running image still boots, re-run the upgrade",
                rc, keys);
        return ENV_FLUSH_FAIL;
    }
    osif_log(OSIF_LOG_INFO, "OTA: slot switch committed: %s", keys);
    return 0;
}

static int ota_latched(void)
{
    struct stat st;

    return g_ota_success || lstat(OTA_SUCCEED_FILE, &st) == 0;
}

static void ota_latch_set(void)
{
    int fd;

    g_ota_success = 1;
    fd            = open(OTA_SUCCEED_FILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0);
    if (fd >= 0)
        close(fd);
    else if (errno != EEXIST)
        osif_log(OSIF_LOG_ERR, "OTA: cannot create %s: %s", OTA_SUCCEED_FILE, strerror(errno));
}

static int ota_path_is_tmpfs(const char *path)
{
    char          dir[256];
    struct statfs sf;
    char         *slash;

    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL)
        snprintf(dir, sizeof dir, ".");
    else if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    if (statfs(dir, &sf) != 0)
        return -1;
    return sf.f_type == TMPFS_MAGIC;
}

static int listener_create(const char *path)
{
    struct sockaddr_un sa;
    int                fd;

    if (strlen(path) >= sizeof sa.sun_path)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || chmod(path, S_IRUSR | S_IWUSR) != 0 ||
            listen(fd, 1) != 0) {
        osif_log(OSIF_LOG_ERR, "OTA: cannot listen on %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void peer_reply(int fd, int error, int8_t percent)
{
    mm_ota_msg  msg;
    const char *p    = (const char *)&msg;
    size_t      done = 0;

    memset(&msg, 0, sizeof msg);
    msg.magic           = (int)MM_OTA_MAGIC;
    msg.type            = MM_OTA_MSG_TYPE;
    msg.upgrade.stat    = 0;
    msg.upgrade.percent = percent;
    msg.upgrade.error   = (int8_t)error;
    msg.upgrade.set     = 0;
    while (done < sizeof msg) {
        ssize_t n = send(fd, p + done, sizeof msg - done, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            osif_log(OSIF_LOG_WARN, "OTA: status reply not delivered: %s", strerror(errno));
            return;
        }
        done += (size_t)n;
    }
}

static void peer_drain(int fd, unsigned char *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
    }
}

static void serve(int fd, ota_state_t *st, ota_mtd_ctx *ctx)
{
    unsigned char buf[OTA_CHUNK];
    ota_sink      sink;
    int8_t        percent = 0;
    int           rc      = UPGRADE_PROCESS_OK;

    memset(ctx, 0, sizeof *ctx);
    ctx->stage_fd = -1;
    bin_clear(ctx);
    if (ota_latched()) {
        osif_log(OSIF_LOG_OP,
                "OTA: upload refused, an upgrade already succeeded "
                "and waits for its reboot");
        peer_reply(fd, UPGRADE_ALREADY_ERR, 0);
        return;
    }

    memset(&sink, 0, sizeof sink);
    sink.begin      = mtd_sink_begin;
    sink.write      = mtd_sink_write;
    sink.end        = mtd_sink_end;
    sink.check_hdr  = mtd_sink_check_hdr;
    sink.check_part = mtd_sink_check_part;
    sink.ctx        = ctx;
    ota_reset(st, &sink);

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        rc = ota_feed(st, buf, (int)n, &percent);
        if (rc != UPGRADE_PROCESS_OK)
            break;
    }
    if (rc == UPGRADE_SUCCEED) {
        peer_drain(fd, buf, sizeof buf);
        if (env_flush(ctx) != 0)
            rc = UPGRADE_PARTITION_ERR;
    }
    stage_release(ctx);
    bin_clear(ctx);
    ctx->npending = 0;
    mtd_dev_cleanup();

    if (rc == UPGRADE_SUCCEED) {
        ota_latch_set();
        osif_log(OSIF_LOG_OP, "OTA: upgrade succeeded");
    } else if (rc == UPGRADE_PROCESS_OK) {
        osif_log(OSIF_LOG_OP, "OTA: upload ended incomplete at %d%%, nothing switched", percent);
    } else {
        osif_log(OSIF_LOG_ERR, "OTA: upgrade failed (%d), nothing switched", rc);
    }
    peer_reply(fd, rc, percent);
    if (rc == UPGRADE_SUCCEED)
        ota_arm_reboot();
}

static void *ota_thread(void *arg)
{
    static ota_mtd_ctx ctx;
    struct timeval     tv = { .tv_sec = MM_OTA_PEER_STALL_S, .tv_usec = 0 };
    ota_state_t       *st;
    int                lfd;

    (void)arg;
    if (ota_path_is_tmpfs(OTA_SUCCEED_FILE) != 1)
        osif_log(OSIF_LOG_ERR,
                "OTA: %s is not on tmpfs; after one successful upgrade "
                "every later upload is refused",
                OTA_SUCCEED_FILE);
    st = malloc(ota_state_size());
    if (st == NULL) {
        osif_log(OSIF_LOG_ERR, "OTA: service unavailable this boot: out of memory");
        return NULL;
    }
    lfd = listener_create(MM_OTA_SOCK_PATH);
    if (lfd < 0) {
        osif_log(OSIF_LOG_ERR, "OTA: service unavailable this boot");
        free(st);
        return NULL;
    }
    for (;;) {
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

        if (c < 0) {
            if (errno != EINTR)
                sleep(1);
            continue;
        }
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        serve(c, st, &ctx);
        close(c);
    }
    return NULL;
}

int mm_ota_start(void)
{
    osif_thread_t *t = NULL;

    if (osif_thread_create(&t, "mm_ota", ota_thread, NULL, OTA_STACK_BYTES) != OSIF_OK) {
        osif_log(OSIF_LOG_ERR,
                "OTA: firmware upgrade unavailable this boot: thread "
                "start failed");
        return -1;
    }
    return 0;
}
