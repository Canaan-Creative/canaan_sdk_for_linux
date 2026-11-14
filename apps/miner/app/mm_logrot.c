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

#include "mm_logrot.h"
#include "osif.h"
#include "osif_logpath.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MINER_LOG_CAP (4u * 1024u * 1024u) /* miner.log rotation cap, bytes */
#define AUX_LOG_CAP   (2u * 1024u * 1024u) /* web.log, miner_op.log rotation cap, bytes */

/* Rotated logs; read-only, scanned by logrot_rule() on the mm_miner supervisor loop */
static const struct {
    const char *live;
    const char *gen1; /* previous generation, "<live>.1" */
    const char *tmp;  /* staging copy, "<live>.1.tmp", renamed onto gen1 */
    unsigned    cap;  /* rotate above this, bytes; if rotation fails, truncate above 2 * cap */
} g_logs[] = {
    { MINER_LOG_PATH, MINER_LOG_PATH ".1", MINER_LOG_PATH ".1.tmp", MINER_LOG_CAP },
    { MINER_WEB_LOG_PATH, MINER_WEB_LOG_PATH ".1", MINER_WEB_LOG_PATH ".1.tmp", AUX_LOG_CAP },
    { MINER_OP_LOG_PATH, MINER_OP_LOG_PATH ".1", MINER_OP_LOG_PATH ".1.tmp", AUX_LOG_CAP },
};

static int copy_whole(const char *from, const char *to)
{
    FILE *src = fopen(from, "rbe");
    if (!src)
        return -1;
    FILE *dst = fopen(to, "wbe");
    if (!dst) {
        fclose(src);
        return -1;
    }
    char   buf[65536];
    size_t n;
    int    rc = 0;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(src))
        rc = -1;
    if (fclose(dst) != 0)
        rc = -1;
    fclose(src);
    return rc;
}

static void rotate_one(const char *live, const char *gen1, const char *tmp, unsigned cap)
{
    struct stat st;
    if (stat(live, &st) != 0 || st.st_size <= (off_t)cap)
        return;
    const char *name = strrchr(live, '/');
    name             = name ? name + 1 : live;
    if (copy_whole(live, tmp) != 0 || rename(tmp, gen1) != 0) {
        unlink(tmp);
        if (st.st_size <= (off_t)(2u * (off_t)cap)) {
            osif_log(OSIF_LOG_WARN, "%s not rotated: copy to .1 failed, log kept", name);
            return;
        }
        if (truncate(live, 0) == 0)
            osif_log(OSIF_LOG_ERR,
                    "%s truncated without rotation: copy to .1 "
                    "failed and the log exceeded %u MB, its contents are lost",
                    name, (2u * cap) >> 20);
        else
            osif_log(OSIF_LOG_ERR,
                    "%s exceeds %u MB and can be neither rotated "
                    "nor truncated",
                    name, (2u * cap) >> 20);
        return;
    }
    if (truncate(live, 0) == 0)
        osif_log(OSIF_LOG_INFO, "%s rotated in-run (over %u MB, previous content kept in .1)", name,
                cap >> 20);
}

void logrot_rule(void *ctx, uint64_t now_ms)
{
    (void)ctx;
    (void)now_ms;
    for (size_t i = 0; i < sizeof g_logs / sizeof g_logs[0]; i++)
        rotate_one(g_logs[i].live, g_logs[i].gen1, g_logs[i].tmp, g_logs[i].cap);
}
