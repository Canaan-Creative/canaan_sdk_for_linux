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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "osif.h"
#include "miner_ipc.h"
#include "mm_efuse.h"
#include "../../tests/include/tst_check.h"

#define TXT_PATH "/data/factory/efuse_info.txt"     /* efuse text file faked by fopen() */
#define TMP_PATH "/data/factory/efuse_info.txt.tmp" /* its temporary write path */
#define DIR_PATH "/data/factory"                    /* directory opened for the post-rename fsync */
#define DIR_FD   1000 /* fake descriptor open() returns for DIR_PATH */

osif_ipc_t           *g_ipc_asic_mm;            /* IPC channel to asic_miner; &tst_chan */
static int            tst_chan;                 /* address is the fake IPC handle */
static int            tst_linked;               /* nonzero reports the IPC channel connected */
static int            tst_sends;                /* accepted MSG_MM_EFUSE sends */
static int            tst_bad_sends;            /* rejected sends: wrong handle, type or length */
static int            tst_present_sends;        /* accepted sends with present set */
static int            tst_reads;                /* TXT_PATH opens for reading */
static mm_efuse_state tst_last;                 /* last accepted send payload */
static char          *tst_file;                 /* TXT_PATH contents; NULL absent */
static char          *tst_tmp;                  /* TMP_PATH contents being written */
static size_t         tst_tmp_len;              /* bytes in tst_tmp */
static int            tst_renames;              /* successful TMP_PATH to TXT_PATH renames */
static int            tst_removes;              /* remove(TMP_PATH) calls */
static int            tst_bin_logs;             /* "Chip bin" log lines */
static unsigned       tst_bin_logged;           /* bin value of the last "Chip bin" line */
static int            tst_fsync_fail;           /* nonzero fails file fsync() with EIO */
static int            tst_file_syncs;           /* successful file fsync() calls */
static int            tst_dir_syncs;            /* DIR_FD fsync() calls */
static int            tst_renames_at_file_sync; /* tst_renames at the last file fsync() */
static int            tst_renames_at_dir_sync;  /* tst_renames at the last DIR_FD fsync() */
static int            tst_write_fail;           /* TMP_PATH writes left to fail with ENOSPC */
static int            tst_rename_fail;          /* nonzero fails rename() with EISDIR */
static int            tst_errs;                 /* OSIF_LOG_ERR lines */
static int            tst_no_chip_logs;         /* "efuse sweep found no chips" lines */
static int            tst_write_fail_errs;      /* "Writing ... failed" error lines */
static int            tst_rename_fail_errs;     /* "Renaming ... failed" error lines */
static uint64_t       tst_now = 1000;           /* fake clock passed to mm_efuse_rule(), ms */

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    if (h != (osif_ipc_t *)&tst_chan || type != MSG_MM_EFUSE ||
            len != (int)sizeof(mm_efuse_state) || !data) {
        tst_bad_sends++;
        return OSIF_ERR;
    }
    mm_efuse_state st;
    memcpy(&st, data, sizeof st);
    if (st.present)
        tst_present_sends++;
    tst_last = st;
    tst_sends++;
    return OSIF_OK;
}

int osif_ipc_connected(osif_ipc_t *h)
{
    return h == (osif_ipc_t *)&tst_chan && tst_linked;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    if (lvl == OSIF_LOG_ERR) {
        tst_errs++;
        if (strncmp(fmt, "Writing %s failed", 17) == 0)
            tst_write_fail_errs++;
        if (strncmp(fmt, "Renaming %s failed", 18) == 0)
            tst_rename_fail_errs++;
    }
    if (lvl == OSIF_LOG_INFO && strstr(fmt, "efuse sweep found no chips"))
        tst_no_chip_logs++;
    if (strstr(fmt, "Chip bin: %u")) {
        va_list ap;
        va_start(ap, fmt);
        tst_bin_logged = va_arg(ap, unsigned);
        va_end(ap);
        tst_bin_logs++;
    }
}

