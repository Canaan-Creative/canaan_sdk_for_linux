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

#include "core_wallpaper.h"
#include "core_ascset_internal.h"
#include "cfgwr_client.h"
#include "cfgwr_proto.h"
#include "ctrl_args.h"
#include "ctrl_cmds.h"
#include "core_sigverify.h"
#include "hex.h"
#include "mc_api_setter.h"
#include "miner_ipc.h"
#include "sha256.h"
#include "wp_proto.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WP_IDLE_MS 300000u

enum wp_result {
    WP_OK          = 0,
    WP_ERR_HEADER  = 1, /* header missing or not hex */
    WP_ERR_MAGIC   = 2,
    WP_ERR_VERSION = 3,
    WP_ERR_SIZE    = 4, /* file size out of range */
    WP_ERR_OFFSET  = 5,
    WP_ERR_PAYLOAD = 6, /* payload length or hex mismatch */
    WP_ERR_FILE    = 7, /* staging file or forward failure */
    WP_ERR_SHALEN  = 8, /* signature chunk has the wrong length */
    WP_ERR_SHA     = 9, /* file SHA-256 mismatch */
    WP_ERR_VERIFY  = 10,
    WP_ERR_UNKNOWN = 0xFF,
};

static const char *g_stage = MM_CFGWR_WP_STAGE_DIR;

static uint32_t        g_uid_cur;
static uint32_t        g_offset_cur;           /* next expected file offset, bytes */
static uint8_t         g_data_sha[WP_SHA_LEN]; /* signed SHA-256 of the file in progress */
static char            g_fmt_cur[8];
static uint64_t        g_last_ms;
static pthread_mutex_t g_wp_lock = PTHREAD_MUTEX_INITIALIZER;

static void stage_file(char *out, size_t cap, uint32_t uid, const char *fmt)
{
    snprintf(out, cap, MM_CFGWR_WP_STAGE_FMT, g_stage, (unsigned)uid, fmt);
}

static void rm_regular(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    struct dirent *e;
    char           path[512];
    while ((e = readdir(dp))) {
        if (e->d_type == DT_REG) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            remove(path);
        }
    }
    closedir(dp);
}

static int wp_end(const char *sfile, int ret)
{
    if (sfile)
        remove(sfile);
    g_uid_cur    = 0;
    g_offset_cur = 0;
    memset(g_data_sha, 0, sizeof g_data_sha);
    g_fmt_cur[0] = '\0';
    return ret;
}

static uint64_t wp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void wp_expire(void)
{
    char sfile[512];

    if (!g_fmt_cur[0] || wp_now_ms() - g_last_ms < WP_IDLE_MS)
        return;
    osif_log(OSIF_LOG_WARN, "wallpaper upload %u abandoned: no data for %u s", (unsigned)g_uid_cur,
            WP_IDLE_MS / 1000u);
    stage_file(sfile, sizeof sfile, g_uid_cur, g_fmt_cur);
    wp_end(sfile, WP_OK);
}

