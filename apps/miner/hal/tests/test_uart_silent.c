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

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "osif.h"
#include "boardconf.h"
#include "toast_port.h"
#include "uart_pro.h"
#include "tst_uart.h"
#include "tst_board_expect.h"

#define open tst_uart_open /* routes uart_pro.c UART opens to the test nodes */
#include "../asic/a3197s/uart_pro.c"
#undef open

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            printf("  FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } else { \
            printf("  ok: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)
#define FRAME_LEN     15 /* length of a one-word reply frame, bytes */
#define CMD_CRC_BYTE  9  /* offset of the command frame CRC byte in a reply */
#define DATA_CRC_BYTE 14 /* offset of the data CRC byte in a one-word reply */
/* failing reads per burst, three times the logged repeats */
#define BURST (3 * UART_SILENT_LOG_MAX)

static int fails;
/* counts of the "no reply to", "nonce read failed" and "silent read" logs */
static int n_selto, n_nonce_err, n_recover;
static int n_resync, n_resync_recover; /* counts of the "out of sync" and "framing in sync" logs */
/* suppressed count of the last "silent read" / "framing in sync" log */
static unsigned last_suppressed, last_resync_suppressed;
static int      pty_master[CHANNELS]; /* pty master behind each ASIC UART channel */

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    char    msg[512];
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (strstr(msg, "no reply to"))
        n_selto++;
    if (strstr(msg, "nonce read failed"))
        n_nonce_err++;
    if (strstr(msg, "silent read")) {
        n_recover++;
        const char *p   = strrchr(msg, '(');
        last_suppressed = p ? (unsigned)strtoul(p + 1, NULL, 10) : 0u;
    }
    if (strstr(msg, "out of sync"))
        n_resync++;
    if (strstr(msg, "framing in sync")) {
        n_resync_recover++;
        const char *p          = strrchr(msg, '(');
        last_resync_suppressed = p ? (unsigned)strtoul(p + 1, NULL, 10) : 0u;
    }
}

static int pty_bind(int ch)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m))
        return -1;
    const char *slave = ptsname(m);
    if (!slave)
        return -1;
    char link[256];
    snprintf(link, sizeof link, "%s/uart%d", TST_UART_ROOT, ch);
    unlink(link);
    if (symlink(slave, link) != 0)
        return -1;
    pty_master[ch] = m;
    uart_init((uint8_t)ch, UART_DEFAULT_BAUD_RATE);
    return 0;
}

static void drain_master(int ch)
{
    char drain[256];
    int  fl = fcntl(pty_master[ch], F_GETFL);
    fcntl(pty_master[ch], F_SETFL, fl | O_NONBLOCK);
    while (read(pty_master[ch], drain, sizeof drain) > 0) {
    }
    fcntl(pty_master[ch], F_SETFL, fl);
}

static int silent_read(int ch)
{
    uint32_t v = 0;
    return uart_read_fifo((uint8_t)ch, 0x42c, &v, 1);
}

static void silent_reads(int ch, int n)
{
    for (int i = 0; i < n; i++)
        (void)silent_read(ch);
}

static void noisy_read(int ch)
{
    static const char junk[] = "abcdefghijkl\n";
    ssize_t           w      = write(pty_master[ch], junk, sizeof junk - 1);
    (void)w;
    (void)silent_read(ch);
    drain_master(ch);
}

static int nonce_poll(int ch)
{
    struct toast_nonce nonce[TOAST_NONCE_COUNT];
    uint32_t           pvmask[8] = { 0 };
    uint8_t            cnt       = 0;
    int                ret       = uart_get_nonce((uint8_t)ch, nonce, &cnt, pvmask);
    drain_master(ch);
    return ret;
}

static int capture_frame(int ch, uint8_t *out)
{
    uint32_t word = 0x44332211u;
    drain_master(ch);
    uart_write_fifo((uint8_t)ch, 0, &word, 1, UART_INIT_MODE);
    int n = 0;
    while (n < FRAME_LEN) {
        ssize_t r = read(pty_master[ch], out + n, (size_t)(FRAME_LEN - n));
        if (r <= 0)
            break;
        n += (int)r;
    }
    return n;
}

static int reply_read(int ch, const uint8_t *frame, int n)
{
    uart_clear_rxfifo((uint8_t)ch);
    ssize_t w = write(pty_master[ch], frame, (size_t)n);
    (void)w;
    uint32_t v = 0;
    return uart_read_fifo((uint8_t)ch, 0x42c, &v, 1);
}

static int offset_reply_read(int ch, const uint8_t *frame, int n, int lead)
{
    uint8_t buf[64];
    if (lead < 0 || (size_t)(lead + n) > sizeof buf)
        return -1;
    memset(buf, 0xa5, (size_t)lead);
    memcpy(buf + lead, frame, (size_t)n);

    uart_clear_rxfifo((uint8_t)ch);
    ssize_t w = write(pty_master[ch], buf, (size_t)(lead + n));
    (void)w;
    uint32_t v = 0;
    return uart_read_fifo((uint8_t)ch, 0x42c, &v, 1);
}