static ssize_t tst_tmp_write(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    if (tst_write_fail) {
        tst_write_fail--;
        errno = ENOSPC;
        return -1;
    }
    char *p = realloc(tst_tmp, tst_tmp_len + n + 1);
    if (!p)
        return -1;
    memcpy(p + tst_tmp_len, buf, n);
    tst_tmp_len += n;
    p[tst_tmp_len] = 0;
    tst_tmp        = p;
    return (ssize_t)n;
}

FILE *fopen(const char *path, const char *mode)
{
    if (strcmp(path, TXT_PATH) == 0 && strcmp(mode, "re") == 0) {
        tst_reads++;
        if (tst_file)
            return fmemopen(tst_file, strlen(tst_file), "r");
    }
    /* "we": the tmp file stays open across a sweep and must not leak into children */
    if (strcmp(path, TMP_PATH) == 0 && strcmp(mode, "we") == 0) {
        free(tst_tmp);
        tst_tmp     = NULL;
        tst_tmp_len = 0;
        if (tst_write_fail) {
            cookie_io_functions_t io = { .write = tst_tmp_write };
            return fopencookie(NULL, "w", io);
        }
        return open_memstream(&tst_tmp, &tst_tmp_len);
    }
    errno = ENOENT;
    return NULL;
}

int rename(const char *from, const char *to)
{
    if (strcmp(from, TMP_PATH) != 0 || strcmp(to, TXT_PATH) != 0 || !tst_tmp)
        return -1;
    if (tst_rename_fail) {
        errno = EISDIR;
        return -1;
    }
    free(tst_file);
    tst_file    = tst_tmp;
    tst_tmp     = NULL;
    tst_tmp_len = 0;
    tst_renames++;
    return 0;
}

int remove(const char *path)
{
    if (strcmp(path, TMP_PATH) == 0)
        tst_removes++;
    errno = ENOENT;
    return -1;
}

int open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, DIR_PATH) == 0)
        return DIR_FD;
    errno = ENOENT;
    return -1;
}

int fsync(int fd)
{
    if (fd == DIR_FD) {
        tst_dir_syncs++;
        tst_renames_at_dir_sync = tst_renames;
        return 0;
    }
    if (tst_fsync_fail) {
        errno = EIO;
        return -1;
    }
    tst_file_syncs++;
    tst_renames_at_file_sync = tst_renames;
    return 0;
}

static void run_ticks(int n)
{
    tst_sends = 0;
    tst_reads = 0;
    for (int i = 0; i < n; i++) {
        mm_efuse_rule(NULL, tst_now);
        tst_now += 1000;
    }
}

static uint8_t ref_cal_bin(uint8_t bin, uint8_t bin_backup1, uint8_t bin_backup2)
{
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = (uint8_t)(1u << i);
        int     sum  = ((bin & mask) ? 1 : 0) + ((bin_backup1 & mask) ? 1 : 0) +
                ((bin_backup2 & mask) ? 1 : 0);
        if (sum >= 2)
            result |= mask;
    }
    return result;
}

static void words_for(uint32_t *w, uint8_t w11_lo, uint8_t w10_lo, uint8_t w10_hi)
{
    for (int i = 0; i < ASIC_EFUSE_WORDS; i++)
        w[i] = 0xa5a5a5a5u ^ (uint32_t)i;
    w[10] = 0xee000000u | 0x00dd0000u | ((uint32_t)w10_hi << 8) | w10_lo;
    w[11] = 0xcccccc00u | w11_lo;
}

static void ingest(uint32_t seq, uint16_t board, uint16_t chip, int last, const uint32_t *w)
{
    asic_efuse_rec r;
    memset(&r, 0, sizeof r);
    r.seq   = seq;
    r.board = board;
    r.chip  = chip;
    r.last  = (uint8_t)last;
    if (w)
        memcpy(r.data, w, sizeof r.data);
    mm_efuse_ingest(&r, (int)sizeof r);
}

