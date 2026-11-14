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

#include "mm_efuse.h"
#include "fsync_dir.h"
#include "mc_paths.h"
#include "miner_ipc.h"
#include "mm_internal.h"
#include "osif.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MM_EFUSE_TMP_PATH       MM_EFUSE_FILE_PATH ".tmp" /* efuse file being written */
#define MM_EFUSE_SEND_WINDOW_MS 10000u    /* efuse state send window after link, ms */
#define MM_EFUSE_LINE0_HEAD     "0-0: 0x" /* first line prefix of the efuse file */
#define MM_EFUSE_WORD_HEXLEN    (2 * (long)sizeof(uint32_t)) /* hex digits per efuse word, count */

/* efuse byte file offset, bytes */
#define MM_EFUSE_HEX_OFF(word, byte) \
    ((long)sizeof(MM_EFUSE_LINE0_HEAD) - 1 + (word) * MM_EFUSE_WORD_HEXLEN + \
            MM_EFUSE_WORD_HEXLEN - 2 * ((byte) + 1))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE           *g_f;      /* efuse file being written */
static uint32_t        g_next;   /* next expected record, index */
static int             g_broken; /* scan failed flag */
static int             g_bin_logged;
static int             g_linked;      /* asic_miner IPC connected flag */
static int             g_ticked;      /* rule ran once flag */
static uint64_t        g_send_until;  /* end of the send window, ms */
static mm_efuse_state  g_state;       /* parsed efuse file */
static int             g_state_valid; /* g_state matches the efuse file flag */

void mm_efuse_asic_spawned(void)
{
    g_linked = 0;
}

void mm_efuse_invalidate(void)
{
    pthread_mutex_lock(&g_lock);
    g_state_valid = 0;
    pthread_mutex_unlock(&g_lock);
}

static void efuse_bin_log(uint8_t bin)
{
    if (g_bin_logged)
        return;
    g_bin_logged = 1;
    osif_log(OSIF_LOG_INFO, "Chip bin: %u", bin);
}

static void efuse_state_read(mm_efuse_state *st)
{
    memset(st, 0, sizeof *st);
    FILE *f = fopen(MM_EFUSE_FILE_PATH, "re");
    if (!f)
        return;
    st->present              = 1;
    static const long off[3] = { MM_EFUSE_HEX_OFF(10, 1), MM_EFUSE_HEX_OFF(10, 0),
        MM_EFUSE_HEX_OFF(11, 0) };
    uint8_t           b[3]   = { 0 };
    for (int i = 0; i < 3; i++) {
        if (fseek(f, off[i], SEEK_SET) != 0)
            continue;
        char hex[3] = { 0 };
        if (fread(hex, 1, 2, f) == 2) {
            unsigned int v;
            if (sscanf(hex, "%x", &v) == 1)
                b[i] = (uint8_t)v;
        }
    }
    fclose(f);
    st->bin_grade = efuse_bin_majority(b[0], b[1], b[2]);
}

void mm_efuse_rule(void *ctx, uint64_t now_ms)
{
    (void)ctx;
    int linked = osif_ipc_connected(g_ipc_asic_mm);
    if (linked && !g_linked)
        g_send_until = now_ms + MM_EFUSE_SEND_WINDOW_MS;
    g_linked = linked;

    int send = linked && now_ms < g_send_until;
    if (!send && g_ticked)
        return;
    g_ticked = 1;

    mm_efuse_state st;
    pthread_mutex_lock(&g_lock);
    if (!g_state_valid) {
        efuse_state_read(&g_state);
        g_state_valid = 1;
    }
    st = g_state;
    if (st.present)
        efuse_bin_log(st.bin_grade);
    else if (!g_f)
        remove(MM_EFUSE_TMP_PATH);
    pthread_mutex_unlock(&g_lock);
    if (send)
        osif_ipc_send(g_ipc_asic_mm, MSG_MM_EFUSE, &st, (int)sizeof st);
}

static void efuse_rec_apply(const asic_efuse_rec *r)
{
    if (r->last && r->seq == 0 && !g_f) {
        osif_log(OSIF_LOG_INFO, "efuse sweep found no chips; efuse file not generated");
        return;
    }

    if (!r->last && r->seq == 0) {
        if (g_f)
            fclose(g_f);
        g_next   = 0;
        g_broken = 0;
        g_f      = fopen(MM_EFUSE_TMP_PATH, "we");
        if (!g_f) {
            g_broken = 1;
            osif_log(OSIF_LOG_ERR, "efuse records not saved: cannot open the efuse file");
            return;
        }
        osif_log(OSIF_LOG_INFO, "Reading efuse data");
        efuse_bin_log(efuse_bin_grade(r->data));
    }

    if (!g_f) {
        if (!g_broken) {
            g_broken = 1;
            osif_log(OSIF_LOG_ERR,
                    "efuse record %u arrived with no scan "
                    "in progress; efuse file not generated",
                    r->seq);
        }
        return;
    }

    if (r->seq != g_next) {
        fclose(g_f);
        g_f      = NULL;
        g_broken = 1;
        osif_log(OSIF_LOG_ERR,
                "efuse record out of order (got %u, "
                "expected %u), a record was lost; efuse file not generated",
                r->seq, g_next);
        return;
    }

    if (!r->last) {
        fprintf(g_f, "%u-%u: 0x", (unsigned)r->board, (unsigned)r->chip);
        for (int w = 0; w < ASIC_EFUSE_WORDS; w++)
            fprintf(g_f, "%08x", r->data[w]);
        fprintf(g_f, "\n");
        g_next++;
        return;
    }

    int ok = fflush(g_f) == 0 && !ferror(g_f) && fsync(fileno(g_f)) == 0;
    if (fclose(g_f) != 0)
        ok = 0;
    g_f = NULL;
    if (!ok) {
        remove(MM_EFUSE_TMP_PATH);
        osif_log(OSIF_LOG_ERR,
                "Writing %s failed (%u records); %s not "
                "generated",
                MM_EFUSE_TMP_PATH, g_next, MM_EFUSE_FILE_PATH);
        return;
    }
    if (rename(MM_EFUSE_TMP_PATH, MM_EFUSE_FILE_PATH) != 0) {
        remove(MM_EFUSE_TMP_PATH);
        osif_log(OSIF_LOG_ERR,
                "Renaming %s failed (%u records); %s not "
                "generated",
                MM_EFUSE_TMP_PATH, g_next, MM_EFUSE_FILE_PATH);
        return;
    }
    g_state_valid = 0;
    fsync_parent_dir(MM_EFUSE_FILE_PATH);
    osif_log(OSIF_LOG_DBG, "efuse file saved (%u records)", g_next);
}

void mm_efuse_ingest(const void *data, int len)
{
    if (!data || len != (int)sizeof(asic_efuse_rec)) {
        osif_log(OSIF_LOG_ERR,
                "efuse record dropped: %d bytes, %d "
                "expected (asic_miner from another build?); efuse file not generated",
                len, (int)sizeof(asic_efuse_rec));
        return;
    }

    asic_efuse_rec r;
    memcpy(&r, data, sizeof r);

    pthread_mutex_lock(&g_lock);
    efuse_rec_apply(&r);
    pthread_mutex_unlock(&g_lock);
}