static int wallpaper_update(const char *content, const char *format, int *display)
{
    struct wp_header hdr;
    uint8_t          buf[4096] = { 0 };
    char             sfile[512];

    *display = 0;

    if (strcmp(format, "none") == 0) {
        rm_regular(g_stage);
        if (cfgwr_clear_wallpaper() != 0) {
            osif_log(OSIF_LOG_ERR, "wallpaper not cleared: mm_miner did not take the request");
            return WP_ERR_FILE;
        }
        osif_log(OSIF_LOG_INFO, "API wallpaper clear: forwarded to mm_miner");
        *display = 1;
        return WP_OK;
    }

    if (strlen(content) <= (size_t)(WP_HDR_LEN * 2))
        return WP_ERR_HEADER;
    if (!hex_valid(content, 2 * WP_HDR_LEN))
        return WP_ERR_HEADER;

    hex_decode((uint8_t *)&hdr, content, WP_HDR_LEN);

    if (strncmp((char *)hdr.magic, WP_MAGIC, strlen(WP_MAGIC)))
        return WP_ERR_MAGIC;
    if (hdr.format_ver != WP_VER)
        return WP_ERR_VERSION;
    if (hdr.file_size > WP_SIZE_MAX || hdr.file_size < WP_SIZE_MIN)
        return WP_ERR_SIZE;
    if ((size_t)(hdr.payload_len + WP_HDR_LEN) * 2 != strlen(content))
        return WP_ERR_PAYLOAD;
    if (!hex_valid(content + WP_HDR_LEN * 2, 2 * hdr.payload_len))
        return WP_ERR_PAYLOAD;
    if ((hdr.offset + hdr.payload_len) > hdr.file_size)
        return WP_ERR_OFFSET;

    if (hdr.idx == 0) {
        if (hdr.offset != 0)
            return WP_ERR_OFFSET;
        if (hdr.payload_len != (WP_SHA_LEN + WP_SIGN_LEN))
            return WP_ERR_SHALEN;
        hex_decode(buf, content + (WP_HDR_LEN * 2), hdr.payload_len);

        uint8_t sha[WP_SHA_LEN], sign[WP_SIGN_LEN], hash[WP_SHA_LEN];
        memcpy(sha, buf, WP_SHA_LEN);
        memcpy(sign, buf + WP_SHA_LEN, WP_SIGN_LEN);
        sha256(sha, WP_SHA_LEN, hash);
        if (!core_sig_verify("wallpaper", core_pic_public_pem, 1, sign, hash))
            return WP_ERR_VERIFY;

        stage_file(sfile, sizeof sfile, g_uid_cur, g_fmt_cur);
        if (hdr.uid == g_uid_cur && memcmp(sha, g_data_sha, WP_SHA_LEN) == 0 &&
                access(sfile, F_OK) == 0) {
            g_last_ms = wp_now_ms();
            return WP_OK;
        }

        if (mkdir(g_stage, 0755) != 0 && errno != EEXIST) {
            osif_log(OSIF_LOG_ERR, "wallpaper not saved: cannot create the staging folder (%s)",
                    strerror(errno));
            return WP_ERR_FILE;
        }
        rm_regular(g_stage);
        stage_file(sfile, sizeof sfile, hdr.uid, format);
        FILE *fp = fopen(sfile, "w+");
        if (!fp) {
            osif_log(OSIF_LOG_ERR, "wallpaper not saved: cannot open the staged file (%s)",
                    strerror(errno));
            return WP_ERR_FILE;
        }
        fsync(fileno(fp));
        fclose(fp);
        g_uid_cur    = hdr.uid;
        g_offset_cur = 0;
        memcpy(g_data_sha, sha, WP_SHA_LEN);
        snprintf(g_fmt_cur, sizeof g_fmt_cur, "%s", format);
        g_last_ms = wp_now_ms();
        return WP_OK;
    }

    if (!g_fmt_cur[0] || g_uid_cur != hdr.uid || strcmp(format, g_fmt_cur) ||
            hdr.offset > g_offset_cur)
        return WP_ERR_OFFSET;
    g_offset_cur = hdr.offset;
    g_last_ms    = wp_now_ms();

    hex_decode(buf, content + (WP_HDR_LEN * 2), hdr.payload_len);

    stage_file(sfile, sizeof sfile, g_uid_cur, format);
    FILE *fp = fopen(sfile, "r+");
    if (!fp) {
        osif_log(OSIF_LOG_ERR, "wallpaper not saved: cannot open the staged file (%s)",
                strerror(errno));
        return wp_end(sfile, WP_ERR_FILE);
    }
    if (fseek(fp, hdr.offset, SEEK_SET) != 0 ||
            fwrite(buf, 1, hdr.payload_len, fp) != hdr.payload_len || fflush(fp) != 0) {
        osif_log(OSIF_LOG_ERR, "wallpaper not saved: cannot write the staged file (%s)",
                strerror(errno));
        fclose(fp);
        return wp_end(sfile, WP_ERR_FILE);
    }
    fsync(fileno(fp));

    if (hdr.file_size != (hdr.offset + hdr.payload_len)) {
        g_offset_cur = hdr.offset + hdr.payload_len;
        fclose(fp);
        return WP_OK;
    }

    uint8_t    whole[WP_SHA_LEN];
    sha256_ctx ctx;
    int        count;
    rewind(fp);
    sha256_init(&ctx);
    while ((count = (int)fread(buf, 1, sizeof buf, fp)) > 0)
        sha256_update(&ctx, buf, (size_t)count);
    sha256_final(&ctx, whole);
    fclose(fp);

    if (memcmp(whole, g_data_sha, WP_SHA_LEN) != 0) {
        osif_log(OSIF_LOG_ERR, "wallpaper refused: checksum mismatch");
        return wp_end(sfile, WP_ERR_SHA);
    }

    if (cfgwr_write_wallpaper(g_uid_cur, format) != 0) {
        osif_log(OSIF_LOG_ERR, "wallpaper not shown: mm_miner did not take it");
        return wp_end(sfile, WP_ERR_FILE);
    }
    osif_log(OSIF_LOG_INFO, "API wallpaper %s received: forwarded to mm_miner", format);
    *display = 1;
    return wp_end(NULL, WP_OK);
}