static void set_file(uint8_t w11_lo, uint8_t w10_lo, uint8_t w10_hi)
{
    uint32_t w[ASIC_EFUSE_WORDS];
    words_for(w, w11_lo, w10_lo, w10_hi);
    char buf[512];
    int  n = snprintf(buf, sizeof buf, "0-0: 0x");
    for (int i = 0; i < ASIC_EFUSE_WORDS; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%08x", w[i]);
    snprintf(buf + n, sizeof buf - (size_t)n, "\n0-1: 0x%0128d\n", 0);
    free(tst_file);
    tst_file = strdup(buf);
    mm_efuse_invalidate();
}

static void set_raw(const char *b91, const char *b93, const char *b101)
{
    char buf[7 + 8 * ASIC_EFUSE_WORDS + 2];
    memset(buf, 'z', sizeof buf - 2);
    buf[sizeof buf - 2] = '\n';
    buf[sizeof buf - 1] = '\0';
    memcpy(buf + 91, b91, 2);
    memcpy(buf + 93, b93, 2);
    memcpy(buf + 101, b101, 2);
    free(tst_file);
    tst_file = strdup(buf);
    mm_efuse_invalidate();
}

static mm_efuse_state rule_state(void)
{
    tst_linked = 1;
    mm_efuse_asic_spawned();
    run_ticks(1);
    return tst_last;
}

static uint8_t rule_bin(void)
{
    tst_linked = 1;
    mm_efuse_asic_spawned();
    tst_present_sends = 0;
    run_ticks(1);
    CHECK(tst_sends == 1 && tst_present_sends == 1, "present file is read and sent");
    return tst_last.bin_grade;
}

static void boot_file_present(void)
{
    set_file(0x0f, 0x33, 0x55);
    tst_linked        = 1;
    tst_present_sends = 0;
    run_ticks(1);
    CHECK(tst_sends == 1 && tst_last.present == 1 && tst_last.bin_grade == 0x17,
            "file at boot: sent present with the file's bin (0x%02x)", tst_last.bin_grade);
    CHECK(tst_bin_logs == 1 && tst_bin_logged == 0x17,
            "file at boot: its bin is logged on the first read (%u)", tst_bin_logged);
    run_ticks(15);
    CHECK(tst_sends == 9 && tst_present_sends == 10,
            "every send in the window reports the file present (%d/%d)", tst_present_sends,
            tst_sends);
    CHECK(tst_bin_logs == 1, "file at boot: bin logged once (%d)", tst_bin_logs);
    CHECK(tst_removes == 0 && tst_renames == 0, "file at boot: nothing removed or written");
}

static void file_cached(void)
{
    set_file(0x0f, 0x33, 0x55);
    tst_linked = 1;
    run_ticks(10);
    CHECK(tst_sends == 10 && tst_reads == 1, "a send window reads the file once (%d reads)",
            tst_reads);
    free(tst_file);
    tst_file = NULL;
    mm_efuse_asic_spawned();
    run_ticks(1);
    CHECK(tst_reads == 0 && tst_last.present == 1,
            "a file removed behind the cache is still reported present");
    mm_efuse_invalidate();
    mm_efuse_asic_spawned();
    run_ticks(1);
    CHECK(tst_reads == 1 && tst_last.present == 0,
            "after an invalidation the removed file is read again and sent absent");
}

static void boot_file_absent(void)
{
    uint32_t w[ASIC_EFUSE_WORDS];

    tst_linked = 1;
    run_ticks(1);
    CHECK(tst_sends == 1 && tst_last.present == 0 && tst_bin_logs == 0,
            "no file at boot: sent absent, no bin logged (%d)", tst_bin_logs);
    CHECK(tst_removes == 1, "no file and no scan: stale tmp is removed");
    words_for(w, 0x24, 0x24, 0x24);
    ingest(0, 0, 0, 0, w);
    CHECK(tst_bin_logs == 1 && tst_bin_logged == 0x24,
            "scan: bin logged from the live record 0 (%u)", tst_bin_logged);
    ingest(1, 0, 0, 1, NULL);
    CHECK(tst_renames == 1 && tst_file != NULL, "scan complete: the file is written");
    mm_efuse_asic_spawned();
    run_ticks(1);
    CHECK(tst_sends == 1 && tst_last.present == 1 && tst_last.bin_grade == 0x24,
            "after the scan: the written file is sent present (0x%02x)", tst_last.bin_grade);
    CHECK(tst_reads == 1, "after the scan: the written file is read once (%d)", tst_reads);
    run_ticks(5);
    CHECK(tst_sends == 5 && tst_reads == 0,
            "after the scan: later sends use the cached state (%d reads)", tst_reads);
    CHECK(tst_bin_logs == 1, "after the scan: bin not logged again (%d)", tst_bin_logs);
}

static void empty_sweep(void)
{
    uint32_t w[ASIC_EFUSE_WORDS];

    ingest(0, 0, 0, 1, NULL);
    CHECK(tst_errs == 0, "no-chip sweep: no ERR logged (%d)", tst_errs);
    CHECK(tst_no_chip_logs == 1, "no-chip sweep: one INFO line (%d)", tst_no_chip_logs);
    CHECK(tst_renames == 0 && tst_file == NULL && tst_tmp == NULL,
            "no-chip sweep: no tmp opened, no file written");
    words_for(w, 0x24, 0x24, 0x24);
    ingest(0, 0, 0, 0, w);
    ingest(1, 0, 0, 1, NULL);
    CHECK(tst_errs == 0 && tst_renames == 1 && tst_file != NULL,
            "after a no-chip sweep: the next sweep writes the file");
}

static void wrong_size_record(void)
{
    static uint8_t big[sizeof(asic_efuse_rec) + 4];
    asic_efuse_rec r;

    memset(&r, 0, sizeof r);
    r.last = 1;
    memcpy(big, &r, sizeof r);
    mm_efuse_ingest(big, (int)sizeof big);
    mm_efuse_ingest(&r, (int)sizeof r - 1);
    CHECK(tst_errs == 2 && tst_no_chip_logs == 0,
            "a long or short record is dropped at ERR (%d), not applied (%d)", tst_errs,
            tst_no_chip_logs);
}

static void rename_failure(void)
{
    uint32_t w[ASIC_EFUSE_WORDS];

    words_for(w, 0x24, 0x24, 0x24);
    tst_rename_fail = 1;
    ingest(0, 0, 0, 0, w);
    ingest(1, 0, 0, 1, NULL);
    CHECK(tst_renames == 0 && tst_file == NULL && tst_dir_syncs == 0,
            "rename failure: no file, no directory fsync");
    CHECK(tst_removes == 1, "rename failure: tmp removed (%d)", tst_removes);
    CHECK(tst_rename_fail_errs == 1 && tst_errs == 1, "rename failure: one ERR line (%d)",
            tst_rename_fail_errs);
}

static void run_child(void (*fn)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        fflush(NULL);
        _exit(fails ? 1 : 0);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0,
            "child case passed");
}