int main(void)
{
    CHECK(CHANNELS == EXPECT_CHANNELS, "%s exposes %d ASIC UART channel(s)", BOARD_NAME, CHANNELS);

    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", TST_UART_ROOT);
        if (system(cmd) != 0 || mkdir(TST_UART_ROOT, 0755) != 0) {
            printf("  FAIL: could not build the uart fixture dir %s\n", TST_UART_ROOT);
            return 1;
        }
    }

    if (pty_bind(0) != 0) {
        printf("  FAIL: could not bind a pty to channel 0\n");
        return 1;
    }

    silent_reads(0, BURST);
    CHECK(n_selto == UART_SILENT_LOG_MAX,
            "%d silent reads printed %d `no reply to` lines, expected the cap %d", BURST, n_selto,
            (int)UART_SILENT_LOG_MAX);

    {
        int before = n_selto;
        silent_reads(0, BURST);
        CHECK(n_selto == before,
                "another %d silent reads printed %d more lines; once capped a "
                "channel must stay quiet until it answers",
                BURST, n_selto - before);
    }

    {
        int selto_before = n_selto;
        noisy_read(0);
        CHECK(n_recover == 1, "the recovery line was printed %d times, expected 1", n_recover);
        CHECK(last_suppressed == (unsigned)(2 * BURST - UART_SILENT_LOG_MAX),
                "recovery reported %u suppressed lines, expected %u", last_suppressed,
                (unsigned)(2 * BURST - UART_SILENT_LOG_MAX));
        CHECK(n_selto == selto_before + 1,
                "the read that received bytes then went silent printed %d lines, "
                "expected 1 — the gate did not reopen",
                n_selto - selto_before);
    }

    {
        int before = n_selto;
        silent_reads(0, 1);
        CHECK(n_selto == before + 1, "the next silent read printed %d lines, expected 1",
                n_selto - before);
    }

    {
        int selto_before = n_selto, nonce_before = n_nonce_err;
        for (int i = 0; i < BURST; i++)
            (void)nonce_poll(0);
        CHECK(n_nonce_err - nonce_before <= UART_SILENT_LOG_MAX,
                "%d silent nonce polls printed %d `nonce read failed` lines, cap is %d", BURST,
                n_nonce_err - nonce_before, (int)UART_SILENT_LOG_MAX);
        CHECK(n_nonce_err - nonce_before >= 1,
                "no `nonce read failed` line at all — the first failures must still "
                "be reported, the cap only bounds the repeat");
        CHECK(n_selto - selto_before <= UART_SILENT_LOG_MAX,
                "the nonce polls' own `no reply to` lines were %d, cap is %d",
                n_selto - selto_before, (int)UART_SILENT_LOG_MAX);
    }

    if (CHANNELS > 1) {
        if (pty_bind(1) != 0) {
            printf("  FAIL: could not bind a pty to channel 1\n");
            return 1;
        }
        int before = n_selto;
        silent_reads(1, UART_SILENT_LOG_MAX);
        CHECK(n_selto - before == UART_SILENT_LOG_MAX,
                "a live channel printed %d of its %d lines while channel 0 was "
                "capped — the run is shared, not per channel",
                n_selto - before, (int)UART_SILENT_LOG_MAX);
        before = n_selto;
        silent_reads(1, BURST);
        CHECK(n_selto == before,
                "channel 1 printed %d more lines past its own cap — it is counted "
                "on another channel's run",
                n_selto - before);
    } else {
        int before = n_selto;
        silent_reads(1, UART_SILENT_LOG_MAX);
        CHECK(n_selto == before,
                "channel 1 printed %d lines on a single-channel product; it is "
                "channel 0, which is already capped",
                n_selto - before);
    }

    uart_clear_errcnt(0);
    CHECK(uart_get_errcnt(0) == 0, "uart_clear_errcnt zeroes the tally");

    silent_reads(0, 3);
    CHECK(uart_get_errcnt(0) == 3, "3 timed-out reads count 3 (got %u) — and none of them printed",
            uart_get_errcnt(0));

    int nonce_rc = nonce_poll(0);
    CHECK(nonce_rc != 0, "a silent-bus nonce poll reports a failure (got %d, want nonzero)",
            nonce_rc);
    CHECK(uart_get_errcnt(0) == 4,
            "a failed nonce poll counts ONCE (got %u, want 4): the counter "
            "counts failed READS, and its transport-level read is not wrapped",
            uart_get_errcnt(0));

    noisy_read(0);
    CHECK(uart_get_errcnt(0) == 5,
            "a read that RECEIVES bytes but cannot parse them counts once too "
            "(got %u, want 5)",
            uart_get_errcnt(0));

    {
        uint8_t frame[FRAME_LEN], bad[FRAME_LEN];
        int     n = capture_frame(0, frame);
        CHECK(n == FRAME_LEN,
                "captured a %d-byte reply off the transport's OWN frame builder, "
                "so both CRCs come from the shipped uart_get_crc8 and no constant "
                "is transcribed here (got %d)",
                FRAME_LEN, n);

        uint32_t base = uart_get_errcnt(0);
        CHECK(reply_read(0, frame, FRAME_LEN) == 0,
                "the unmodified frame PARSES — the fixture is a valid reply, so "
                "the two failures below are caused by the flipped CRC and by "
                "nothing else");
        CHECK(uart_get_errcnt(0) == base,
                "...and a read that succeeds costs no count (got %u, want %u)", uart_get_errcnt(0),
                base);

        int selto_before = n_selto;
        memcpy(bad, frame, sizeof bad);
        bad[CMD_CRC_BYTE] ^= 0xffu;
        base = uart_get_errcnt(0);
        CHECK(reply_read(0, bad, CMD_CRC_BYTE + 1) != 0,
                "a reply whose CMD-frame CRC is wrong fails the read");
        CHECK(n_selto == selto_before,
                "...at the CRC check and NOT on silence — a silent bus returns "
                "from the select-timeout path BEFORE either CRC check, which is "
                "why the parts above cannot pin this (%d `no reply to` line(s), want 0)",
                n_selto - selto_before);
        CHECK(uart_get_errcnt(0) == base + 1,
                "...and costs EXACTLY ONE count (got %u, want %u): the tally is "
                "per failed READ, so a per-error-site increment inside "
                "uart_read_fifo_with_ret would double-count every CRC failure",
                uart_get_errcnt(0), base + 1);

        selto_before = n_selto;
        memcpy(bad, frame, sizeof bad);
        bad[DATA_CRC_BYTE] ^= 0xffu;
        base = uart_get_errcnt(0);
        CHECK(reply_read(0, bad, FRAME_LEN) != 0,
                "a reply whose DATA-frame CRC is wrong fails the read");
        CHECK(n_selto == selto_before,
                "...at that check and NOT on silence (%d `no reply to` line(s), want 0)",
                n_selto - selto_before);
        CHECK(uart_get_errcnt(0) == base + 1,
                "...and costs EXACTLY ONE count too (got %u, want %u)", uart_get_errcnt(0),
                base + 1);
    }

    {
        uint8_t frame[FRAME_LEN];
        int     n = capture_frame(0, frame);
        CHECK(n == FRAME_LEN,
                "captured a %d-byte reply for the resync cases "
                "(got %d)",
                FRAME_LEN, n);

        int      resync_before = n_resync;
        uint32_t base          = uart_get_errcnt(0);
        CHECK(offset_reply_read(0, frame, FRAME_LEN, 3) == 0,
                "a reply with 3 junk bytes in front of the header still PARSES — "
                "the header hunt finds 0x5a at offset 3, memmoves it down and the "
                "read completes");
        CHECK(n_resync == resync_before + 1,
                "...and says so exactly once (%d `out of sync` line(s), want 1)",
                n_resync - resync_before);
        CHECK(uart_get_errcnt(0) == base,
                "...and costs NO failed-read count (got %u, want %u): a recovered "
                "resync is not a failure, so it must not reach estats CRC[]/COMCRC[]",
                uart_get_errcnt(0), base);

        resync_before = n_resync;
        base          = uart_get_errcnt(0);
        CHECK(offset_reply_read(0, frame, FRAME_LEN, 1) == 0,
                "a 1-byte lead parses too (the smallest non-zero offset)");
        CHECK(n_resync == resync_before + 1, "...one line for it as well (%d, want 1)",
                n_resync - resync_before);
        CHECK(uart_get_errcnt(0) == base, "...and still no count");

        (void)reply_read(0, frame, FRAME_LEN);
        n_resync         = 0;
        n_resync_recover = 0;
        for (int i = 0; i < BURST; i++)
            (void)offset_reply_read(0, frame, FRAME_LEN, 2);
        CHECK(n_resync <= UART_SILENT_LOG_MAX,
                "%d consecutive resyncs print at most %d WARN line(s) (got %d) — "
                "a channel that is permanently misframed cannot flood a 2.7 MB "
                "rootfs",
                BURST, UART_SILENT_LOG_MAX, n_resync);
        CHECK(n_resync > 0,
                "...but the first ones DO print (got %d, want >0): the cap must "
                "suppress, never silence",
                n_resync);

        n_resync_recover       = 0;
        last_resync_suppressed = 0;
        CHECK(reply_read(0, frame, FRAME_LEN) == 0,
                "a correctly framed reply after the burst parses");
        CHECK(n_resync_recover == 1, "...and closes the run with ONE summary line (got %d, want 1)",
                n_resync_recover);
        CHECK(last_resync_suppressed == (unsigned)(BURST - UART_SILENT_LOG_MAX),
                "...naming how many were withheld (got %u, want %d), so the log "
                "still tells a flake from a storm",
                last_resync_suppressed, BURST - UART_SILENT_LOG_MAX);
    }

    printf("uart silent-bus ERR bound (%s, %d ch): %s\n", BOARD_NAME, CHANNELS,
            fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