static void wp_reload_send(void)
{
    if (!g_ipc_core_mm)
        return;
    mc_ctrl_cmd cmd;
    if (ctrl_parse_cmd(CTRL_OP_WP_RELOAD, "", &cmd) != 0)
        return;
    if (osif_ipc_send(g_ipc_core_mm, MSG_CTRL_CMD, &cmd, (int)sizeof cmd) != OSIF_OK)
        osif_log(OSIF_LOG_WARN, "wallpaper reload request not sent to mm_miner");
}

static mc_set_result wallpaper_request(const char *raw_tail, char *msg, size_t msgcap)
{
    if (!*raw_tail) {
        snprintf(msg, msgcap, "missing wallpaper setting");
        return MC_SET_ERR_ARG;
    }
    size_t      flen      = strcspn(raw_tail, ",");
    int         has_comma = raw_tail[flen] == ',';
    const char *hex       = raw_tail + flen + has_comma;
    char        format[8] = { 0 };
    memcpy(format, raw_tail, flen < sizeof format ? flen : sizeof format - 1);
    if (has_comma == !strcmp(format, "none")) {
        snprintf(msg, msgcap, "input param error");
        return MC_SET_ERR_ARG;
    }

    if (strlen(hex) > WP_PKG_LEN) {
        snprintf(msg, msgcap, "input param len greater than %d", WP_PKG_LEN);
        return MC_SET_ERR_ARG;
    }

    if (strcmp(format, "png") && strcmp(format, "gif") && strcmp(format, "none")) {
        snprintf(msg, msgcap, "Input param cmd error.");
        return MC_SET_ERR_ARG;
    }

    int display = 0;
    int ret     = wallpaper_update(hex, format, &display);
    if (ret != WP_OK) {
        snprintf(msg, msgcap, "wallpaper recv failed %d", ret);
        return MC_SET_ERR_ARG;
    }

    if (display)
        wp_reload_send();
    snprintf(msg, msgcap, "wallpaper recv ok %zu", strlen(hex));
    return MC_SET_INFO;
}

static mc_set_result core_wallpaper_setter(const char *raw_tail, int asc, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)asc;
    (void)s;
    (void)ctx;

    pthread_mutex_lock(&g_wp_lock);
    wp_expire();
    mc_set_result r = wallpaper_request(raw_tail, msg, msgcap);
    pthread_mutex_unlock(&g_wp_lock);
    return r;
}

void core_wallpaper_tick(void)
{
    if (pthread_mutex_trylock(&g_wp_lock) != 0)
        return;
    wp_expire();
    pthread_mutex_unlock(&g_wp_lock);
}

int core_wallpaper_register(void)
{
    if (!ctrl_cmd_available(CTRL_OP_WP_RELOAD))
        return 0;
    if (mc_api_setter_register_raw("wallpaper", core_wallpaper_setter, NULL) != 0) {
        osif_log(OSIF_LOG_ERR, "API setting wallpaper unavailable");
        return 0;
    }
    return 1;
}