int main(void)
{
    g_ipc_asic_mm = (osif_ipc_t *)&tst_chan;

    run_child(boot_file_present);
    run_child(boot_file_absent);
    run_child(file_cached);
    run_child(empty_sweep);
    run_child(wrong_size_record);
    run_child(rename_failure);

    tst_linked = 0;
    run_ticks(1);
    CHECK(tst_reads == 1 && tst_sends == 0, "first tick reads once, link down sends nothing");
    run_ticks(30);
    CHECK(tst_reads == 0 && tst_sends == 0, "link down: no file access, no send");

    tst_linked = 1;
    run_ticks(10);
    CHECK(tst_reads == 0 && tst_sends == 10,
            "link up: one send per tick for 10 s from the cached state (%d/%d)", tst_reads,
            tst_sends);
    run_ticks(60);
    CHECK(tst_reads == 0 && tst_sends == 0, "window over: no file access, no send");

    tst_linked = 0;
    run_ticks(1);
    CHECK(tst_reads == 0 && tst_sends == 0, "link drop: nothing");
    tst_linked = 1;
    run_ticks(12);
    CHECK(tst_sends == 10 && tst_reads == 0, "reconnect restarts the window (%d sends)", tst_sends);

    mm_efuse_asic_spawned();
    run_ticks(12);
    CHECK(tst_sends == 10 && tst_reads == 0,
            "respawn with the link never seen down restarts the window (%d sends)", tst_sends);

    mm_efuse_asic_spawned();
    tst_linked = 0;
    run_ticks(3);
    CHECK(tst_sends == 0 && tst_reads == 0, "spawned, not yet linked: nothing");
    tst_linked = 1;
    run_ticks(12);
    CHECK(tst_sends == 10, "spawned then linked: window starts at link-up (%d sends)", tst_sends);

    CHECK(tst_bad_sends == 0 && tst_present_sends == 0,
            "every send is MSG_MM_EFUSE with an absent-file state");
    CHECK(tst_removes > 0, "absent file and no sweep: stale tmp is removed");

    int mismatch = 0;
    for (unsigned v = 0; v < (1u << 24); v++) {
        uint8_t a = (uint8_t)v, b = (uint8_t)(v >> 8), c = (uint8_t)(v >> 16);
        if (efuse_bin_majority(a, b, c) != ref_cal_bin(a, b, c))
            mismatch++;
    }
    CHECK(mismatch == 0,
            "efuse_bin_majority equals the per-bit 2-of-3 vote on every input (%d off)", mismatch);
    CHECK(efuse_bin_majority(0x5a, 0x5a, 0x5a) == 0x5a, "three equal bytes: that byte");
    CHECK(efuse_bin_majority(0x5a, 0x5a, 0x81) == 0x5a &&
                    efuse_bin_majority(0x5a, 0x81, 0x5a) == 0x5a &&
                    efuse_bin_majority(0x81, 0x5a, 0x5a) == 0x5a,
            "one byte differs: the other two win");
    CHECK(efuse_bin_majority(0x0f, 0x33, 0x55) == 0x17, "all three differ: per-bit majority 0x17");

    uint32_t w[ASIC_EFUSE_WORDS];
    words_for(w, 0x0f, 0x33, 0x55);
    CHECK(efuse_bin_grade(w) == 0x17, "efuse_bin_grade votes w[11][7:0], w[10][7:0], w[10][15:8]");
    words_for(w, 0x24, 0x24, 0x24);
    CHECK(efuse_bin_grade(w) == 0x24, "efuse_bin_grade ignores the other bytes of w[10] and w[11]");

    words_for(w, 0x0f, 0x33, 0x55);
    tst_bin_logs = 0;
    tst_removes  = 0;
    ingest(0, 0, 0, 0, w);
    CHECK(tst_bin_logs == 1 && tst_bin_logged == 0x17, "first record logs the majority bin (%u)",
            tst_bin_logged);
    tst_linked = 0;
    run_ticks(1);
    mm_efuse_asic_spawned();
    tst_linked = 1;
    run_ticks(1);
    CHECK(tst_removes == 0, "sweep in progress: tmp is not removed");
    uint32_t w1[ASIC_EFUSE_WORDS];
    words_for(w1, 0xff, 0xff, 0xff);
    ingest(1, 0, 1, 0, w1);
    ingest(2, 0, 0, 1, NULL);
    CHECK(tst_renames == 1 && tst_file != NULL, "complete sweep renames tmp to txt");
    CHECK(tst_file_syncs == 1 && tst_renames_at_file_sync == 0, "tmp is fsynced before the rename");
    CHECK(tst_dir_syncs == 1 && tst_renames_at_dir_sync == 1,
            "directory is fsynced after the rename");
    CHECK(tst_file && strlen(tst_file) == 2 * (7 + 8 * ASIC_EFUSE_WORDS + 1) &&
                    strncmp(tst_file + 7 + 8 * ASIC_EFUSE_WORDS + 1, "0-1: 0x", 7) == 0,
            "txt holds both records in full");
    CHECK(tst_file && strncmp(tst_file, "0-0: 0x", 7) == 0 &&
                    strncmp(tst_file + 91, "55", 2) == 0 && strncmp(tst_file + 93, "33", 2) == 0 &&
                    strncmp(tst_file + 101, "0f", 2) == 0,
            "file offsets 91/93/101 hold w[10][15:8], w[10][7:0], w[11][7:0]");
    CHECK(rule_bin() == 0x17, "written file reads back as the majority bin");

    set_file(0x5a, 0x5a, 0x5a);
    CHECK(rule_bin() == 0x5a, "file: three equal bytes -> 0x5a");
    set_file(0x5a, 0x5a, 0x81);
    CHECK(rule_bin() == 0x5a, "file: w[10][15:8] differs -> 0x5a");
    set_file(0x81, 0x5a, 0x5a);
    CHECK(rule_bin() == 0x5a, "file: w[11][7:0] differs -> 0x5a");
    set_file(0x5a, 0x81, 0x5a);
    CHECK(rule_bin() == 0x5a, "file: w[10][7:0] differs -> 0x5a");
    set_file(0x0f, 0x33, 0x55);
    CHECK(rule_bin() == 0x17, "file: all three differ -> per-bit majority 0x17");
    set_raw("55", "33", "0f");
    CHECK(rule_bin() == 0x17,
            "only bytes 91/93/101 are read: everything else is non-hex and the vote is 0x17");
    CHECK(tst_bin_logs == 1, "core_bin is logged once");

    char *before   = strdup(tst_file);
    tst_fsync_fail = 1;
    tst_removes    = 0;
    tst_dir_syncs  = 0;
    ingest(0, 0, 0, 0, w);
    ingest(1, 0, 0, 1, NULL);
    CHECK(tst_renames == 1 && tst_dir_syncs == 0 && tst_removes == 1,
            "tmp fsync failure: no rename, tmp removed");
    CHECK(tst_write_fail_errs == 1, "tmp fsync failure: logged at ERR (%d)", tst_write_fail_errs);
    CHECK(strcmp(tst_file, before) == 0, "tmp fsync failure: txt is unchanged");
    tst_fsync_fail = 0;
    free(before);

    before         = strdup(tst_file);
    int renames    = tst_renames;
    tst_write_fail = 1;
    tst_removes    = 0;
    tst_dir_syncs  = 0;
    ingest(0, 0, 0, 0, w);
    ingest(1, 0, 1, 0, w1);
    ingest(2, 0, 0, 1, NULL);
    CHECK(tst_write_fail == 0, "the buffered record write failed (ENOSPC)");
    CHECK(tst_renames == renames && tst_dir_syncs == 0 && tst_removes == 1,
            "record write failure: no rename, tmp removed");
    CHECK(strcmp(tst_file, before) == 0, "record write failure: txt is unchanged, not truncated");
    CHECK(tst_write_fail_errs == 2, "record write failure: logged at ERR (%d)",
            tst_write_fail_errs);
    free(before);

    mm_efuse_state st;
    free(tst_file);
    tst_file = strdup("0-0: 0x0123\n");
    mm_efuse_invalidate();
    st = rule_state();
    CHECK(st.present == 1 && st.bin_grade == 0,
            "a file too short for the three bytes: present, unread bytes vote as 0");
    set_raw("zz", "zz", "zz");
    st = rule_state();
    CHECK(st.present == 1 && st.bin_grade == 0,
            "non-hex bytes at 91/93/101: present, unread bytes vote as 0");

    free(tst_file);
    free(tst_tmp);
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
